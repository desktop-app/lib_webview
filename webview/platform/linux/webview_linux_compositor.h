// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include <QtCore/QObject>

#ifdef QT_WAYLANDCOMPOSITOR_LIB
#include <QtWaylandCompositor/QWaylandCompositor>
#endif // QT_WAYLANDCOMPOSITOR_LIB

namespace Webview {

#ifdef QT_WAYLANDCOMPOSITOR_LIB
class Compositor : public QWaylandCompositor {
public:
	Compositor(const QByteArray &socketName = {}, bool gpu = true);
	~Compositor();

	[[nodiscard]] QWidget *createWidget(QWidget *parent);
	void setBackground(QColor color);

private:
	class Output;
	class Display;
	class Chrome;

	struct Private;
	const std::unique_ptr<Private> _private;
};
#else // QT_WAYLANDCOMPOSITOR_LIB
class Compositor : public QObject {
public:
	Compositor(const QByteArray &socketName = {}, bool gpu = true) {}
	QString socketName() { return {}; }
};
#endif // !QT_WAYLANDCOMPOSITOR_LIB

} // namespace Webview
