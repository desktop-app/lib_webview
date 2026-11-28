// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include "base/basic_types.h"

#include <QtCore/QPointF>

class QMimeData;
class QWaylandCompositor;
class QWaylandSeat;
class QWaylandSurface;

struct wl_global;

namespace Webview {

class DataDeviceManager final {
public:
	DataDeviceManager(
		not_null<QWaylandCompositor*> compositor,
		not_null<QWaylandSeat*> seat,
		Fn<void()> dragStarted);
	~DataDeviceManager();

	[[nodiscard]] const wl_global *global() const;

	[[nodiscard]] bool dragging() const;
	[[nodiscard]] QMimeData *createDragData() const;
	[[nodiscard]] Qt::DropActions dragActions() const;
	[[nodiscard]] QWaylandSurface *dragIcon() const;
	[[nodiscard]] QPoint dragIconOffset() const;
	void dragFinished(Qt::DropAction result);
	void cancelDrag();

	void dragMove(
		QWaylandSurface *target,
		QPointF position,
		const QMimeData *data,
		Qt::DropActions actions);
	[[nodiscard]] Qt::DropAction dragAction() const;
	void dragLeave();
	Qt::DropAction drop(const QMimeData *data);

private:
	struct Private;
	const std::unique_ptr<Private> _private;

};

} // namespace Webview
