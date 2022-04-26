// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/win/webview_win.h"

#include "base/platform/base_platform_info.h"
#include "webview/platform/win/webview_windows_edge_chromium.h"
#include "webview/platform/win/webview_windows_edge_html.h"

namespace Webview {

Available Availability() {
	if (!Platform::IsWindows8Point1OrGreater()) {
		return Available{
			.error = Available::Error::OldWindows,
			.details = "Please update your system to Windows 8.1 or later.",
		};
	}
	if (EdgeHtml::Supported() || EdgeChromium::Supported()) {
		return Available{};
	}
	return Available{
		.error = Available::Error::NoWebview2,
		.details = "Please install Microsoft Edge Webview2 Runtime.",
	};
}

bool SupportsEmbedAfterCreate() {
	return EdgeHtml::Supported();
}

std::unique_ptr<Interface> CreateInstance(Config config) {
	if (!Platform::IsWindows8Point1OrGreater()) {
		return nullptr;
	} else if (Platform::IsWindows11OrGreater()) {
		if (auto result = EdgeChromium::CreateInstance(config)) {
			return result;
		}
	}
	if (auto result = EdgeHtml::CreateInstance(config)) {
		return result;
	}
	return EdgeChromium::CreateInstance(std::move(config));
}

} // namespace Webview
