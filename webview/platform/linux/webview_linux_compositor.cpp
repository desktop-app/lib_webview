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
		QWaylandXdgSurface *xdgSurface,
		bool windowFollowsSize);

	rpl::producer<> surfaceCompleted() const {
		return _surfaceCompletes.events();
	}

private:
	QQuickItem _moveItem;
	bool _completed = false;
	rpl::event_stream<> _surfaceCompletes;
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
		xdgSurface->setProperty("output", QVariant::fromValue(this));
		QCoreApplication::processEvents();
		_chrome.emplace(this, xdgSurface, !window);
	}

	QQuickWindow *quickWindow() const {
		return qobject_cast<QQuickWindow*>(window());
	}

	Chrome *chrome() const {
		return _chrome;
	}

private:
	std::optional<QQuickWindow> _ownedWindow;
	std::optional<Chrome> _chrome;
};

Chrome::Chrome(
		Output *output,
		QWaylandXdgSurface *xdgSurface,
		bool windowFollowsSize) {
	setParentItem(output->quickWindow()->contentItem());
	setOutput(output);
	setShellSurface(xdgSurface);
	setAutoCreatePopupItems(false);
	setMoveItem(&_moveItem);
	_moveItem.setEnabled(false);

	base::qt_signal_producer(
		output->window(),
		&QObject::destroyed
	) | rpl::start_with_next([=] {
		if (const auto toplevel = xdgSurface->toplevel()) {
			toplevel->sendClose();
		} else if (const auto popup = xdgSurface->popup()) {
			popup->sendPopupDone();
		}
	}, _lifetime);

	rpl::single(rpl::empty) | rpl::then(
		rpl::merge(
			base::qt_signal_producer(
				output->window(),
				&QWindow::widthChanged
			),
			base::qt_signal_producer(
				output->window(),
				&QWindow::heightChanged
			)
		) | rpl::to_empty
	) | rpl::map([=] {
		return output->window()->size();
	}) | rpl::start_with_next([=](QSize size) {
		if (!windowFollowsSize
				|| !xdgSurface->windowGeometry().size().isEmpty()) {
			if (const auto toplevel = xdgSurface->toplevel()) {
				toplevel->sendFullscreen(size);
			} else if (const auto popup = xdgSurface->popup()) {
				popup->sendConfigure(QRect(QPoint(), size));
			}
		}
	}, _lifetime);

	rpl::single(rpl::empty) | rpl::then(
		base::qt_signal_producer(
			xdgSurface,
			&QWaylandXdgSurface::windowGeometryChanged
		)
	) | rpl::map([=] {
		return xdgSurface->windowGeometry();
	}) | rpl::start_with_next([=](const QRect &geometry) {
		setX(-geometry.x());
		setY(-geometry.y());

		if (windowFollowsSize) {
			output->window()->resize(geometry.size());
		}

		if (!_completed && !output->window()->size().isEmpty()) {
			_completed = true;
			_surfaceCompletes.fire({});
		}
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
			output->window()->setTitle(title);
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
				toplevel->sendFullscreen(output->window()->size());
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
		const auto output = new Output(this, xdgSurface);

		output->chrome()->surfaceCompleted() | rpl::start_with_next([=] {
			const auto parent = (*static_cast<Output * const *>(
				popup->parentXdgSurface()->property("output").constData()
			))->window();
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
		}, _private->lifetime);
	});

	setSocketName(socketName);
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
