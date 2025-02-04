// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/linux/webview_linux.h"

#include "base/random.h"
#include "webview/platform/linux/webview_linux_webkitgtk.h"

namespace Webview {

Available Availability() {
	return WebKitGTK::Availability();
}

bool SupportsEmbedAfterCreate() {
	return true;
}

bool SeparateStorageIdSupported() {
	return true;
}

std::unique_ptr<Interface> CreateInstance(Config config) {
	return WebKitGTK::CreateInstance(std::move(config));
}

std::string GenerateStorageToken() {
	constexpr auto kSize = 16;
	auto result = std::string(kSize, ' ');
	base::RandomFill(result.data(), result.size());
	return result;
}

void ClearStorageDataByToken(const std::string &token) {
}

} // namespace Webview
