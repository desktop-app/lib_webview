// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include "webview/details/webview_wrap.h"

#include <QtCore/QString>

namespace Webview {

class Window final {
public:
	explicit Window(const QString &url);

private:
	details::Wrap _wrap;

};

} // namespace Webview
