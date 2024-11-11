// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include "base/basic_types.h"

#include <QByteArray>
#include <QString>
#include <QColor>

namespace Webview {

struct StorageId {
	QString path;
	QByteArray token;

	explicit operator bool() const {
		return !path.isEmpty() && !token.isEmpty();
	}
};

[[nodiscard]] inline QByteArray LegacyStorageIdToken() {
	return "<legacy>"_q;
}

struct ThemeParams {
	QColor bodyBg;
	QColor titleBg;
	QColor scrollBg;
	QColor scrollBgOver;
	QColor scrollBarBg;
	QColor scrollBarBgOver;
	QByteArray json;
};

} // namespace Webview
