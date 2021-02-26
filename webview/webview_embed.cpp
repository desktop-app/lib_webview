// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/webview_embed.h"

#include "webview/details/webview_wrap.h"

namespace Webview {

Window::Window(const QString &url) {
	_wrap.setSize(480, 320, details::Hint::None);
	_wrap.navigate(url.toStdString());
}

} // namespace Webview
