// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/linux/webview_linux.h"

#include "webview/platform/linux/webview_linux_webkit2gtk.h"

namespace Webview {

Available Availability() {
#ifndef DESKTOP_APP_DISABLE_GTK_INTEGRATION
	return WebKit2Gtk::Availability();
#else // !DESKTOP_APP_DISABLE_GTK_INTEGRATION
	return false;
#endif // DESKTOP_APP_DISABLE_GTK_INTEGRATION
}

bool SupportsEmbedAfterCreate() {
	return true;
}

std::unique_ptr<Interface> CreateInstance(Config config) {
#ifndef DESKTOP_APP_DISABLE_GTK_INTEGRATION
	return WebKit2Gtk::CreateInstance(std::move(config));
#else // !DESKTOP_APP_DISABLE_GTK_INTEGRATION
	return nullptr;
#endif // DESKTOP_APP_DISABLE_GTK_INTEGRATION
}

} // namespace Webview
