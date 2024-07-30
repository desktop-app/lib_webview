// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include "base/unique_qptr.h"
#include "webview/webview_interface.h"

class QWindow;

namespace Webview {

[[nodiscard]] base::unique_qptr<QWindow> MakeFramelessWindow();

} // namespace Webview