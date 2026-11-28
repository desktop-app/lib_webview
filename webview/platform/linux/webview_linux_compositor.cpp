// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/linux/webview_linux_compositor.h"

#include "base/event_filter.h"
#include "base/flat_map.h"
#include "base/qt_signal_producer.h"
#include "base/unique_qptr.h"
#include "base/weak_ptr.h"
#include "ui/gl/gl_image.h"
#include "ui/gl/gl_surface.h"
#include "ui/platform/ui_platform_utility.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"

#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>
#include <QtGui/QOpenGLContext>
#include <QtGui/QScreen>
#include <QtOpenGL/QOpenGLTexture>
#include <QtOpenGL/QOpenGLTextureBlitter>
#include <QtOpenGLWidgets/QOpenGLWidget>
#include <QtWaylandCompositor/QWaylandDrag>
#include <QtWaylandCompositor/QWaylandOutput>
#include <QtWaylandCompositor/QWaylandSeat>
#include <QtWaylandCompositor/QWaylandView>
#include <QtWaylandCompositor/QWaylandViewporter>
#include <QtWaylandCompositor/QWaylandXdgSurface>
#include <QtWaylandCompositor/QWaylandXdgOutputV1>

#include <wayland-server-core.h>

#include <cmath>
#include <ranges>

namespace Webview {
namespace {

struct SurfaceGeometry {
	QRect window;
	QSize surface;

	friend inline bool operator==(
		const SurfaceGeometry &,
		const SurfaceGeometry &) = default;
};

[[nodiscard]] QRect WindowGeometry(not_null<QWaylandXdgSurface*> xdgSurface) {
	const auto geometry = xdgSurface->windowGeometry();
	return geometry.isValid()
		? geometry
		: QRect(QPoint(), xdgSurface->surface()->destinationSize());
}

#ifndef DESKTOP_APP_DISABLE_PRIVATE_QT_API
[[nodiscard]] uint ConstraintAdjustment(not_null<QWaylandXdgPopup*> popup) {
	const auto bits = [](Qt::Orientations value, uint x, uint y) {
		return (value.testFlag(Qt::Horizontal) ? x : 0u)
			| (value.testFlag(Qt::Vertical) ? y : 0u);
	};
	return bits(popup->slideConstraints(), 1, 2)
		| bits(popup->flipConstraints(), 4, 8)
		| bits(popup->resizeConstraints(), 16, 32);
}
#endif // !DESKTOP_APP_DISABLE_PRIVATE_QT_API

[[nodiscard]] QRectF SourceRect(not_null<QWaylandSurface*> surface) {
	const auto source = surface->sourceGeometry();
	const auto scale = surface->bufferScale();
	return QRectF(source.topLeft() * scale, source.size() * scale);
}

[[nodiscard]] int OutputScale(not_null<QWidget*> widget) {
	return std::max(int(widget->devicePixelRatio()), 1);
}

[[nodiscard]] qreal UnitScale(not_null<QWidget*> widget) {
	return OutputScale(widget) / widget->devicePixelRatio();
}

[[nodiscard]] bool IsScaleChange(not_null<QEvent*> e) {
	return (e->type() == QEvent::DevicePixelRatioChange)
		|| (e->type() == QEvent::ScreenChangeInternal)
		|| (e->type() == QEvent::Show);
}

void Upload(
		QOpenGLFunctions &f,
		const Ui::GL::Textures<1> &texture,
		QImage image) {
	// Converting RGBX8888 to ARGB32 keeps its undefined X byte as alpha
	const auto opaque = (image.format() == QImage::Format_RGBX8888);
	if (image.format() != QImage::Format_ARGB32_Premultiplied) {
		image = std::move(image).convertToFormat(opaque
			? QImage::Format_RGB32
			: QImage::Format_ARGB32_Premultiplied);
	}
	texture.bind(f, 0);
	f.glPixelStorei(GL_UNPACK_ROW_LENGTH, image.bytesPerLine() / 4);
	f.glTexImage2D(
		GL_TEXTURE_2D,
		0,
		Ui::GL::kFormatRGBA,
		image.width(),
		image.height(),
		0,
		Ui::GL::kFormatRGBA,
		GL_UNSIGNED_BYTE,
		image.constBits());
	f.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

class SurfaceNode final {
public:
	SurfaceNode(
		not_null<QWaylandSurface*> surface,
		QWaylandOutput *output,
		Fn<void()> update);

	[[nodiscard]] QWaylandSurface *surface() const;
	[[nodiscard]] not_null<QWaylandView*> view();

	void enumerate(
		const Fn<void(not_null<SurfaceNode*>, QPoint)> &callback,
		QPoint origin);

private:
	void addChild(not_null<QWaylandSurface*> surface);
	void removeChild(not_null<SurfaceNode*> child);
	void restack(
		not_null<SurfaceNode*> child,
		QWaylandSurface *sibling,
		bool above);

	const QPointer<QWaylandSurface> _surface;
	const Fn<void()> _update;
	QWaylandView _view;
	QPoint _position;
	std::vector<std::unique_ptr<SurfaceNode>> _children;
	std::vector<not_null<SurfaceNode*>> _stack;
	rpl::lifetime _lifetime;

};

SurfaceNode::SurfaceNode(
		not_null<QWaylandSurface*> surface,
		QWaylandOutput *output,
		Fn<void()> update)
: _surface(surface.get())
, _update(std::move(update))
, _stack({ this }) {
	_view.setSurface(surface);
	_view.setOutput(output);

	base::qt_signal_producer(
		surface.get(),
		&QWaylandSurface::redraw
	) | rpl::on_next([=] { _update(); }, _lifetime);

	base::qt_signal_producer(
		surface.get(),
		&QWaylandSurface::childAdded
	) | rpl::on_next([=](QWaylandSurface *child) {
		addChild(child);
	}, _lifetime);
}

QWaylandSurface *SurfaceNode::surface() const {
	return _surface.data();
}

not_null<QWaylandView*> SurfaceNode::view() {
	return &_view;
}

void SurfaceNode::enumerate(
		const Fn<void(not_null<SurfaceNode*>, QPoint)> &callback,
		QPoint origin) {
	for (const auto &node : _stack) {
		if (node != this) {
			node->enumerate(callback, origin + node->_position);
		} else if (_surface) {
			callback(this, origin);
		}
	}
}

void SurfaceNode::addChild(not_null<QWaylandSurface*> surface) {
	auto owned = std::make_unique<SurfaceNode>(
		surface,
		_view.output(),
		_update);
	const auto child = owned.get();
	_children.push_back(std::move(owned));
	_stack.push_back(child);

	base::qt_signal_producer(
		surface.get(),
		&QWaylandSurface::subsurfacePositionChanged
	) | rpl::on_next([=](const QPoint &position) {
		child->_position = position;
		_update();
	}, child->_lifetime);

	base::qt_signal_producer(
		surface.get(),
		&QWaylandSurface::subsurfacePlaceAbove
	) | rpl::on_next([=](QWaylandSurface *sibling) {
		restack(child, sibling, true);
	}, child->_lifetime);

	base::qt_signal_producer(
		surface.get(),
		&QWaylandSurface::subsurfacePlaceBelow
	) | rpl::on_next([=](QWaylandSurface *sibling) {
		restack(child, sibling, false);
	}, child->_lifetime);

	base::qt_signal_producer(
		surface.get(),
		&QWaylandSurface::surfaceDestroyed
	) | rpl::on_next([=] { removeChild(child); }, child->_lifetime);
}

void SurfaceNode::removeChild(not_null<SurfaceNode*> child) {
	std::erase(_stack, child);
	std::erase_if(
		_children,
		[&](const std::unique_ptr<SurfaceNode> &node) {
			return node.get() == child;
		});
	_update();
}

void SurfaceNode::restack(
		not_null<SurfaceNode*> child,
		QWaylandSurface *sibling,
		bool above) {
	std::erase(_stack, child);
	const auto i = std::ranges::find_if(
		_stack,
		[&](not_null<SurfaceNode*> node) {
			return node->surface() == sibling;
		});
	_stack.insert((i != end(_stack) && above) ? std::next(i) : i, child);
	_update();
}

} // namespace

class Compositor::Output final : public QWaylandOutput {
public:
	Output(not_null<Compositor*> compositor, not_null<QWidget*> widget);

private:
	void updateMode(not_null<QWidget*> widget);

