// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/linux/webview_linux_compositor.h"

#include "webview/platform/linux/webview_linux_compositor_output.h"
#include "base/flat_map.h"
#include "base/unique_qptr.h"

#include <QtQml/QQmlEngine>
#include <QtQuickWidgets/QQuickWidget>
#include <QtWaylandCompositor/QWaylandSurface>
#include <QtWaylandCompositor/QWaylandXdgShell>

namespace Webview {

struct Compositor::Private {
	Private(Compositor *parent)
	: shell(parent) {
	}

	QQmlEngine engine;
	QPointer<QQuickWidget> widget;
	base::unique_qptr<Output> output;
	QWaylandXdgShell shell;
};

Compositor::Compositor()
: _private(std::make_unique<Private>(this)) {
	connect(&_private->shell, &QWaylandXdgShell::toplevelCreated, [=](
			QWaylandXdgToplevel *toplevel,
			QWaylandXdgSurface *xdgSurface) {
		if (_private->output) {
			const auto output = new Output(
				&_private->engine,
				this,
				xdgSurface);

			connect(output, &Output::surfaceCompleted, [=] {
				output->window()->show();
			});
		} else if (_private->widget) {
			_private->output = base::make_unique_q<Output>(
				&_private->engine,
				this,
				xdgSurface,
				_private->widget->quickWindow());
		}
	});

	connect(&_private->shell, &QWaylandXdgShell::popupCreated, [=](
			QWaylandXdgPopup *popup,
			QWaylandXdgSurface *xdgSurface) {
		const auto output = new Output(&_private->engine, this, xdgSurface);

		connect(output, &Output::surfaceCompleted, [=] {
			const auto parent = qvariant_cast<Output*>(
				popup->parentXdgSurface()->property("output"))->window();
			if (_private->widget
					&& parent == _private->widget->quickWindow()) {
				output->window()->setTransientParent(
					_private->widget->window()->windowHandle());
				output->window()->setPosition(
					popup->unconstrainedPosition()
						+ _private->widget->mapToGlobal(QPoint()));
			} else {
				output->window()->setTransientParent(parent);
				output->window()->setPosition(
					popup->unconstrainedPosition() + parent->position());
			}
			output->window()->setFlag(Qt::Popup);
			output->quickWindow()->setColor(Qt::transparent);
			output->window()->show();
		});
	});

	create();
}

void Compositor::setWidget(QQuickWidget *widget) {
	_private->widget = widget;
	setParent(widget);
	if (!widget) {
		_private->output.reset();
	}
}

} // namespace Webview
