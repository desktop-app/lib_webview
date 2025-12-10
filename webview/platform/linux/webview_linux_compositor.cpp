// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/linux/webview_linux_compositor.h"

#ifdef DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
#include "base/flat_map.h"
#include "base/unique_qptr.h"
#include "base/qt_signal_producer.h"
#include "base/event_filter.h"

#include <QtQuickWidgets/QQuickWidget>
#include <QtWaylandCompositor/QWaylandXdgSurface>
#include <QtWaylandCompositor/QWaylandXdgOutputV1>
#include <QtWaylandCompositor/QWaylandQuickOutput>
#include <QtWaylandCompositor/QWaylandQuickShellSurfaceItem>

namespace Webview {

struct Compositor::Private {
	Private(Compositor *parent)
	: shell(parent)
	, xdgOutput(parent) {
	}

	QPointer<QQuickWidget> widget;
	base::unique_qptr<Output> output;
	QWaylandXdgShell shell;
	QWaylandXdgOutputManagerV1 xdgOutput;
	rpl::lifetime lifetime;
};

class Compositor::Chrome : public QWaylandQuickShellSurfaceItem {
public:
	Chrome(
		Output *output,
		QQuickWindow *window,
		QWaylandXdgSurface *xdgSurface,
		bool windowFollowsSize);

	rpl::producer<> surfaceCompleted() const {
		return _completed.value()
			| rpl::filter(rpl::mappers::_1)
			| rpl::to_empty;
	}

private:
	QQuickItem _moveItem;
	rpl::variable<bool> _completed = false;
	rpl::lifetime _lifetime;
};

class Compositor::Output : public QWaylandQuickOutput {
public:
	Output(Compositor *compositor, QObject *parent = nullptr)
	: _xdg(this, &compositor->_private->xdgOutput) {
		const auto xdgSurface = qobject_cast<QWaylandXdgSurface*>(parent);
		const auto window = qobject_cast<QQuickWindow*>(parent);
		setParent(parent);
		setCompositor(compositor);
		setWindow(window ? window : &_ownedWindow.emplace());
		setScaleFactor(this->window()->devicePixelRatio());
		setSizeFollowsWindow(true);
		this->window()->setProperty("output", QVariant::fromValue(this));
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
		base::install_event_filter(this, this->window(), [=](
				not_null<QEvent*> e) {
			if (e->type() == QEvent::DevicePixelRatioChange) {
				setScaleFactor(this->window()->devicePixelRatio());
			}
			return base::EventFilterResult::Continue;
		});
#endif // Qt >= 6.6.0
		rpl::single(rpl::empty) | rpl::then(
			rpl::merge(
				base::qt_signal_producer(
					this,
					&QWaylandOutput::geometryChanged
				),
				base::qt_signal_producer(
					this,
					&QWaylandOutput::scaleFactorChanged
				)
			)
		) | rpl::map([=] {
			return std::make_tuple(geometry(), scaleFactor());
		}) | rpl::on_next([=](QRect geometry, int scaleFactor) {
			_xdg.setLogicalPosition(geometry.topLeft() / scaleFactor);
			_xdg.setLogicalSize(geometry.size() / scaleFactor);
		}, _lifetime);
		setXdgSurface(xdgSurface);
	}

	QQuickWindow *window() const {
		return static_cast<QQuickWindow*>(QWaylandQuickOutput::window());
	}

	Chrome *chrome() const {
		return _chrome;
	}

