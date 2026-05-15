// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/webview_dialog.h"

#include "webview/webview_interface.h"
#include "ui/widgets/separate_panel.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/integration.h"
#include "ui/qt_object_factory.h"
#include "base/invoke_queued.h"
#include "base/unique_qptr.h"
#include "base/integration.h"
#include "styles/style_widgets.h"
#include "styles/style_layers.h"

#include <QtCore/QUrl>
#include <QtCore/QString>
#include <QtCore/QEventLoop>
#include <QtCore/QPointer>
#include <QtCore/QCoreApplication>

#include <memory>

namespace Webview {
namespace {

constexpr auto kPopupsQuicklyLimit = 3;
constexpr auto kPopupsQuicklyDelay = 8 * crl::time(1000);

bool InBlockingPopup/* = false*/;
int PopupsShownQuickly/* = 0*/;
crl::time PopupLastShown/* = 0*/;
QPointer<Ui::SeparatePanel> CurrentBlockingPopup;
bool CloseBlockingPopupRequested/* = false*/;

struct AsyncPopupState {
	PopupResult result;
	Fn<void(PopupResult)> done;
	base::unique_qptr<Ui::SeparatePanel> widget;
	bool finished = false;
};

[[nodiscard]] DialogResult DialogResultFromPopup(
		DialogType,
		PopupResult &&result) {
	return {
		.text = (result.id == "cancel"
			? std::string()
			: result.value.value_or(QString()).toStdString()),
		.accepted = (result.id == "ok" || result.value.has_value()),
	};
}

void FinishAsyncPopup(
		const std::shared_ptr<AsyncPopupState> &state,
		bool destroyWidget) {
	if (state->finished) {
		return;
	}
	state->finished = true;
	auto result = std::move(state->result);
	const auto widget = state->widget.release();
	if (widget && destroyWidget) {
		widget->deleteLater();
	}
	InBlockingPopup = false;
	CurrentBlockingPopup = nullptr;
	CloseBlockingPopupRequested = false;
	PopupLastShown = crl::now();
	if (state->done) {
		state->done(std::move(result));
	}
}

} // namespace

bool CloseBlockingPopup() {
	if (CurrentBlockingPopup) {
		CurrentBlockingPopup->hideGetDuration();
		return true;
	} else if (InBlockingPopup) {
		CloseBlockingPopupRequested = true;
		return true;
	}
	return false;
}

PopupResult ShowBlockingPopup(PopupArgs &&args) {
	if (InBlockingPopup) {
		return {};
	}
	InBlockingPopup = true;
	const auto guard = gsl::finally([] {
		InBlockingPopup = false;
		CurrentBlockingPopup = nullptr;
		CloseBlockingPopupRequested = false;
	});

	if (!args.ignoreFloodCheck) {
		const auto now = crl::now();
		if (!PopupLastShown || PopupLastShown + kPopupsQuicklyDelay <= now) {
			PopupsShownQuickly = 1;
		} else if (++PopupsShownQuickly > kPopupsQuicklyLimit) {
			return {};
		}
	}
	const auto timeguard = gsl::finally([] {
		PopupLastShown = crl::now();
	});

	// This fixes animations in a nested event loop.
	base::Integration::Instance().enterFromEventLoop([] {});

	auto result = PopupResult();
	auto context = QObject();

	QEventLoop loop;
	auto running = true;
	auto widget = base::unique_qptr<Ui::SeparatePanel>();
	InvokeQueued(&context, [&] {
		auto separatePanelArgs = Ui::SeparatePanelArgs{
			.parent = args.parent,
		};
		separatePanelArgs.anchorGeometry = args.anchorGeometry;
		separatePanelArgs.transientParent = args.transientParent;
		widget = base::make_unique_q<Ui::SeparatePanel>(
			std::move(separatePanelArgs));
		const auto raw = widget.get();

		raw->setWindowFlag(Qt::WindowStaysOnTopHint, false);
		raw->setAttribute(Qt::WA_DeleteOnClose, false);
		raw->setAttribute(Qt::WA_ShowModal, true);

		const auto titleHeight = args.title.isEmpty()
			? st::separatePanelNoTitleHeight
			: st::separatePanelTitleHeight;
		if (!args.title.isEmpty()) {
			raw->setTitle(rpl::single(args.title));
		}
		raw->setTitleHeight(titleHeight);
		auto layout = base::make_unique_q<Ui::VerticalLayout>(raw);
		CurrentBlockingPopup = raw;
		const auto skip = st::boxDividerHeight;
		const auto container = layout.get();
		const auto addedRightPadding = args.title.isEmpty()
			? (st::separatePanelClose.width - st::boxRowPadding.right())
			: 0;
		const auto label = container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				rpl::single(args.text),
				st::boxLabel),
			st::boxRowPadding + QMargins(0, 0, addedRightPadding, 0));
		label->resizeToWidth(st::boxWideWidth
			- st::boxRowPadding.left()
			- st::boxRowPadding.right()
			- addedRightPadding);
		const auto input = args.value
			? container->add(
				object_ptr<Ui::InputField>(
					container,
					st::defaultInputField,
					rpl::single(QString()),
					*args.value),
				st::boxRowPadding + QMargins(0, 0, 0, skip))
			: nullptr;
		const auto buttonPadding = st::webviewDialogPadding;
		const auto buttons = container->add(
			object_ptr<Ui::RpWidget>(container),
			QMargins(
				buttonPadding.left(),
				buttonPadding.top(),
				buttonPadding.left(),
				buttonPadding.bottom()));
		const auto list = buttons->lifetime().make_state<
			std::vector<not_null<Ui::RoundButton*>>
		>();
		list->reserve(args.buttons.size());
		for (const auto &descriptor : args.buttons) {
			using Type = PopupArgs::Button::Type;
			const auto text = [&] {
				const auto integration = &Ui::Integration::Instance();
				switch (descriptor.type) {
				case Type::Default: return descriptor.text;
				case Type::Ok: return integration->phraseButtonOk();
				case Type::Close: return integration->phraseButtonClose();
				case Type::Cancel: return integration->phraseButtonCancel();
				case Type::Destructive: return descriptor.text;
				}
				Unexpected("Button type in blocking popup.");
			}();
			const auto button = Ui::CreateChild<Ui::RoundButton>(
				buttons,
				rpl::single(text),
				(descriptor.type != Type::Destructive
					? st::webviewDialogButton
					: st::webviewDialogDestructiveButton));
			button->setClickedCallback([=, &result, id = descriptor.id]{
				result.id = id;
				if (input) {
					result.value = input->getLastText();
				}
				raw->hideGetDuration();
			});
			list->push_back(button);
		}

		buttons->resizeToWidth(st::boxWideWidth - 2 * buttonPadding.left());
		buttons->widthValue(
		) | rpl::on_next([=](int width) {
			const auto count = list->size();
			const auto skip = st::webviewDialogPadding.right();
			auto buttonsWidth = 0;
			for (const auto &button : *list) {
				buttonsWidth += button->width() + (buttonsWidth ? skip : 0);
			}
			const auto vertical = (count > 1) && (buttonsWidth > width);
			const auto single = st::webviewDialogSubmit.height;
			auto top = 0;
			auto right = 0;
			for (const auto &button : *list) {
				button->moveToRight(right, top, width);
				if (vertical) {
					top += single + skip;
				} else {
					right += button->width() + skip;
				}
			}
			const auto height = (top > 0) ? (top - skip) : single;
			if (buttons->height() != height) {
				buttons->resize(buttons->width(), height);
			}
		}, buttons->lifetime());

		container->resizeToWidth(st::boxWideWidth);

		container->heightValue(
		) | rpl::on_next([=](int height) {
			raw->setInnerSize({ st::boxWideWidth, titleHeight + height });
		}, container->lifetime());

		if (input) {
			input->selectAll();
			input->setFocusFast();
			const auto submitted = [=, &result] {
				result.value = input->getLastText();
				raw->hideGetDuration();
			};
			input->submits(
			) | rpl::on_next(submitted, input->lifetime());
		}
		container->events(
		) | rpl::on_next([=](not_null<QEvent*> event) {
			if (input && event->type() == QEvent::FocusIn) {
				input->setFocus();
			}
		}, container->lifetime());

		raw->closeRequests() | rpl::on_next([=] {
			raw->hideGetDuration();
		}, raw->lifetime());

		const auto finish = [&] {
			if (running) {
				running = false;
				loop.quit();
			}
		};
		QObject::connect(raw, &QObject::destroyed, finish);
		raw->closeEvents() | rpl::on_next(finish, raw->lifetime());

		raw->showInner(std::move(layout));
		if (CloseBlockingPopupRequested) {
			raw->hideGetDuration();
		}
	});
	loop.exec(QEventLoop::DialogExec);
	widget = nullptr;

	return result;
}

