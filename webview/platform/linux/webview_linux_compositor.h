// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include <QtWaylandCompositor/QWaylandQuickCompositor>

class QQuickWidget;

namespace Webview {

class Compositor : public QWaylandQuickCompositor {
public:
	Compositor(const QByteArray &socketName = {});

	void setWidget(QQuickWidget *widget);

private:
	struct Private;
	const std::unique_ptr<Private> _private;
};

} // namespace Webview
