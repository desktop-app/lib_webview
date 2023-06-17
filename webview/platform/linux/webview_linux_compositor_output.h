// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include "webview/platform/linux/webview_linux_compositor.h"

#include <QtQml/QQmlComponent>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QQuickItem>
#include <QtWaylandCompositor/QWaylandQuickOutput>
#include <QtWaylandCompositor/QWaylandShellSurface>

namespace Webview {

class Compositor::Output : public QWaylandQuickOutput {
	Q_OBJECT
public:
	Output(
			QQmlEngine *engine,
			QWaylandCompositor *compositor,
			QWaylandShellSurface *shellSurface,
			QQuickWindow *window = nullptr) {
		setCompositor(compositor);
		setWindow(window ? window : &_ownedWindow.emplace());
		setScaleFactor(this->window()->devicePixelRatio());
		setSizeFollowsWindow(true);
		shellSurface->setProperty("output", QVariant::fromValue(this));
		QCoreApplication::processEvents();
		_chrome.reset(
			qobject_cast<QQuickItem*>(
				QQmlComponent(
					engine,
					QUrl("qrc:///webview/Chrome.qml")
				).createWithInitialProperties({
					{
						"output",
						QVariant::fromValue(this)
					},
					{
						"shellSurface",
						QVariant::fromValue(shellSurface)
					},
					{
						"windowFollowsSize",
						!window
					},
				})));
		_chrome->setParentItem(quickWindow()->contentItem());
	}

	QQuickWindow *quickWindow() {
		return qobject_cast<QQuickWindow*>(window());
	}

Q_SIGNALS:
	void surfaceCompleted();

private:
	std::optional<QQuickWindow> _ownedWindow;
	std::unique_ptr<QQuickItem> _chrome;
};

} // namespace Webview
