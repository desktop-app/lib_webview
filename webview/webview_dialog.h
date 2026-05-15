// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include "ui/platform/ui_platform_utility.h"

#include <QtCore/QString>
#include <QtCore/QRect>

#include <optional>
#include <vector>

class QWidget;

namespace Webview {

struct PopupArgs {
	struct Button {
		enum class Type {
			Default,
			Ok,
			Close,
			Cancel,
			Destructive,
		};
		QString id;
		QString text;
		Type type = Type::Default;
	};

	QWidget *parent = nullptr;
	std::optional<QRect> anchorGeometry;
	Ui::Platform::ForeignParent transientParent;
	QString title;
	QString text;
	std::optional<QString> value;
	std::vector<Button> buttons;
	bool ignoreFloodCheck = false;
};
struct PopupResult {
	std::optional<QString> id;
	std::optional<QString> value;
};
[[nodiscard]] PopupResult ShowBlockingPopup(PopupArgs &&args);
bool CloseBlockingPopup();

struct DialogArgs;
struct DialogResult;

[[nodiscard]] DialogResult DefaultDialogHandler(DialogArgs &&args);

} // namespace Webview
