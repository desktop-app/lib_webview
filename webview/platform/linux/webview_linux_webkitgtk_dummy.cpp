// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/linux/webview_linux_webkitgtk.h"

namespace Webview::WebKitGTK {

Available Availability() {
	return Available{};
}

std::unique_ptr<Interface> CreateInstance(Config config) {
	return nullptr;
}

int Exec() {
	return 1;
}

void SetSocketPath(const std::string &socketPath) {
}

} // namespace Webview::WebKitGTK
