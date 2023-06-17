// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/linux/webview_linux_compositor.h"

#include "base/flat_map.h"
#include "webview/platform/linux/webview_linux_compositor_output.h"

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
	std::optional<Output> output;
	QWaylandXdgShell shell;
	base::flat_map<QWaylandXdgSurface*, std::unique_ptr<Output>> toplevels;
	base::flat_map<QWaylandXdgSurface*, std::unique_ptr<Output>> popups;
};

Compositor::Compositor()
: _private(std::make_unique<Private>(this)) {
	connect(&_private->shell, &QWaylandXdgShell::toplevelCreated, [=](
			QWaylandXdgToplevel *toplevel,
			QWaylandXdgSurface *xdgSurface) {
		if (_private->output) {
			const auto output = _private->toplevels.emplace(
				xdgSurface,
				std::make_unique<Output>(&_private->engine, this, xdgSurface)
			).first->second.get();

			connect(output, &Output::surfaceCompleted, [=] {
				output->window()->show();
			});

			connect(
				xdgSurface,
				&QObject::destroyed,
				[=] {
					auto it = _private->toplevels.find(xdgSurface);
					if (it != _private->toplevels.cend()) {
						_private->toplevels.erase(it);
					}
				});
		} else if (_private->widget) {
			_private->output.emplace(
				&_private->engine,
				this,
				xdgSurface,
				_private->widget->quickWindow());

			connect(
				xdgSurface,
				&QObject::destroyed,
				[=] { _private->output.reset(); });
		}
	});

	connect(&_private->shell, &QWaylandXdgShell::popupCreated, [=](
			QWaylandXdgPopup *popup,
			QWaylandXdgSurface *xdgSurface) {
		const auto output = _private->popups.emplace(
			xdgSurface,
			std::make_unique<Output>(&_private->engine, this, xdgSurface)
		).first->second.get();

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

		connect(
			xdgSurface,
			&QObject::destroyed,
			[=] {
				auto it = _private->popups.find(xdgSurface);
				if (it != _private->popups.cend()) {
					_private->popups.erase(it);
				}
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
