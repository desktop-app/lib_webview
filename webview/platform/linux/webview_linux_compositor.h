// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include <QtCore/QObject>

#if defined QT_QUICKWIDGETS_LIB && defined QT_WAYLANDCOMPOSITOR_LIB
#include <QtWaylandCompositor/qtwaylandcompositor-config.h>

#if QT_CONFIG(wayland_compositor_quick)
#include <QtWaylandCompositor/QWaylandQuickCompositor>

#define DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR

class QQuickWidget;

namespace Webview {

class Compositor : public QWaylandQuickCompositor {
public:
	Compositor(const QByteArray &socketName = {});

	void setWidget(QQuickWidget *widget);

private:
	class Output;
	class Chrome;

	struct Private;
	const std::unique_ptr<Private> _private;
};

} // namespace Webview
#endif // QT_CONFIG(wayland_compositor_quick)
#endif // QT_QUICKWIDGETS_LIB && QT_WAYLANDCOMPOSITOR_LIB

#ifndef DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
namespace Webview {

class Compositor : public QObject {
public:
	Compositor(const QByteArray &socketName = {}) {}
	QString socketName() { return {}; }
};

} // namespace Webview
#endif // !DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
