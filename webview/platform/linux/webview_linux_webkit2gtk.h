// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include "webview/platform/linux/webview_linux.h"

namespace Webview::WebKit2Gtk {

[[nodiscard]] Available Availability();
[[nodiscard]] bool ProvidesQWidget();
[[nodiscard]] std::unique_ptr<Interface> CreateInstance(Config config);

int Exec();
void SetSocketPath(const std::string &socketPath);

} // namespace Webview::WebKit2Gtk