	void setXdgSurface(QWaylandXdgSurface *xdgSurface) {
		if (xdgSurface) {
			_chrome.emplace(this, window(), xdgSurface, bool(_ownedWindow));
		} else {
			_chrome.reset();
		}
	}

private:
	QWaylandXdgOutputV1 _xdg;
	std::optional<QQuickWindow> _ownedWindow;
	base::unique_qptr<Chrome> _chrome;
	rpl::lifetime _lifetime;
};

Compositor::Chrome::Chrome(
		Output *output,
		QQuickWindow *window,
		QWaylandXdgSurface *xdgSurface,
		bool windowFollowsSize)
: QWaylandQuickShellSurfaceItem(window->contentItem()) {
	base::qt_signal_producer(
		xdgSurface,
		&QObject::destroyed
	) | rpl::on_next([=] {
		delete this;
	}, _lifetime);

	rpl::single(rpl::empty) | rpl::then(
		base::qt_signal_producer(
			view(),
			&QWaylandView::surfaceChanged
		)
	) | rpl::on_next([=] {
		setOutput(output);
	}, _lifetime);

	setShellSurface(xdgSurface);
	setAutoCreatePopupItems(false);
	setMoveItem(&_moveItem);
	_moveItem.setEnabled(false);
	xdgSurface->setProperty("window", QVariant::fromValue(window));

	base::install_event_filter(this, window, [=](not_null<QEvent*> e) {
		if (e->type() != QEvent::Close) {
			return base::EventFilterResult::Continue;
		}
		e->ignore();
		if (const auto toplevel = xdgSurface->toplevel()) {
			toplevel->sendClose();
		} else if (const auto popup = xdgSurface->popup()) {
			popup->sendPopupDone();
		}
		return base::EventFilterResult::Cancel;
	});

	rpl::single(rpl::empty) | rpl::then(
		rpl::merge(
			base::qt_signal_producer(
				window,
				&QWindow::widthChanged
			),
			base::qt_signal_producer(
				window,
				&QWindow::heightChanged
			)
		) | rpl::to_empty
	) | rpl::map([=] {
		return window->size();
	}) | rpl::distinct_until_changed(
	) | rpl::filter([=](const QSize &size) {
		return !size.isEmpty();
	}) | rpl::on_next([=](const QSize &size) {
		if (const auto toplevel = xdgSurface->toplevel()) {
			toplevel->sendFullscreen(size);
		}
	}, _lifetime);

	rpl::single(rpl::empty) | rpl::then(
		rpl::merge(
			base::qt_signal_producer(
				xdgSurface->surface(),
				&QWaylandSurface::destinationSizeChanged
			),
			base::qt_signal_producer(
				xdgSurface,
				&QWaylandXdgSurface::windowGeometryChanged
			)
		)
	) | rpl::map([=] {
		return xdgSurface->windowGeometry().isValid()
			? xdgSurface->windowGeometry()
			: QRect(QPoint(), xdgSurface->surface()->destinationSize());
	}) | rpl::distinct_until_changed(
	) | rpl::filter([=](const QRect &geometry) {
		return geometry.isValid();
	}) | rpl::on_next([=](const QRect &geometry) {
		setX(-geometry.x());
		setY(-geometry.y());

		if (windowFollowsSize) {
			if (xdgSurface->popup()) {
				window->setMinimumSize(geometry.size());
				window->setMaximumSize(geometry.size());
			} else {
				window->resize(geometry.size());
			}
		}

		_completed = true;
	}, _lifetime);

	if (const auto toplevel = xdgSurface->toplevel()) {
		rpl::single(rpl::empty) | rpl::then(
			base::qt_signal_producer(
				toplevel,
				&QWaylandXdgToplevel::titleChanged
			)
		) | rpl::map([=] {
			return toplevel->title();
		}) | rpl::on_next([=](const QString &title) {
			window->setTitle(title);
		}, _lifetime);

		rpl::single(rpl::empty) | rpl::then(
			base::qt_signal_producer(
				toplevel,
				&QWaylandXdgToplevel::fullscreenChanged
			)
		) | rpl::map([=] {
			return toplevel->fullscreen();
		}) | rpl::on_next([=](bool fullscreen) {
			if (!fullscreen) {
				toplevel->sendFullscreen(window->size());
			}
		}, _lifetime);
	}
}

Compositor::Compositor(const QByteArray &socketName)
: _private(std::make_unique<Private>(this)) {
	connect(&_private->shell, &QWaylandXdgShell::toplevelCreated, [=](
			QWaylandXdgToplevel *toplevel,
			QWaylandXdgSurface *xdgSurface) {
		if (!_private->output || _private->output->chrome()) {
			const auto output = new Output(this, xdgSurface);

			output->chrome()->surfaceCompleted() | rpl::on_next([=] {
				output->window()->show();
			}, _private->lifetime);
		} else {
			_private->output->setXdgSurface(xdgSurface);
		}
	});

	connect(&_private->shell, &QWaylandXdgShell::popupCreated, [=](
			QWaylandXdgPopup *popup,
			QWaylandXdgSurface *xdgSurface) {
		const auto widget = _private->widget;
		const auto parent = (*static_cast<QQuickWindow * const *>(
			popup->parentXdgSurface()->property("window").constData()
		));
		const auto output = (*static_cast<Output * const *>(
			parent->property("output").constData()
		));
		const auto window = new QQuickWindow;
		static_cast<QObject*>(window)->setParent(xdgSurface);
		window->setProperty("output", QVariant::fromValue(output));
		const auto chrome = new Chrome(output, window, xdgSurface, true);

		chrome->surfaceCompleted() | rpl::on_next([=] {
			if (widget && parent == widget->quickWindow()) {
				window->setTransientParent(widget->window()->windowHandle());
				window->setPosition(
					popup->unconstrainedPosition()
						+ widget->mapToGlobal(QPoint()));
			} else {
				window->setTransientParent(parent);
				window->setPosition(
					popup->unconstrainedPosition() + parent->position());
			}
			window->setFlag(Qt::Popup);
			window->setColor(Qt::transparent);
			window->show();
		}, _private->lifetime);
	});

	setSocketName(socketName);
	create();
}

void Compositor::setWidget(QQuickWidget *widget) {
	_private->widget = widget;
	if (widget) {
		_private->output.emplace(this, widget->quickWindow());
	} else {
		_private->output.reset();
	}
}

} // namespace Webview
#endif // DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
