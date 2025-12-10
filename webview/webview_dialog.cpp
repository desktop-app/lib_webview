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

namespace Webview {
namespace {

constexpr auto kPopupsQuicklyLimit = 3;
constexpr auto kPopupsQuicklyDelay = 8 * crl::time(1000);

bool InBlockingPopup/* = false*/;
int PopupsShownQuickly/* = 0*/;
crl::time PopupLastShown/* = 0*/;

} // namespace

PopupResult ShowBlockingPopup(PopupArgs &&args) {
	if (InBlockingPopup) {
		return {};
	}
	InBlockingPopup = true;
	const auto guard = gsl::finally([] {
		InBlockingPopup = false;
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
		widget = base::make_unique_q<Ui::SeparatePanel>(Ui::SeparatePanelArgs{
			.parent = args.parent,
		});
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
			button->setTextTransform(Ui::RoundButton::TextTransform::NoTransform);
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

} // namespace Webview