DialogResult DefaultDialogHandler(DialogArgs &&args) {
	auto buttons = std::vector<PopupArgs::Button>();
	buttons.push_back({
		.id = "ok",
		.type = PopupArgs::Button::Type::Ok,
	});
	if (args.type != DialogType::Alert) {
		buttons.push_back({
			.id = "cancel",
			.type = PopupArgs::Button::Type::Cancel,
		});
	}
	const auto result = ShowBlockingPopup({
		.parent = args.parent,
		.anchorGeometry = args.anchorGeometry,
		.transientParent = args.transientParent,
		.title = QUrl(QString::fromStdString(args.url)).host(),
		.text = QString::fromStdString(args.text),
		.value = (args.type == DialogType::Prompt
			? QString::fromStdString(args.value)
			: std::optional<QString>()),
		.buttons = std::move(buttons),
	});
	return {
		.text = (result.id == "cancel"
			? std::string()
			: result.value.value_or(QString()).toStdString()),
		.accepted = (result.id == "ok" || result.value.has_value()),
	};
}

void ShowPopupAsync(
		PopupArgs &&popup,
		Fn<void(PopupResult)> done,
		bool modal) {
	if (InBlockingPopup) {
		if (done) {
			done({});
		}
		return;
	}
	InBlockingPopup = true;

	if (!popup.ignoreFloodCheck) {
		const auto now = crl::now();
		if (!PopupLastShown || PopupLastShown + kPopupsQuicklyDelay <= now) {
			PopupsShownQuickly = 1;
		} else if (++PopupsShownQuickly > kPopupsQuicklyLimit) {
			InBlockingPopup = false;
			CloseBlockingPopupRequested = false;
			if (done) {
				done({});
			}
			return;
		}
	}

	const auto state = std::make_shared<AsyncPopupState>();
	state->done = std::move(done);
	const auto context = QCoreApplication::instance();
	if (!context) {
		FinishAsyncPopup(state, false);
		return;
	}
	InvokeQueued(context, [state, popup = std::move(popup), modal]() mutable {
		auto separatePanelArgs = Ui::SeparatePanelArgs{
			.parent = popup.parent,
		};
		separatePanelArgs.anchorGeometry = popup.anchorGeometry;
		separatePanelArgs.transientParent = popup.transientParent;
		state->widget = base::make_unique_q<Ui::SeparatePanel>(
			std::move(separatePanelArgs));
		const auto raw = state->widget.get();

		raw->setWindowFlag(Qt::WindowStaysOnTopHint, false);
		raw->setAttribute(Qt::WA_DeleteOnClose, false);
		raw->setAttribute(Qt::WA_ShowModal, modal);

		const auto titleHeight = popup.title.isEmpty()
			? st::separatePanelNoTitleHeight
			: st::separatePanelTitleHeight;
		if (!popup.title.isEmpty()) {
			raw->setTitle(rpl::single(popup.title));
		}
		raw->setTitleHeight(titleHeight);
		auto layout = base::make_unique_q<Ui::VerticalLayout>(raw);
		CurrentBlockingPopup = raw;
		const auto skip = st::boxDividerHeight;
		const auto container = layout.get();
		const auto addedRightPadding = popup.title.isEmpty()
			? (st::separatePanelClose.width - st::boxRowPadding.right())
			: 0;
		const auto label = container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				rpl::single(popup.text),
				st::boxLabel),
			st::boxRowPadding + QMargins(0, 0, addedRightPadding, 0));
		label->resizeToWidth(st::boxWideWidth
			- st::boxRowPadding.left()
			- st::boxRowPadding.right()
			- addedRightPadding);
		const auto input = popup.value
			? container->add(
				object_ptr<Ui::InputField>(
					container,
					st::defaultInputField,
					rpl::single(QString()),
					*popup.value),
				st::boxRowPadding + QMargins(0, 0, 0, skip))
			: nullptr;
		const auto buttonPadding = st::webviewDialogPadding;
		const auto buttons = container->add(
			object_ptr<Ui::RpWidget>(container),
			QMargins(
				buttonPadding.left(),
				buttonPadding.top(),
				buttonPadding.left(),
				buttonPadding.bottom()));
		const auto list = buttons->lifetime().make_state<
			std::vector<not_null<Ui::RoundButton*>>
		>();
		list->reserve(popup.buttons.size());
		for (const auto &descriptor : popup.buttons) {
			using Type = PopupArgs::Button::Type;
			const auto text = [&] {
				const auto integration = &Ui::Integration::Instance();
				switch (descriptor.type) {
				case Type::Default: return descriptor.text;
				case Type::Ok: return integration->phraseButtonOk();
				case Type::Close: return integration->phraseButtonClose();
				case Type::Cancel: return integration->phraseButtonCancel();
				case Type::Destructive: return descriptor.text;
				}
				Unexpected("Button type in async popup.");
			}();
			const auto button = Ui::CreateChild<Ui::RoundButton>(
				buttons,
				rpl::single(text),
				(descriptor.type != Type::Destructive
					? st::webviewDialogButton
					: st::webviewDialogDestructiveButton));
			button->setClickedCallback([=, id = descriptor.id] {
				state->result.id = id;
				if (input) {
					state->result.value = input->getLastText();
				}
				raw->hideGetDuration();
			});
			list->push_back(button);
		}

		buttons->resizeToWidth(st::boxWideWidth - 2 * buttonPadding.left());
		buttons->widthValue(
		) | rpl::on_next([=](int width) {
			const auto count = list->size();
			const auto skip = st::webviewDialogPadding.right();
			auto buttonsWidth = 0;
			for (const auto &button : *list) {
				buttonsWidth += button->width() + (buttonsWidth ? skip : 0);
			}
			const auto vertical = (count > 1) && (buttonsWidth > width);
			const auto single = st::webviewDialogSubmit.height;
			auto top = 0;
			auto right = 0;
			for (const auto &button : *list) {
				button->moveToRight(right, top, width);
				if (vertical) {
					top += single + skip;
				} else {
					right += button->width() + skip;
				}
			}
			const auto height = (top > 0) ? (top - skip) : single;
			if (buttons->height() != height) {
				buttons->resize(buttons->width(), height);
			}
		}, buttons->lifetime());

		container->resizeToWidth(st::boxWideWidth);

		container->heightValue(
		) | rpl::on_next([=](int height) {
			raw->setInnerSize({ st::boxWideWidth, titleHeight + height });
		}, container->lifetime());

		if (input) {
			input->selectAll();
			input->setFocusFast();
			const auto submitted = [=] {
				state->result.value = input->getLastText();
				raw->hideGetDuration();
			};
			input->submits(
			) | rpl::on_next(submitted, input->lifetime());
		}
		container->events(
		) | rpl::on_next([=](not_null<QEvent*> event) {
			if (input && event->type() == QEvent::FocusIn) {
				input->setFocus();
			}
		}, container->lifetime());

		raw->closeRequests() | rpl::on_next([=] {
			raw->hideGetDuration();
		}, raw->lifetime());

		QObject::connect(raw, &QObject::destroyed, [state] {
			FinishAsyncPopup(state, false);
		});
		raw->closeEvents(
		) | rpl::on_next([state] {
			FinishAsyncPopup(state, true);
		}, raw->lifetime());

		raw->showInner(std::move(layout));
		if (CloseBlockingPopupRequested) {
			raw->hideGetDuration();
		}
	});
}

void DefaultDialogHandlerAsync(
		DialogArgs &&args,
		Fn<void(DialogResult)> done,
		bool modal) {
	auto buttons = std::vector<PopupArgs::Button>();
	buttons.push_back({
		.id = "ok",
		.type = PopupArgs::Button::Type::Ok,
	});
	if (args.type != DialogType::Alert) {
		buttons.push_back({
			.id = "cancel",
			.type = PopupArgs::Button::Type::Cancel,
		});
	}
	const auto type = args.type;
	ShowPopupAsync({
		.parent = args.parent,
		.anchorGeometry = args.anchorGeometry,
		.transientParent = args.transientParent,
		.title = QUrl(QString::fromStdString(args.url)).host(),
		.text = QString::fromStdString(args.text),
		.value = (args.type == DialogType::Prompt
			? QString::fromStdString(args.value)
			: std::optional<QString>()),
		.buttons = std::move(buttons),
	}, [=, done = std::move(done)](PopupResult result) mutable {
		auto dialogResult = DialogResultFromPopup(type, std::move(result));
		if (done) {
			done(std::move(dialogResult));
		}
	}, modal);
}

} // namespace Webview
