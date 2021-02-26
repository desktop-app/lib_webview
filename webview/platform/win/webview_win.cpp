// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/win/webview_win.h"

#include "webview/platform/win/webview_windows_edge_chromium.h"
#include "webview/platform/win/webview_windows_edge_html.h"

namespace Webview {

bool Supported() {
	return EdgeChromium::Supported() || EdgeHtml::Supported();
}

bool SupportsEmbedAfterCreate() {
	return !EdgeChromium::Supported();
}

std::unique_ptr<Interface> CreateInstance(Config config) {
	if (!Supported()) {
		return nullptr;
	}
	if (auto result = EdgeChromium::CreateInstance(config)) {
		return result;
	}
	return EdgeHtml::CreateInstance(std::move(config));
}

} // namespace Webview