	QWaylandXdgOutputV1 _xdg;
	rpl::lifetime _lifetime;

};

class Compositor::Display final
	: public Ui::GL::Renderer
	, public base::has_weak_ptr {
public:
	Display(not_null<Compositor*> compositor, Output *output);

	void attach(not_null<Ui::RpWidgetWrap*> widget);

	[[nodiscard]] not_null<Ui::RpWidgetWrap*> widget() const;
	[[nodiscard]] Output *output() const;
	[[nodiscard]] bool hasSurface() const;
	[[nodiscard]] qreal scale() const;
	[[nodiscard]] rpl::producer<qreal> scaleValue() const;

	void setSurface(QWaylandSurface *surface);
	void setOrigin(QPoint origin);
	void setBackground(QColor color);
	void setParentDisplay(not_null<Display*> parent);
	void setHeld(bool held);

	void init(QOpenGLFunctions &f) override;
	void deinit(QOpenGLFunctions *f) override;
	void paint(
		not_null<QOpenGLWidget*> widget,
		QOpenGLFunctions &f) override;
	[[nodiscard]] std::optional<QColor> clearColor() override;
	void paintFallback(
		Painter &p,
		const QRegion &clip,
		Ui::GL::Backend backend) override;

private:
	struct Target {
		not_null<SurfaceNode*> node;
		QPoint position;
	};

	[[nodiscard]] std::vector<Target> targets() const;
	[[nodiscard]] QRectF opaqueRect() const;
	[[nodiscard]] std::optional<Target> targetAt(
		QPointF position) const;
	[[nodiscard]] std::optional<Target> targetFor(
		QWaylandView *view) const;
	[[nodiscard]] QWaylandView *viewAt(QPointF position) const;
	[[nodiscard]] std::vector<not_null<Display*>> displayChain();
	[[nodiscard]] Display *pointerTarget(QPointF global);
	void update();
	void updateScale();
	void sendFrameCallbacks();
	void syncPointer();
	void sendMouseMove(not_null<QWaylandSeat*> seat, QPointF global);
	void sendLocalMouseMove(
		not_null<QWaylandSeat*> seat,
		QPointF position);
	[[nodiscard]] QWaylandDrag *activeDrag(
		not_null<QWaylandSeat*> seat) const;
	void sendDragMove(not_null<QWaylandDrag*> drag, QPointF global);
	void finishDrag(not_null<QWaylandDrag*> drag, bool cancel);
	void releaseGrab(not_null<QWaylandSeat*> seat);
	[[nodiscard]] base::EventFilterResult filterEvent(
		not_null<QEvent*> e);

	const QPointer<Compositor> _compositor;
	base::unique_qptr<Output> _ownedOutput;
	QPointer<Output> _output;
	Ui::RpWidgetWrap *_widget = nullptr;
	std::unique_ptr<SurfaceNode> _root;
	QPoint _origin;
	QColor _background = Qt::transparent;
	bool _held = false;
	rpl::variable<qreal> _scale = 1.;
	std::optional<QOpenGLTextureBlitter> _blitter;
	base::flat_map<SurfaceNode*, Ui::GL::Textures<1>> _textures;
	QPointer<QWaylandView> _grab;
	QPointer<QWaylandView> _touch;
	base::weak_ptr<Display> _parentDisplay;
	base::unique_qptr<QObject> _filter;

};

class Compositor::Chrome final {
public:
	Chrome(
		not_null<QWaylandXdgSurface*> xdgSurface,
		not_null<Display*> display,
		base::unique_qptr<QWidget> window);
	~Chrome();

	[[nodiscard]] Display *display() const;
	[[nodiscard]] QPoint geometryPosition() const;
	[[nodiscard]] rpl::producer<> surfaceCompleted() const;
	[[nodiscard]] rpl::lifetime &lifetime();

private:
	void trackGeometry(
		not_null<QWaylandXdgSurface*> xdgSurface,
		not_null<Display*> display);
	void updatePopupWindow(const SurfaceGeometry &geometry, qreal scale);
	void trackToplevel(
		not_null<QWaylandXdgToplevel*> toplevel,
		not_null<Display*> display);
	void trackSizeLimits(
		not_null<QWaylandXdgToplevel*> toplevel,
		not_null<Display*> display);
	void setHeld(bool held);
	void followClient();
	void setupWindow(not_null<QWaylandXdgSurface*> xdgSurface);

	const not_null<QWaylandXdgSurface*> _xdgSurface;
	const base::weak_ptr<Display> _display;
	const base::unique_qptr<QWidget> _window;
	rpl::variable<bool> _completed = false;
	bool _held = false;
	bool _following = false;
	rpl::lifetime _lifetime;

};

struct Compositor::Private {
	Private(not_null<Compositor*> compositor, bool gpu)
	: compositor(compositor)
	, shell(compositor)
	, xdgOutput(compositor)
	, viewporter(compositor)
	, gpu(gpu) {
	}

