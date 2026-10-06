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
#include "base/algorithm.h"
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
#include <QtWidgets/QWidget>

#include <memory>

namespace Webview {
namespace {

constexpr auto kPopupsQuicklyLimit = 3;
constexpr auto kPopupsQuicklyDelay = 8 * crl::time(1000);

bool InBlockingPopup/* = false*/;
bool InBlockingPopupLoop/* = false*/;
int PopupsShownQuickly/* = 0*/;
crl::time PopupLastShown/* = 0*/;
std::vector<Fn<void()>> BlockingPopupFinishCallbacks;

struct AsyncPopupState {
	PopupResult result;
	Fn<void(PopupResult)> done;
	base::unique_qptr<Ui::SeparatePanel> widget;
	bool closeRequested = false;
	bool finished = false;
};

[[nodiscard]] PopupArgs DialogPopupArgs(const DialogArgs &args) {
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
	return {
		.parent = args.parent,
		.transientParent = args.transientParent,
		.title = QUrl(QString::fromStdString(args.url)).host(),
		.text = QString::fromStdString(args.text),
		.value = (args.type == DialogType::Prompt
			? QString::fromStdString(args.value)
			: std::optional<QString>()),
		.buttons = std::move(buttons),
	};
}

[[nodiscard]] DialogResult DialogResultFromPopup(PopupResult &&result) {
	if (result.id == "cancel") {
		return {};
	}
	return {
		.text = result.value.value_or(QString()).toStdString(),
		.accepted = (result.id == "ok" || result.value.has_value()),
	};
}

[[nodiscard]] bool PopupsShownTooQuickly() {
	const auto now = crl::now();
	if (!PopupLastShown || PopupLastShown + kPopupsQuicklyDelay <= now) {
		PopupsShownQuickly = 1;
		return false;
	}
	return (++PopupsShownQuickly > kPopupsQuicklyLimit);
}

[[nodiscard]] base::unique_qptr<Ui::SeparatePanel> CreatePopupPanel(
		const PopupArgs &args,
		QWidget *parent,
		bool modal,
		not_null<PopupResult*> result) {
	auto separatePanelArgs = Ui::SeparatePanelArgs{
		.parent = parent,
	};
	separatePanelArgs.transientParent = args.transientParent;
	auto panel = base::make_unique_q<Ui::SeparatePanel>(
		std::move(separatePanelArgs));
	const auto raw = panel.get();

	raw->setWindowFlag(Qt::WindowStaysOnTopHint, false);
	raw->setAttribute(Qt::WA_DeleteOnClose, false);
	raw->setAttribute(Qt::WA_ShowModal, modal);

	const auto titleHeight = args.title.isEmpty()
		? st::separatePanelNoTitleHeight
		: st::separatePanelTitleHeight;
	if (!args.title.isEmpty()) {
		raw->setTitle(rpl::single(args.title));
	}
	raw->setTitleHeight(titleHeight);
	auto layout = base::make_unique_q<Ui::VerticalLayout>(raw);
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
			Unexpected("Button type in webview popup.");
		}();
		const auto button = Ui::CreateChild<Ui::RoundButton>(
			buttons,
			rpl::single(text),
			(descriptor.type != Type::Destructive
				? st::webviewDialogButton
				: st::webviewDialogDestructiveButton));
		button->setClickedCallback([=, id = descriptor.id] {
			result->id = id;
			if (input) {
				result->value = input->getLastText();
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
			result->value = input->getLastText();
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

	raw->showInner(std::move(layout));
	return panel;
}

void FlushBlockingPopupFinishCallbacks() {
	if (InBlockingPopupLoop || BlockingPopupFinishCallbacks.empty()) {
		return;
	}
	const auto invoke = [] {
		if (InBlockingPopupLoop) {
			return;
		}
		for (const auto &callback : base::take(BlockingPopupFinishCallbacks)) {
			callback();
		}
	};
	if (const auto context = QCoreApplication::instance()) {
		InvokeQueued(context, invoke);
	} else {
		invoke();
	}
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
	PopupLastShown = crl::now();
	if (state->done) {
		state->done(std::move(result));
	}
}

} // namespace

bool InsideBlockingPopup() {
	return InBlockingPopupLoop;
}

void RunWhenBlockingPopupFinished(Fn<void()> callback) {
	if (!callback) {
		return;
	} else if (!InBlockingPopupLoop) {
		callback();
		return;
	}
	BlockingPopupFinishCallbacks.push_back(std::move(callback));
}

PopupResult ShowBlockingPopup(PopupArgs &&args) {
	if (InBlockingPopup) {
		return {};
	}
	InBlockingPopup = InBlockingPopupLoop = true;
	const auto guard = gsl::finally([] {
		InBlockingPopup = InBlockingPopupLoop = false;
		FlushBlockingPopupFinishCallbacks();
	});

	if (!args.ignoreFloodCheck && PopupsShownTooQuickly()) {
		return {};
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
		widget = CreatePopupPanel(args, args.parent, true, &result);
		const auto raw = widget.get();
		const auto finish = [&] {
			if (running) {
				running = false;
				loop.quit();
			}
		};
		QObject::connect(raw, &QObject::destroyed, finish);
		raw->closeEvents() | rpl::on_next(finish, raw->lifetime());
	});
	loop.exec(QEventLoop::DialogExec);
	widget = nullptr;

	return result;
}

DialogResult DefaultDialogHandler(DialogArgs &&args) {
	return DialogResultFromPopup(ShowBlockingPopup(DialogPopupArgs(args)));
}

Fn<void()> ShowPopupAsync(
		PopupArgs &&popup,
		Fn<void(PopupResult)> done,
		bool modal) {
	if (InBlockingPopup) {
		if (done) {
			done({});
		}
		return nullptr;
	}
	InBlockingPopup = true;

	if (!popup.ignoreFloodCheck && PopupsShownTooQuickly()) {
		InBlockingPopup = false;
		if (done) {
			done({});
		}
		return nullptr;
	}

	const auto state = std::make_shared<AsyncPopupState>();
	state->done = std::move(done);
	const auto context = QCoreApplication::instance();
	if (!context) {
		FinishAsyncPopup(state, false);
		return nullptr;
	}
	const auto parent = QPointer<QWidget>(popup.parent);
	const auto parentRequired = (popup.parent != nullptr);
	InvokeQueued(context, [
		state,
		popup = std::move(popup),
		modal,
		parent,
		parentRequired
	]() mutable {
		if (parentRequired && !parent) {
			FinishAsyncPopup(state, false);
			return;
		}
		state->widget = CreatePopupPanel(
			popup,
			parent.data(),
			modal,
			&state->result);
		const auto raw = state->widget.get();
		QObject::connect(raw, &QObject::destroyed, [state] {
			FinishAsyncPopup(state, false);
		});
		raw->closeEvents(
		) | rpl::on_next([state] {
			FinishAsyncPopup(state, true);
		}, raw->lifetime());
		if (state->closeRequested) {
			raw->hideGetDuration();
		}
	});
	return [weak = std::weak_ptr(state)] {
		if (const auto state = weak.lock()) {
			if (state->widget) {
				state->widget->hideGetDuration();
			} else {
				state->closeRequested = true;
			}
		}
	};
}

Fn<void()> DefaultDialogHandlerAsync(
		DialogArgs &&args,
		Fn<void(DialogResult)> done,
		bool modal) {
	return ShowPopupAsync(DialogPopupArgs(args), [=](PopupResult result) {
		if (done) {
			done(DialogResultFromPopup(std::move(result)));
		}
	}, modal);
}

} // namespace Webview
