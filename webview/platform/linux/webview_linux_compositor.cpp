// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/linux/webview_linux_compositor.h"

#include "base/flat_map.h"
#include "base/unique_qptr.h"
#include "base/qt_signal_producer.h"
#include "base/event_filter.h"

#include <QtQuickWidgets/QQuickWidget>
#include <QtWaylandCompositor/QWaylandXdgSurface>
#include <QtWaylandCompositor/QWaylandQuickOutput>
#include <QtWaylandCompositor/QWaylandQuickShellSurfaceItem>

namespace Webview {
namespace {

class Output;

class Chrome : public QWaylandQuickShellSurfaceItem {
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

class Output : public QWaylandQuickOutput {
public:
	Output(
			QWaylandCompositor *compositor,
			QWaylandXdgSurface *xdgSurface,
			QQuickWindow *window = nullptr) {
		setParent(xdgSurface);
		setCompositor(compositor);
		setWindow(window ? window : &_ownedWindow.emplace());
		setScaleFactor(this->window()->devicePixelRatio());
		setSizeFollowsWindow(true);
		this->window()->setProperty("output", QVariant::fromValue(this));
		QCoreApplication::processEvents();
		_chrome.emplace(this, this->window(), xdgSurface, !window);
	}

	QQuickWindow *window() const {
		return static_cast<QQuickWindow*>(QWaylandQuickOutput::window());
	}

	Chrome *chrome() const {
		return _chrome;
	}

private:
	std::optional<QQuickWindow> _ownedWindow;
	base::unique_qptr<Chrome> _chrome;
};

Chrome::Chrome(
		Output *output,
		QQuickWindow *window,
		QWaylandXdgSurface *xdgSurface,
		bool windowFollowsSize)
: QWaylandQuickShellSurfaceItem(window->contentItem()) {
	rpl::single(rpl::empty) | rpl::then(
		base::qt_signal_producer(
			view(),
			&QWaylandView::surfaceChanged
		)
	) | rpl::start_with_next([=] {
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
	}) | rpl::start_with_next([=](const QSize &size) {
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
	}) | rpl::start_with_next([=](const QRect &geometry) {
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
		}) | rpl::start_with_next([=](const QString &title) {
			window->setTitle(title);
		}, _lifetime);

		rpl::single(rpl::empty) | rpl::then(
			base::qt_signal_producer(
				toplevel,
				&QWaylandXdgToplevel::fullscreenChanged
			)
		) | rpl::map([=] {
			return toplevel->fullscreen();
		}) | rpl::start_with_next([=](bool fullscreen) {
			if (!fullscreen) {
				toplevel->sendFullscreen(window->size());
			}
		}, _lifetime);
	}
}

} // namespace

struct Compositor::Private {
	Private(Compositor *parent)
	: shell(parent) {
	}

	QPointer<QQuickWidget> widget;
	base::unique_qptr<Output> output;
	QWaylandXdgShell shell;
	rpl::lifetime lifetime;
};

Compositor::Compositor(const QByteArray &socketName)
: _private(std::make_unique<Private>(this)) {
	connect(&_private->shell, &QWaylandXdgShell::toplevelCreated, [=](
			QWaylandXdgToplevel *toplevel,
			QWaylandXdgSurface *xdgSurface) {
		if (_private->output || !_private->widget) {
			const auto output = new Output(this, xdgSurface);

			output->chrome()->surfaceCompleted() | rpl::start_with_next([=] {
				output->window()->show();
			}, _private->lifetime);
		} else {
			_private->output.emplace(
				this,
				xdgSurface,
				_private->widget->quickWindow());
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

		chrome->surfaceCompleted() | rpl::start_with_next([=] {
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
}

} // namespace Webview