	[[nodiscard]] not_null<Display*> createDisplay(
		QWidget *parent,
		Output *output);
	not_null<Chrome*> addChrome(
		not_null<QWaylandXdgSurface*> xdgSurface,
		not_null<Display*> display,
		base::unique_qptr<QWidget> window);
	void toplevelCreated(not_null<QWaylandXdgSurface*> xdgSurface);
	void trackParent(
		not_null<QWaylandXdgToplevel*> toplevel,
		not_null<QWidget*> window,
		rpl::lifetime &lifetime);
	void popupCreated(
		not_null<QWaylandXdgPopup*> popup,
		not_null<QWaylandXdgSurface*> xdgSurface);
	void showPopup(
		not_null<QWaylandXdgPopup*> popup,
		not_null<Chrome*> chrome);
	void sendPopupPlacement(
		not_null<QWaylandXdgPopup*> popup,
		not_null<Chrome*> chrome,
		not_null<QRect*> sent);

	const not_null<Compositor*> compositor;
	QWaylandXdgShell shell;
	QWaylandXdgOutputManagerV1 xdgOutput;
	QWaylandViewporter viewporter;
	const bool gpu = false;
	bool dragging = false;
	QColor background;
	base::weak_ptr<Display> display;
	base::flat_map<QWaylandXdgSurface*, std::unique_ptr<Chrome>> chromes;
};

Compositor::Output::Output(
		not_null<Compositor*> compositor,
		not_null<QWidget*> widget)
: QWaylandOutput(compositor, nullptr)
, _xdg(this, &compositor->_private->xdgOutput) {
	updateMode(widget);
	base::install_event_filter(
		this,
		widget,
		[=](not_null<QEvent*> e) {
			if (IsScaleChange(e)) {
				updateMode(widget);
			}
			return base::EventFilterResult::Continue;
		});
	rpl::single(rpl::empty) | rpl::then(
		rpl::merge(
			base::qt_signal_producer(
				this,
				&QWaylandOutput::geometryChanged),
			base::qt_signal_producer(
				this,
				&QWaylandOutput::scaleFactorChanged))
	) | rpl::map([=] {
		return std::make_tuple(geometry(), scaleFactor());
	}) | rpl::on_next([=](QRect geometry, int scaleFactor) {
		_xdg.setLogicalPosition(geometry.topLeft() / scaleFactor);
		_xdg.setLogicalSize(geometry.size() / scaleFactor);
	}, _lifetime);
}

void Compositor::Output::updateMode(not_null<QWidget*> widget) {
	const auto screen = widget->screen();
	const auto scale = OutputScale(widget);
	const auto units = QSizeF(screen->size()) / UnitScale(widget);
	const QWaylandOutputMode mode(
		units.toSize() * scale,
		int(std::floor(screen->refreshRate() * 1000)));
	if (mode == currentMode() && scale == scaleFactor()) {
		return;
	}
	addMode(mode, true);
	setCurrentMode(mode);
	setScaleFactor(scale);
}

Compositor::Display::Display(not_null<Compositor*> compositor, Output *output)
: _compositor(compositor.get())
, _output(output) {
}

void Compositor::Display::attach(not_null<Ui::RpWidgetWrap*> widget) {
	_widget = widget;
	const auto raw = widget->rpWidget();
	raw->setMouseTracking(true);
	raw->setFocusPolicy(Qt::StrongFocus);
	raw->setAttribute(Qt::WA_AcceptTouchEvents);
	if (!_output) {
		_ownedOutput = base::make_unique_q<Output>(_compositor, raw);
		_output = _ownedOutput.get();
	}
	updateScale();
	const auto filter = [=](not_null<QEvent*> e) {
		return filterEvent(e);
	};
	_filter.reset(base::install_event_filter(raw, filter));
}

not_null<Ui::RpWidgetWrap*> Compositor::Display::widget() const {
	return _widget;
}

Compositor::Output *Compositor::Display::output() const {
	return _output;
}

bool Compositor::Display::hasSurface() const {
	return _root != nullptr;
}

qreal Compositor::Display::scale() const {
	return _scale.current();
}

rpl::producer<qreal> Compositor::Display::scaleValue() const {
	return _scale.value();
}

void Compositor::Display::setSurface(QWaylandSurface *surface) {
	_grab = nullptr;
	_touch = nullptr;
	_root = nullptr;
	if (surface) {
		_root = std::make_unique<SurfaceNode>(
			surface,
			_output,
			[=] { update(); });
	}
	update();
}

void Compositor::Display::setOrigin(QPoint origin) {
	_origin = origin;
	update();
}

void Compositor::Display::setBackground(QColor color) {
	_background = color;
	update();
}

void Compositor::Display::setParentDisplay(not_null<Display*> parent) {
	_parentDisplay = base::make_weak(parent);
}

void Compositor::Display::setHeld(bool held) {
	if (_held == held) {
		return;
	}
	_held = held;
	_widget->rpWidget()->setUpdatesEnabled(!held);
	if (held) {
		sendFrameCallbacks();
	}
}

void Compositor::Display::init(QOpenGLFunctions &f) {
	_blitter.emplace();
	_blitter->create();

	const auto context = QOpenGLContext::currentContext();
	const auto widget = static_cast<QOpenGLWidget*>(_widget->rpWidget());
	// Let QtWaylandCompositor free its textures with the context current
	QObject::connect(
		context,
		&QOpenGLContext::aboutToBeDestroyed,
		context,
		[=] { widget->makeCurrent(); });
}

void Compositor::Display::deinit(QOpenGLFunctions *f) {
	for (auto &[node, texture] : _textures) {
		texture.destroy(f);
	}
	_textures.clear();
	if (_blitter && f) {
		_blitter->destroy();
	}
	_blitter.reset();
}

void Compositor::Display::paint(
		not_null<QOpenGLWidget*> widget,
		QOpenGLFunctions &f) {
	if (!_blitter) {
		return;
	}
	const auto list = targets();
	const auto factor = widget->devicePixelRatio() * scale();
	const auto viewport = QRect(
		QPoint(),
		widget->size() * widget->devicePixelRatio());
	f.glEnable(GL_BLEND);
	f.glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	auto textures = base::flat_map<SurfaceNode*, Ui::GL::Textures<1>>();
	auto bound = GLenum();
	for (const auto &[node, position] : list) {
		const auto updated = node->view()->advance();
		const auto buffer = node->view()->currentBuffer();
		if (!buffer.hasContent()) {
			continue;
		}
		auto id = GLuint();
		auto target = GLenum(GL_TEXTURE_2D);
		auto swizzle = false;
		if (buffer.isSharedMemory()) {
			auto &texture = textures[node.get()];
			const auto i = _textures.find(node.get());
			if (i != _textures.end()) {
				texture = i->second;
				_textures.erase(i);
			}
			const auto upload = updated || !texture.created();
			texture.ensureCreated(f);
			if (upload) {
				Upload(f, texture, buffer.image());
			}
			id = texture.id(0);
			swizzle = Ui::GL::kSwizzleRedBlue;
		} else if (const auto texture = buffer.toOpenGLTexture()) {
			id = texture->textureId();
			target = GLenum(texture->target());
		} else {
			continue;
		}
		if (target != bound) {
			if (bound) {
				_blitter->release();
			}
			bound = target;
			_blitter->bind(bound);
		}
		const auto rect = QRectF(
			QPointF(position) * factor,
			QSizeF(node->surface()->destinationSize()) * factor);
		const auto transform = QOpenGLTextureBlitter::targetTransform(
			rect,
			viewport);
		const auto origin = buffer.origin();
		const auto source = QOpenGLTextureBlitter::sourceTransform(
			SourceRect(node->surface()),
			buffer.size(),
			(origin == QWaylandSurface::OriginTopLeft)
				? QOpenGLTextureBlitter::OriginTopLeft
				: QOpenGLTextureBlitter::OriginBottomLeft);
		if (node->surface()->isOpaque()) {
			f.glDisable(GL_BLEND);
		} else {
			f.glEnable(GL_BLEND);
		}
		_blitter->setRedBlueSwizzle(swizzle);
		_blitter->blit(id, transform, source);
	}
	if (bound) {
		_blitter->release();
	}
	for (auto &[node, texture] : _textures) {
		texture.destroy(&f);
	}
	_textures = std::move(textures);
	sendFrameCallbacks();
	syncPointer();
}

std::optional<QColor> Compositor::Display::clearColor() {
	const auto widget = _widget->rpWidget();
	const auto ratio = widget->devicePixelRatio();
	const auto opaque = opaqueRect();
	const auto covered = QRectF(
		opaque.topLeft() * ratio,
		opaque.size() * ratio).toRect();
	if (covered.contains(QRect(QPoint(), widget->size() * ratio))) {
		return std::nullopt;
	}
	return _background;
}

void Compositor::Display::paintFallback(
		Painter &p,
		const QRegion &clip,
		Ui::GL::Backend backend) {
	const auto opaque = opaqueRect();
	const auto left = int(std::ceil(opaque.left()));
	const auto top = int(std::ceil(opaque.top()));
	const auto right = int(std::floor(opaque.right()));
	const auto bottom = int(std::floor(opaque.bottom()));
	const auto covered = QRect(left, top, right - left, bottom - top);
	p.setCompositionMode(QPainter::CompositionMode_Source);
	for (const auto &rect : clip - covered) {
		p.fillRect(rect, _background);
	}
	const auto list = targets();
	const auto factor = scale();
	for (const auto &[node, position] : list) {
		node->view()->advance();
		const auto buffer = node->view()->currentBuffer();
		if (!buffer.hasContent() || !buffer.isSharedMemory()) {
			continue;
		}
		const auto rect = QRectF(
			QPointF(position) * factor,
			QSizeF(node->surface()->destinationSize()) * factor);
		const auto source = SourceRect(node->surface());
		p.setCompositionMode(node->surface()->isOpaque()
			? QPainter::CompositionMode_Source
			: QPainter::CompositionMode_SourceOver);
		p.drawImage(rect, buffer.image(), source);
	}
	p.setCompositionMode(QPainter::CompositionMode_SourceOver);
	sendFrameCallbacks();
	syncPointer();
}

auto Compositor::Display::targets() const -> std::vector<Target> {
	auto result = std::vector<Target>();
	const auto add = [&](not_null<SurfaceNode*> node, QPoint position) {
		result.push_back({ node, position });
	};
	if (_root) {
		_root->enumerate(add, _origin);
	}
	return result;
}

QRectF Compositor::Display::opaqueRect() const {
	auto result = QRectF();
	const auto factor = scale();
	for (const auto &[node, position] : targets()) {
		const auto surface = node->surface();
		if (!surface->hasContent() || !surface->isOpaque()) {
			continue;
		}
		const auto rect = QRectF(
			QPointF(position) * factor,
			QSizeF(surface->destinationSize()) * factor);
		if (rect.width() * rect.height()
			> result.width() * result.height()) {
			result = rect;
		}
	}
	return result;
}

auto Compositor::Display::targetAt(
		QPointF position) const -> std::optional<Target> {
	const auto list = targets();
	const auto point = position / scale();
	for (const auto &target : list | std::views::reverse) {
		const auto surface = target.node->surface();
		const auto local = point - QPointF(target.position);
		const auto size = QSizeF(surface->destinationSize());
		if (QRectF(QPointF(), size).contains(local)
				&& surface->inputRegionContains(local)) {
			return target;
		}
	}
	return std::nullopt;
}

auto Compositor::Display::targetFor(
		QWaylandView *view) const -> std::optional<Target> {
	if (!view) {
		return std::nullopt;
	}
	const auto list = targets();
	const auto i = std::ranges::find_if(
		list,
		[&](const Target &target) {
			return target.node->view() == view;
		});
	return (i != end(list)) ? std::make_optional(*i) : std::nullopt;
}

QWaylandView *Compositor::Display::viewAt(QPointF position) const {
	const auto target = targetAt(position);
	return target ? target->node->view().get() : nullptr;
}

auto Compositor::Display::displayChain() -> std::vector<not_null<Display*>> {
	auto result = std::vector<not_null<Display*>>{ this };
	while (const auto parent = result.back()->_parentDisplay.get()) {
		result.push_back(parent);
	}
	return result;
}

auto Compositor::Display::pointerTarget(QPointF global) -> Display* {
	const auto chain = displayChain();
	for (const auto &display : chain) {
		if (display->_grab) {
			return display;
		}
	}
	for (const auto &display : chain) {
		const auto widget = display->_widget->rpWidget();
		if (display->viewAt(widget->mapFromGlobal(global))) {
			return display;
		}
	}
	return nullptr;
}

void Compositor::Display::update() {
	if (_held) {
		// Let the client draw the frame we are waiting for
		sendFrameCallbacks();
	} else if (_widget) {
		_widget->rpWidget()->update();
	}
}

void Compositor::Display::updateScale() {
	_scale = UnitScale(_widget->rpWidget());
	update();
}

void Compositor::Display::sendFrameCallbacks() {
	if (_output) {
		_output->frameStarted();
		_output->sendFrameCallbacks();
	}
}

void Compositor::Display::syncPointer() {
	const auto seat = _compositor ? _compositor->defaultSeat() : nullptr;
	if (!seat) {
		return;
	}
	const auto chain = displayChain();
	const auto focus = seat->mouseFocus();
	const auto ours = [&](not_null<Display*> display) {
		return display->_grab || display->targetFor(focus);
	};
	const auto hovered = [](not_null<Display*> display) {
		return display->_widget->rpWidget()->underMouse();
	};
	if (std::ranges::any_of(chain, ours)
			|| !std::ranges::any_of(chain, hovered)) {
		return;
	}
	sendMouseMove(seat, QCursor::pos());
}

void Compositor::Display::sendMouseMove(
		not_null<QWaylandSeat*> seat,
		QPointF global) {
	if (const auto display = pointerTarget(global)) {
		const auto widget = display->_widget->rpWidget();
		const auto position = widget->mapFromGlobal(global);
		display->sendLocalMouseMove(seat, position);
		return;
	}
	for (const auto &display : displayChain()) {
		if (display->targetFor(seat->mouseFocus())) {
			seat->setMouseFocus(nullptr);
			return;
		}
	}
}

void Compositor::Display::sendLocalMouseMove(
		not_null<QWaylandSeat*> seat,
		QPointF position) {
	const auto target = _grab ? targetFor(_grab) : targetAt(position);
	if (target) {
		const auto point = position / scale();
		seat->sendMouseMoveEvent(
			target->node->view(),
			point - QPointF(target->position),
			point);
	}
}

QWaylandDrag *Compositor::Display::activeDrag(
		not_null<QWaylandSeat*> seat) const {
	return (_compositor && _compositor->_private->dragging)
		? seat->drag()
		: nullptr;
}

void Compositor::Display::sendDragMove(
		not_null<QWaylandDrag*> drag,
		QPointF global) {
	for (const auto &display : displayChain()) {
		const auto widget = display->_widget->rpWidget();
		const auto position = widget->mapFromGlobal(global);
		if (const auto target = display->targetAt(position)) {
			const auto point = position / display->scale();
			drag->dragMove(
				target->node->surface(),
				point - QPointF(target->position));
			return;
		}
	}
	drag->dragMove(nullptr, QPointF());
}

void Compositor::Display::finishDrag(
		not_null<QWaylandDrag*> drag,
		bool cancel) {
	_compositor->_private->dragging = false;
	if (cancel) {
		drag->dragMove(nullptr, QPointF());
	}
	drag->drop();
}

void Compositor::Display::releaseGrab(not_null<QWaylandSeat*> seat) {
	const auto drag = (_grab || _touch) ? activeDrag(seat) : nullptr;
	if (drag) {
		finishDrag(drag, true);
	} else if (_grab) {
		const auto buttons = QGuiApplication::mouseButtons();
		const auto last = uint(Qt::MaxMouseButton);
		for (auto bit = 1u; bit <= last; bit <<= 1) {
			const auto button = Qt::MouseButton(bit);
			if (buttons.testFlag(button)) {
				seat->sendMouseReleaseEvent(button);
			}
		}
	}
	if (const auto target = targetFor(_touch)) {
		seat->sendTouchCancelEvent(target->node->surface()->client());
	}
	if (targetFor(seat->mouseFocus())) {
		seat->setMouseFocus(nullptr);
	}
	_grab = nullptr;
	_touch = nullptr;
}

base::EventFilterResult Compositor::Display::filterEvent(
		not_null<QEvent*> e) {
	using Result = base::EventFilterResult;
	if (IsScaleChange(e)) {
		updateScale();
	}
	const auto seat = (_compositor && _root)
		? _compositor->defaultSeat()
		: nullptr;
	if (!seat) {
		return Result::Continue;
	}
	switch (e->type()) {
	case QEvent::MouseButtonPress:
	case QEvent::MouseButtonDblClick: {
		const auto event = static_cast<QMouseEvent*>(e.get());
		const auto global = event->globalPosition();
		const auto display = pointerTarget(global);
		if (!display) {
			if (_widget->rpWidget()->windowType() != Qt::Popup) {
				return Result::Continue;
			}
			for (const auto &popup : displayChain()) {
				const auto raw = popup->_widget->rpWidget();
				if (raw->windowType() == Qt::Popup) {
					raw->close();
				}
			}
			return Result::Cancel;
		}
		if (!display->_grab) {
			const auto widget = display->_widget->rpWidget();
			const auto position = widget->mapFromGlobal(global);
			display->_grab = display->viewAt(position);
		}
		sendMouseMove(seat, global);
		seat->sendMousePressEvent(event->button());
		return Result::Cancel;
	}
	case QEvent::MouseMove: {
		const auto event = static_cast<QMouseEvent*>(e.get());
		if (const auto drag = activeDrag(seat)) {
			sendDragMove(drag, event->globalPosition());
		} else {
			sendMouseMove(seat, event->globalPosition());
		}
		return Result::Cancel;
	}
	case QEvent::MouseButtonRelease: {
		const auto event = static_cast<QMouseEvent*>(e.get());
		const auto global = event->globalPosition();
		const auto display = pointerTarget(global);
		if (const auto drag = activeDrag(seat)) {
			finishDrag(drag, false);
		} else if (display && display->_grab) {
			seat->sendMouseReleaseEvent(event->button());
		}
		if (event->buttons() == Qt::NoButton) {
			if (display) {
				display->_grab = nullptr;
			}
			sendMouseMove(seat, global);
		}
		return Result::Cancel;
	}
	case QEvent::Enter: {
		const auto event = static_cast<QEnterEvent*>(e.get());
		sendMouseMove(seat, event->globalPosition());
		return Result::Continue;
	}
	case QEvent::Leave: {
		if (!_grab && targetFor(seat->mouseFocus())) {
			seat->setMouseFocus(nullptr);
		}
		return Result::Continue;
	}
	case QEvent::Wheel: {
		const auto event = static_cast<QWheelEvent*>(e.get());
		const auto global = event->globalPosition();
		sendMouseMove(seat, global);
		if (!pointerTarget(global)) {
			return Result::Continue;
		}
		const auto delta = event->angleDelta();
		if (delta.x()) {
			seat->sendMouseWheelEvent(Qt::Horizontal, delta.x());
		}
		if (delta.y()) {
			seat->sendMouseWheelEvent(Qt::Vertical, delta.y());
		}
		return Result::Cancel;
	}
	case QEvent::KeyPress:
	case QEvent::KeyRelease: {
		const auto event = static_cast<QKeyEvent*>(e.get());
		const auto focus = (e->type() == QEvent::KeyPress)
			|| !seat->keyboardFocus();
		if (!focus || seat->setKeyboardFocus(_root->surface())) {
			seat->sendFullKeyEvent(event);
		}
		return Result::Cancel;
	}
	case QEvent::FocusIn: {
		seat->setKeyboardFocus(_root->surface());
		return Result::Continue;
	}
	case QEvent::Hide: {
		releaseGrab(seat);
		return Result::Continue;
	}
	case QEvent::TouchBegin:
	case QEvent::TouchUpdate:
	case QEvent::TouchEnd: {
		const auto event = static_cast<QTouchEvent*>(e.get());
		const auto &points = event->points();
		if (e->type() == QEvent::TouchBegin) {
			_touch = !points.isEmpty()
				? viewAt(points.front().position())
				: nullptr;
		}
		if (const auto target = targetFor(_touch)) {
			const auto surface = target->node->surface();
			const auto offset = QPointF(target->position);
			for (const auto &point : points) {
				seat->sendTouchPointEvent(
					surface,
					point.id(),
					point.position() / scale() - offset,
					Qt::TouchPointState(point.state()));
			}
			seat->sendTouchFrameEvent(surface->client());
		}
		if (const auto drag = _touch ? activeDrag(seat) : nullptr) {
			if (e->type() == QEvent::TouchEnd) {
				finishDrag(drag, false);
			} else if (!points.isEmpty()) {
				const auto &point = points.front();
				sendDragMove(drag, point.globalPosition());
			}
		}
		if (e->type() == QEvent::TouchEnd) {
			_touch = nullptr;
		}
		e->accept();
		return Result::Cancel;
	}
	case QEvent::TouchCancel: {
		if (const auto drag = _touch ? activeDrag(seat) : nullptr) {
			finishDrag(drag, true);
		}
		if (const auto target = targetFor(_touch)) {
			const auto client = target->node->surface()->client();
			seat->sendTouchCancelEvent(client);
		}
		_touch = nullptr;
		return Result::Cancel;
	}
	default:
		return Result::Continue;
	}
}

Compositor::Chrome::Chrome(
		not_null<QWaylandXdgSurface*> xdgSurface,
		not_null<Display*> display,
		base::unique_qptr<QWidget> window)
: _xdgSurface(xdgSurface)
, _display(base::make_weak(display))
, _window(std::move(window)) {
	display->setSurface(xdgSurface->surface());
	trackGeometry(xdgSurface, display);
	if (const auto toplevel = xdgSurface->toplevel()) {
		trackToplevel(toplevel, display);
	}
	if (_window) {
		setupWindow(xdgSurface);
	}
}

Compositor::Chrome::~Chrome() {
	if (const auto display = _display.get()) {
		display->setSurface(nullptr);
	}
}

auto Compositor::Chrome::display() const -> Display* {
	return _display.get();
}

QPoint Compositor::Chrome::geometryPosition() const {
	const auto display = _display.get();
	if (!display || !_xdgSurface->popup()) {
		return QPoint();
	}
	const auto position = WindowGeometry(_xdgSurface).topLeft();
	return (QPointF(position) * display->scale()).toPoint();
}

rpl::producer<> Compositor::Chrome::surfaceCompleted() const {
	return _completed.value()
		| rpl::filter(rpl::mappers::_1)
		| rpl::to_empty;
}

rpl::lifetime &Compositor::Chrome::lifetime() {
	return _lifetime;
}

void Compositor::Chrome::trackGeometry(
		not_null<QWaylandXdgSurface*> xdgSurface,
		not_null<Display*> display) {
	auto geometries = rpl::single(rpl::empty) | rpl::then(
		rpl::merge(
			base::qt_signal_producer(
				xdgSurface->surface(),
				&QWaylandSurface::hasContentChanged),
			base::qt_signal_producer(
				xdgSurface->surface(),
				&QWaylandSurface::destinationSizeChanged),
			base::qt_signal_producer(
				xdgSurface.get(),
				&QWaylandXdgSurface::windowGeometryChanged))
	) | rpl::map([=] {
		return SurfaceGeometry{
			.window = WindowGeometry(xdgSurface),
			.surface = xdgSurface->surface()->destinationSize(),
		};
	}) | rpl::filter([=](const SurfaceGeometry &geometry) {
		return xdgSurface->surface()->hasContent()
			&& geometry.window.isValid()
			&& !geometry.surface.isEmpty();
	}) | rpl::distinct_until_changed();

	rpl::combine(
		std::move(geometries),
		display->scaleValue()
	) | rpl::on_next([=](const SurfaceGeometry &geometry, qreal scale) {
		const auto popup = xdgSurface->popup();
		const auto origin = popup
			? QPoint()
			: -geometry.window.topLeft();
		if (const auto current = _display.get()) {
			current->setOrigin(origin);
		}

		if (_window && popup) {
			updatePopupWindow(geometry, scale);
		} else if (_window) {
			followClient();
		}

		_completed = true;
	}, _lifetime);
}

void Compositor::Chrome::updatePopupWindow(
		const SurfaceGeometry &geometry,
		qreal scale) {
	const auto size = QSizeF(geometry.surface) * scale;
	const auto full = QRect(QPoint(), size.toSize());
	const auto content = QRectF(
		QPointF(geometry.window.topLeft()) * scale,
		QSizeF(geometry.window.size()) * scale).toRect();
	_window->createWinId();
	Ui::Platform::SetWindowMargins(
		_window.get(),
		QMargins(
			content.left(),
			content.top(),
			full.right() - content.right(),
			full.bottom() - content.bottom()));
	_window->resize(full.size());
	_window->windowHandle()->setMask(content);
}

void Compositor::Chrome::trackToplevel(
		not_null<QWaylandXdgToplevel*> toplevel,
		not_null<Display*> display) {
	const auto widget = display->widget();
	const auto sizes = [=] {
		return rpl::combine(
			widget->sizeValue(),
			display->scaleValue()
		) | rpl::map([](const QSize &size, qreal scale) {
			return (QSizeF(size) / scale).toSize();
		});
	};
	auto surfaceSizes = rpl::producer<QSize>();
	if (_window) {
		toplevel->sendFullscreen(QSize(0, 0));
		surfaceSizes = surfaceCompleted(
		) | rpl::take(1) | rpl::map(sizes) | rpl::flatten_latest();
	} else {
		surfaceSizes = sizes();
	}
	const auto weak = QPointer<QWaylandXdgToplevel>(toplevel.get());
	std::move(
		surfaceSizes
	) | rpl::filter([=](const QSize &size) {
		return weak && !size.isEmpty();
	}) | rpl::on_next([=](const QSize &size) {
		if (_following) {
			return;
		}
		weak->sendFullscreen(size);
		if (_window && _window->isVisible()) {
			// Commit only once the client has drawn the new size
			setHeld(true);
		}
	}, _lifetime);

	if (_window) {
		trackSizeLimits(toplevel, display);

		base::qt_signal_producer(
			_xdgSurface->surface(),
			&QWaylandSurface::redraw
		) | rpl::filter([=] { return _held; }) | rpl::on_next([=] {
			followClient();
			setHeld(false);
		}, _lifetime);
	}

	base::qt_signal_producer(
		toplevel.get(),
		&QWaylandXdgToplevel::fullscreenChanged
	) | rpl::map([=] {
		return toplevel->fullscreen();
	}) | rpl::on_next([=](bool fullscreen) {
		const auto current = _display.get();
		if (fullscreen || !current) {
			return;
		}
		const auto raw = current->widget()->rpWidget();
		const auto size = QSizeF(raw->size()) / current->scale();
		toplevel->sendFullscreen(size.toSize());
	}, _lifetime);
}

void Compositor::Chrome::trackSizeLimits(
		not_null<QWaylandXdgToplevel*> toplevel,
		not_null<Display*> display) {
	const auto weak = QPointer<QWaylandXdgToplevel>(toplevel.get());
	rpl::combine(
		rpl::single(rpl::empty) | rpl::then(rpl::merge(
			base::qt_signal_producer(
				toplevel.get(),
				&QWaylandXdgToplevel::minSizeChanged),
			base::qt_signal_producer(
				toplevel.get(),
				&QWaylandXdgToplevel::maxSizeChanged))),
		display->scaleValue()
	) | rpl::filter([=] {
		return weak != nullptr;
	}) | rpl::on_next([=](rpl::empty_value, qreal scale) {
		const auto min = QSizeF(
			weak->minSize().expandedTo(QSize(0, 0))) * scale;
		_window->setMinimumSize(
			int(std::ceil(min.width())),
			int(std::ceil(min.height())));
		const auto max = QSizeF(
			weak->maxSize().expandedTo(QSize(0, 0))) * scale;
		const auto side = [](qreal value) {
			return (value > 0)
				? int(std::floor(value))
				: QWIDGETSIZE_MAX;
		};
		_window->setMaximumSize(
			side(max.width()),
			side(max.height()));
	}, _lifetime);
}

void Compositor::Chrome::setHeld(bool held) {
	_held = held;
	if (const auto display = _display.get()) {
		display->setHeld(held);
	}
}

void Compositor::Chrome::followClient() {
	const auto display = _display.get();
	if (!display) {
		return;
	}
	const auto geometry = QSizeF(WindowGeometry(_xdgSurface).size());
	const auto size = (geometry * display->scale()).toSize();
	if (size.isEmpty() || _window->size() == size) {
		return;
	} else if (_window->isVisible()) {
		setHeld(true);
	}
	_following = true;
	_window->resize(size);
	_following = false;
}

void Compositor::Chrome::setupWindow(
		not_null<QWaylandXdgSurface*> xdgSurface) {
	using Result = base::EventFilterResult;
	const auto toplevel = QPointer(xdgSurface->toplevel());
	const auto popup = QPointer(xdgSurface->popup());
	const auto filter = [=](not_null<QEvent*> e) {
		if (e->type() != QEvent::Close) {
			return Result::Continue;
		} else if (toplevel) {
			e->ignore();
			toplevel->sendClose();
			return Result::Cancel;
		} else if (popup) {
			popup->sendPopupDone();
		}
		return Result::Continue;
	};
	base::install_event_filter(_window.get(), filter);

	if (!toplevel) {
		return;
	}
	rpl::single(rpl::empty) | rpl::then(
		base::qt_signal_producer(
			toplevel.data(),
			&QWaylandXdgToplevel::titleChanged)
	) | rpl::map([=] {
		return toplevel->title();
	}) | rpl::on_next([=](const QString &title) {
		_window->setWindowTitle(title);
	}, _lifetime);
}

auto Compositor::Private::createDisplay(
		QWidget *parent,
		Output *output) -> not_null<Display*> {
	auto renderer = std::make_unique<Display>(compositor, output);
	const auto result = renderer.get();
	const auto backend = gpu
		? Ui::GL::Backend::OpenGL
		: Ui::GL::Backend::Raster;
	auto surface = Ui::GL::CreateSurface(
		parent,
		{ .renderer = std::move(renderer), .backend = backend });
	result->attach(surface.release());
	return result;
}

auto Compositor::Private::addChrome(
		not_null<QWaylandXdgSurface*> xdgSurface,
		not_null<Display*> display,
		base::unique_qptr<QWidget> window) -> not_null<Chrome*> {
	const auto remove = [=] { chromes.take(xdgSurface); };
	QObject::connect(xdgSurface, &QObject::destroyed, &shell, remove);
	auto chrome = std::make_unique<Chrome>(
		xdgSurface,
		display,
		std::move(window));
	const auto result = chrome.get();
	chromes.emplace(xdgSurface, std::move(chrome));
	return result;
}

void Compositor::Private::toplevelCreated(
		not_null<QWaylandXdgSurface*> xdgSurface) {
	const auto embedded = display.get();
	if (embedded && !embedded->hasSurface()) {
		addChrome(xdgSurface, embedded, base::unique_qptr<QWidget>());
		return;
	}
	const auto own = createDisplay(nullptr, nullptr);
	const auto window = own->widget()->rpWidget();
	const auto chrome = addChrome(
		xdgSurface,
		own,
		base::unique_qptr<QWidget>(window));
	chrome->surfaceCompleted(
	) | rpl::on_next([=] { window->show(); }, chrome->lifetime());
	trackParent(xdgSurface->toplevel(), window, chrome->lifetime());
}

void Compositor::Private::trackParent(
		not_null<QWaylandXdgToplevel*> toplevel,
		not_null<QWidget*> window,
		rpl::lifetime &lifetime) {
	rpl::single(rpl::empty) | rpl::then(
		base::qt_signal_producer(
			toplevel.get(),
			&QWaylandXdgToplevel::parentToplevelChanged)
	) | rpl::on_next([=] {
		const auto parent = toplevel->parentToplevel();
		const auto i = parent
			? chromes.find(parent->xdgSurface())
			: chromes.end();
		const auto display = (i != chromes.end())
			? i->second->display()
			: nullptr;
		const auto widget = display
			? display->widget()->rpWidget()
			: nullptr;
		window->createWinId();
		window->windowHandle()->setTransientParent(widget
			? widget->window()->windowHandle()
			: nullptr);
	}, lifetime);

	rpl::single(rpl::empty) | rpl::then(
		base::qt_signal_producer(
			toplevel.get(),
			&QWaylandXdgToplevel::modalChanged)
	) | rpl::on_next([=] {
		window->setWindowModality(toplevel->isModal()
			? Qt::WindowModal
			: Qt::NonModal);
	}, lifetime);
}

void Compositor::Private::popupCreated(
		not_null<QWaylandXdgPopup*> popup,
		not_null<QWaylandXdgSurface*> xdgSurface) {
	const auto i = chromes.find(popup->parentXdgSurface());
	const auto parent = (i != chromes.end())
		? i->second->display()
		: nullptr;
	if (!parent) {
		popup->sendPopupDone();
		return;
	}
	const auto own = createDisplay(
		parent->widget()->rpWidget()->window(),
		parent->output());
	const auto window = own->widget()->rpWidget();
	window->setWindowFlags(Qt::Popup);
	window->setAttribute(Qt::WA_TranslucentBackground);
	own->setParentDisplay(parent);
	const auto chrome = addChrome(
		xdgSurface,
		own,
		base::unique_qptr<QWidget>(window));
	const auto weak = QPointer<QWaylandXdgPopup>(popup.get());
	chrome->surfaceCompleted() | rpl::on_next([=] {
		if (const auto strong = weak.data()) {
			showPopup(strong, chrome);
		}
	}, chrome->lifetime());
}

void Compositor::Private::showPopup(
		not_null<QWaylandXdgPopup*> popup,
		not_null<Chrome*> chrome) {
	const auto i = chromes.find(popup->parentXdgSurface());
	const auto parent = (i != chromes.end()) ? i->second.get() : nullptr;
	const auto display = parent ? parent->display() : nullptr;
	if (!display) {
		return;
	}
	const auto widget = display->widget()->rpWidget();
	const auto scale = display->scale();
	const auto origin = parent->geometryPosition();
	const auto window = chrome->display()->widget()->rpWidget();
#ifndef DESKTOP_APP_DISABLE_PRIVATE_QT_API
	const auto anchorRect = popup->anchorRect();
	const auto anchorSize = QSizeF(anchorRect.size()) * scale;
	const auto anchor = QRect(
		(QPointF(anchorRect.topLeft()) * scale).toPoint(),
		anchorSize.toSize().expandedTo(QSize(1, 1)));
	const auto offset = QPointF(popup->offset()) * scale;
	const auto handle = window->windowHandle();
	handle->setProperty(
		"_q_waylandPopupAnchorRect",
		anchor.translated(widget->mapTo(widget->window(), origin)));
	handle->setProperty(
		"_q_waylandPopupAnchor",
		QVariant::fromValue(popup->anchorEdges()));
	handle->setProperty(
		"_q_waylandPopupGravity",
		QVariant::fromValue(popup->gravityEdges()));
	handle->setProperty(
		"_q_waylandPopupConstraintAdjustment",
		ConstraintAdjustment(popup));
	handle->setProperty("_q_waylandPopupOffset", offset.toPoint());
#endif // !DESKTOP_APP_DISABLE_PRIVATE_QT_API

	const auto position = QPointF(popup->unconstrainedPosition()) * scale;
	window->move(widget->mapToGlobal(origin)
		+ position.toPoint()
		- chrome->geometryPosition());
	window->show();

	const auto sent = chrome->lifetime().make_state<QRect>(
		popup->unconstrainedPosition(),
		popup->positionerSize());
	const auto weak = QPointer<QWaylandXdgPopup>(popup);
	chrome->display()->widget()->geometryValue() | rpl::on_next([=] {
		if (const auto strong = weak.data()) {
			sendPopupPlacement(strong, chrome, sent);
		}
	}, chrome->lifetime());
}

void Compositor::Private::sendPopupPlacement(
		not_null<QWaylandXdgPopup*> popup,
		not_null<Chrome*> chrome,
		not_null<QRect*> sent) {
	const auto i = chromes.find(popup->parentXdgSurface());
	const auto parent = (i != chromes.end()) ? i->second.get() : nullptr;
	const auto display = parent ? parent->display() : nullptr;
	const auto own = chrome->display();
	if (!display || !own) {
		return;
	}
	const auto widget = display->widget()->rpWidget();
	const auto window = own->widget()->rpWidget();
	const auto origin = widget->mapToGlobal(parent->geometryPosition());
	const auto content = window->mapToGlobal(chrome->geometryPosition());
	const auto relative = QPointF(content - origin) / display->scale();
	const auto position = relative.toPoint();

	const auto xdgSurface = popup->xdgSurface();
	const auto geometry = WindowGeometry(xdgSurface);
	const auto surface = xdgSurface->surface()->destinationSize();
	const auto requested = (QSizeF(surface) * own->scale()).toSize();
	const auto shadow = QSizeF(surface - geometry.size());
	const auto size = (window->size() != requested)
		? (QSizeF(window->size()) / own->scale() - shadow).toSize()
		: geometry.size();
	const auto placement = QRect(position, size);
	if (placement == *sent) {
		return;
	}
	*sent = placement;
	popup->sendConfigure(*sent);
}

Compositor::Compositor(const QByteArray &socketName, bool gpu)
: _private(std::make_unique<Private>(this, gpu)) {
	connect(
		&_private->shell,
		&QWaylandXdgShell::toplevelCreated,
		[=](QWaylandXdgToplevel*, QWaylandXdgSurface *xdgSurface) {
			_private->toplevelCreated(xdgSurface);
		});

	connect(
		&_private->shell,
		&QWaylandXdgShell::popupCreated,
		[=](QWaylandXdgPopup *popup, QWaylandXdgSurface *xdgSurface) {
			_private->popupCreated(popup, xdgSurface);
		});

	if (!gpu) {
		const auto filter = [](
				const wl_client*,
				const wl_global *global,
				void*) {
			const auto name = std::string_view(
				wl_global_get_interface(global)->name);
			return name != "wl_drm"
				&& name != "zwp_linux_dmabuf_v1"
				&& name != "wl_eglstream_display";
		};
		wl_display_set_global_filter(display(), filter, nullptr);
	}

	setSocketName(socketName);
	create();

	const auto seat = defaultSeat();
	connect(
		seat->drag(),
		&QWaylandDrag::dragStarted,
		this,
		[=] {
			seat->setMouseFocus(nullptr);
			_private->dragging = true;
		});
}

Compositor::~Compositor() {
	for (const auto client : clients()) {
		destroyClient(client);
	}
	for (const auto output : outputs()) {
		delete output;
	}
}

QWidget *Compositor::createWidget(QWidget *parent) {
	const auto display = _private->createDisplay(parent, nullptr);
	display->setBackground(_private->background);
	_private->display = base::make_weak(display);
	return display->widget()->rpWidget();
}

void Compositor::setBackground(QColor color) {
	_private->background = color;
	if (const auto display = _private->display.get()) {
		display->setBackground(color);
	}
}

} // namespace Webview
