// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/linux/webview_linux_webkitgtk.h"

#include "webview/platform/linux/webview_linux_webkitgtk_library.h"
#include "webview/platform/linux/webview_linux_compositor.h"
#include "webview/platform/linux/webview_linux_http_server.h"
#include "webview/webview_data_stream.h"
#include "base/platform/base_platform_info.h"
#include "base/debug_log.h"
#include "base/integration.h"
#include "base/unique_qptr.h"
#include "base/weak_ptr.h"
#include "base/event_filter.h"
#include "ui/gl/gl_detection.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QUrl>
#include <QtNetwork/QTcpSocket>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#include <QtGui/QtEvents>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>
#ifdef DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
#include <QtQuickWidgets/QQuickWidget>
#endif // DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR

#if __has_include(<giounix/giounix.hpp>)
#include <giounix/giounix.hpp>
#endif // __has_include(<giounix/giounix.hpp>)
#include <webview/webview.hpp>
#include <crl/crl.h>
#include <rpl/rpl.h>
#include <format>

namespace Webview::WebKitGTK {
namespace {

using namespace gi::repository;
using namespace gi::repository::Webview;
using namespace Library;
namespace GObject = gi::repository::GObject;

constexpr auto kObjectPath = "/org/desktop_app/GtkIntegration/Webview";
constexpr auto kMasterObjectPath
	= "/org/desktop_app/GtkIntegration/Webview/Master";
constexpr auto kHelperObjectPath
	= "/org/desktop_app/GtkIntegration/Webview/Helper";
constexpr auto kDataHost = "127.0.0.1";
constexpr auto kExternalShellFallbackBackground = "#eeeeee";
constexpr auto kMaxScriptMessageBytes = 1024 * 1024;
constexpr auto kExternalMessageType = "tdesktop_external_bot_webapp";
constexpr auto kExternalShellSource = "shell";
constexpr auto kMaxPopupAnchorDimension = 32768;
constexpr auto kMaxWaylandPopupAnchorHandleBytes = 4096;

#ifdef DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
void (* const SetGraphicsApi)(QSGRendererInterface::GraphicsApi) =
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	QQuickWindow::setGraphicsApi;
#else // Qt >= 6.0.0
	QQuickWindow::setSceneGraphBackend;
#endif // Qt < 6.0.0
#endif // DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR

std::string SocketPath;

inline auto MethodError() {
	return GLib::Error::new_literal(
		Gio::DBusErrorNS_::quark(),
		int(Gio::DBusError::UNKNOWN_METHOD_),
		"Method does not exist.");
}

inline std::string SocketPathToDBusAddress(const std::string &socketPath) {
	return "unix:path=" + socketPath;
}

enum class ShellControlAction {
	None,
	BeginMove,
	BeginResize,
};

enum class ShellControlParseStatus {
	NotShellControl,
	Invalid,
	Valid,
};

struct ShellControlMessage {
	ShellControlParseStatus status
		= ShellControlParseStatus::NotShellControl;
	ShellControlAction action = ShellControlAction::None;
	QJsonObject arguments;
};

[[nodiscard]] std::optional<double> JsonNumber(
		const QJsonObject &arguments,
		const char *key) {
	const auto value = arguments.value(QString::fromLatin1(key));
	return value.isDouble()
		? std::make_optional(value.toDouble())
		: std::nullopt;
}

[[nodiscard]] std::string JavascriptMessageText(void *message) {
	const auto value = jsc_value_to_string(
		!webkit_javascript_result_get_js_value
			? reinterpret_cast<JSCValue*>(message)
			: webkit_javascript_result_get_js_value(
				reinterpret_cast<WebKitJavascriptResult*>(message)));
	const auto guard = gsl::finally([&] {
		g_free(value);
	});
	return std::string(value);
}

[[nodiscard]] ShellControlAction ShellControlActionFromCommand(
		const QString &command) {
	if (command == "shell_begin_move") {
		return ShellControlAction::BeginMove;
	} else if (command == "shell_begin_resize") {
		return ShellControlAction::BeginResize;
	}
	return ShellControlAction::None;
}

[[nodiscard]] bool IsExternalShellOrigin(const QString &origin) {
	const auto url = QUrl(origin);
	return url.isValid()
		&& url.scheme() == "https"
		&& url.host() == "web.telegram.org"
		&& url.port(443) == 443
		&& url.userInfo().isEmpty()
		&& url.path().isEmpty()
		&& url.query().isEmpty()
		&& url.fragment().isEmpty();
}

[[nodiscard]] ShellControlMessage ParseShellControlMessage(
		const std::string &message,
		const std::string &shellMessageToken) {
	const auto document = QJsonDocument::fromJson(
		QByteArray::fromRawData(message.data(), int(message.size())));
	if (document.isArray()) {
		const auto list = document.array();
		const auto action = ShellControlActionFromCommand(
			list.at(0).toString());
		return (action != ShellControlAction::None)
			? ShellControlMessage{
				.status = ShellControlParseStatus::Invalid,
				.action = action,
			}
			: ShellControlMessage();
	}
	if (!document.isObject()) {
		return {};
	}

	const auto object = document.object();
	const auto action = ShellControlActionFromCommand(
		object.value("eventType").toString());
	if (action == ShellControlAction::None) {
		return {};
	}
	const auto eventData = object.value("eventData");
	if (object.value("type").toString() != kExternalMessageType
		|| object.value("source").toString() != kExternalShellSource
		|| object.value("token").toString().toStdString()
			!= shellMessageToken
		|| !IsExternalShellOrigin(object.value("origin").toString())
		|| (!eventData.isUndefined() && !eventData.isObject())) {
		return ShellControlMessage{
			.status = ShellControlParseStatus::Invalid,
			.action = action,
		};
	}

	return ShellControlMessage{
		.status = ShellControlParseStatus::Valid,
		.action = action,
		.arguments = eventData.isObject()
			? eventData.toObject()
			: QJsonObject(),
	};
}

[[nodiscard]] int ShellControlButton(const QJsonObject &arguments) {
	if (const auto button = JsonNumber(arguments, "gdkButton")) {
		return static_cast<int>(*button);
	} else if (const auto button = JsonNumber(arguments, "button")) {
		const auto value = static_cast<int>(*button);
		return (value >= 0 && value <= 2) ? (value + 1) : value;
	}
	return 1;
}

[[nodiscard]] guint32 ShellControlTimestamp(const QJsonObject &arguments) {
	if (const auto timestamp = JsonNumber(arguments, "timestamp")) {
		return static_cast<guint32>(*timestamp);
	} else if (const auto timestamp = JsonNumber(arguments, "timeStamp")) {
		return static_cast<guint32>(*timestamp);
	} else if (const auto timestamp = JsonNumber(arguments, "time")) {
		return static_cast<guint32>(*timestamp);
	}
	return 0;
}

[[nodiscard]] std::pair<double, double> ShellControlSurfacePosition(
		const QJsonObject &arguments) {
	const auto x = JsonNumber(arguments, "x").value_or(
		JsonNumber(arguments, "clientX").value_or(0.));
	const auto y = JsonNumber(arguments, "y").value_or(
		JsonNumber(arguments, "clientY").value_or(0.));
	return { x, y };
}

[[nodiscard]] std::optional<std::pair<int, int>> ShellControlRootPosition(
		const QJsonObject &arguments) {
	const auto x = JsonNumber(arguments, "rootX").value_or(
		JsonNumber(arguments, "screenX").value_or(-1.));
	const auto y = JsonNumber(arguments, "rootY").value_or(
		JsonNumber(arguments, "screenY").value_or(-1.));
	return (x >= 0.) && (y >= 0.)
		? std::make_optional(std::pair<int, int>{
			static_cast<int>(x),
			static_cast<int>(y),
		})
		: std::nullopt;
}

[[nodiscard]] std::optional<int> ShellControlResizeEdge(
		const QJsonObject &arguments) {
	const auto value = arguments.value("edge");
	if (value.isDouble()) {
		const auto edge = static_cast<int>(value.toDouble(-1));
		return (edge >= 0) && (edge <= 7)
			? std::make_optional(edge)
			: std::nullopt;
	}
	const auto edge = value.toString();
	if (edge == "north_west" || edge == "north-west"
		|| edge == "top_left" || edge == "top-left") {
		return 0;
	} else if (edge == "north" || edge == "top") {
		return 1;
	} else if (edge == "north_east" || edge == "north-east"
		|| edge == "top_right" || edge == "top-right") {
		return 2;
	} else if (edge == "west" || edge == "left") {
		return 3;
	} else if (edge == "east" || edge == "right") {
		return 4;
	} else if (edge == "south_west" || edge == "south-west"
		|| edge == "bottom_left" || edge == "bottom-left") {
		return 5;
	} else if (edge == "south" || edge == "bottom") {
		return 6;
	} else if (edge == "south_east" || edge == "south-east"
		|| edge == "bottom_right" || edge == "bottom-right") {
		return 7;
	}
	return std::nullopt;
}

[[nodiscard]] bool ValidPopupAnchorSize(int width, int height) {
	return (width > 0)
		&& (height > 0)
		&& (width <= kMaxPopupAnchorDimension)
		&& (height <= kMaxPopupAnchorDimension);
}

[[nodiscard]] Ui::Platform::ForeignParent PopupAnchorParent(
		int parentPlatform,
		std::uint64_t x11Id,
		const std::string &waylandHandle) {
	const auto type = Ui::Platform::ForeignParent::Type(parentPlatform);
	switch (type) {
	case Ui::Platform::ForeignParent::Type::None:
		return {};
	case Ui::Platform::ForeignParent::Type::X11:
		return x11Id
			? Ui::Platform::ForeignParent{
				.type = type,
				.x11 = static_cast<uintptr_t>(x11Id),
			}
			: Ui::Platform::ForeignParent();
	case Ui::Platform::ForeignParent::Type::Wayland:
		return (!waylandHandle.empty()
			&& waylandHandle.size() <= kMaxWaylandPopupAnchorHandleBytes)
			? Ui::Platform::ForeignParent{
				.type = type,
				.wayland = QString::fromUtf8(
					waylandHandle.data(),
					int(waylandHandle.size())),
			}
			: Ui::Platform::ForeignParent();
	}
	return {};
}

[[nodiscard]] std::optional<QRect> PopupAnchorGeometry(
		bool hasGeometry,
		int x,
		int y,
		int width,
		int height) {
	return (hasGeometry && ValidPopupAnchorSize(width, height))
		? std::make_optional(QRect(x, y, width, height))
		: std::nullopt;
}

[[nodiscard]] std::optional<QSize> PopupAnchorOuterSize(
		bool hasOuterSize,
		int width,
		int height) {
	return (hasOuterSize && ValidPopupAnchorSize(width, height))
		? std::make_optional(QSize(width, height))
		: std::nullopt;
}

[[nodiscard]] bool IsGdkX11Display(GdkDisplay *display) {
	return display
		&& gdk_x11_display_get_type
		&& GDK_IS_X11_DISPLAY(display);
}

[[nodiscard]] bool IsGdkX11Screen(GdkScreen *screen) {
	return screen
		&& gdk_x11_screen_get_type
		&& GDK_IS_X11_SCREEN(screen);
}

[[nodiscard]] bool IsGdkX11Surface(GdkSurface *surface) {
	return surface
		&& gdk_x11_surface_get_type
		&& GDK_IS_X11_SURFACE(surface);
}

[[nodiscard]] bool IsGdkX11Window(GdkWindow *window) {
	return window
		&& gdk_x11_window_get_type
		&& GDK_IS_X11_WINDOW(window);
}

[[nodiscard]] bool IsGdkWaylandWindow(GdkWindow *window) {
	return window
		&& gdk_wayland_window_get_type
		&& GDK_IS_WAYLAND_WINDOW(window);
}

[[nodiscard]] GdkSurface *GtkNativeSurface(GtkWidget *window) {
	return window
		&& gtk_native_get_surface
		&& gtk_native_get_type
		&& GTK_IS_NATIVE(window)
		? gtk_native_get_surface(GTK_NATIVE(window))
		: nullptr;
}

[[nodiscard]] GdkToplevel *GdkToplevelFromSurface(GdkSurface *surface) {
	return surface
		&& gdk_toplevel_get_type
		&& GDK_IS_TOPLEVEL(surface)
		? GDK_TOPLEVEL(surface)
		: nullptr;
}

[[nodiscard]] GdkToplevel *GdkWaylandToplevelFromSurface(
		GdkSurface *surface) {
	return surface
		&& gdk_wayland_toplevel_get_type
		&& GDK_IS_WAYLAND_TOPLEVEL(surface)
		? GDK_WAYLAND_TOPLEVEL(surface)
		: nullptr;
}

[[nodiscard]] unsigned long X11WindowId(GtkWidget *window) {
	if (!window) {
		return 0;
	}
	const auto isX11Window = [&] {
		if (gtk_widget_get_display) {
			if (const auto display = gtk_widget_get_display(window)) {
				return IsGdkX11Display(display);
			}
		}
		return gtk_widget_get_screen
			&& IsGdkX11Screen(gtk_widget_get_screen(window));
	}();
	if (!isX11Window) {
		return 0;
	}
	if (gtk_native_get_surface && gdk_x11_surface_get_xid) {
		if (const auto surface = GtkNativeSurface(window)) {
			if (IsGdkX11Surface(surface)) {
				if (const auto xid = gdk_x11_surface_get_xid(surface)) {
					return xid;
				}
			}
		}
	}
	if (gtk_widget_get_window && gdk_x11_window_get_xid) {
		if (const auto gdkWindow = gtk_widget_get_window(window)) {
			if (IsGdkX11Window(gdkWindow)) {
				return gdk_x11_window_get_xid(gdkWindow);
			}
		}
	}
	return 0;
}

[[nodiscard]] bool SetupWindowAlpha(GtkWidget *window) {
	if (!window) {
		return false;
	}
	if (gtk_widget_set_visual
		&& gtk_widget_get_screen
		&& gdk_screen_get_rgba_visual) {
		const auto screen = gtk_widget_get_screen(window);
		if (!screen) {
			return false;
		}
		const auto composited = !gdk_screen_is_composited
			|| gdk_screen_is_composited(screen);
		const auto visual = composited
			? gdk_screen_get_rgba_visual(screen)
			: nullptr;
		if (!visual) {
			return false;
		}
		gtk_widget_set_visual(window, visual);
		return true;
	}
	if (gdk_display_is_composited && gtk_widget_get_display) {
		if (const auto display = gtk_widget_get_display(window)) {
			return gdk_display_is_composited(display);
		}
	}
	return true;
}

void SetFrameExtents(GtkWidget *window, const QMargins &margins) {
	if (gdk_window_set_shadow_width && gtk_widget_get_window) {
		if (const auto gdkWindow = gtk_widget_get_window(window)) {
			gdk_window_set_shadow_width(
				gdkWindow,
				std::max(margins.left(), 0),
				std::max(margins.right(), 0),
				std::max(margins.top(), 0),
				std::max(margins.bottom(), 0));
			return;
		}
	}
}

class Instance final : public Interface, public ::base::has_weak_ptr {
public:
	Instance(
		bool remoting = true,
		WindowMode mode = WindowMode::Embedded);
	~Instance();

	bool create(Config config);
	ResolveResult resolve();
	bool startDataServer();

	void resize(int w, int h) override;

	void navigate(std::string url) override;
	void navigateToData(std::string id) override;
	void reload() override;

	void init(std::string js) override;
	void eval(std::string js) override;

	void focus() override;
	void setInteractionHandler(Fn<void()> handler) override;
	void setFullscreen(bool fullscreen) override;

	QWidget *widget() override;
	void *winId() override;
	PopupAnchor popupAnchor() override;

	void refreshNavigationHistoryState() override;
	auto navigationHistoryState()
		-> rpl::producer<NavigationHistoryState> override;

	void setOpaqueBg(QColor opaqueBg) override;

	int exec();

private:
	void scriptMessageReceived(void *message);
	bool handleShellControlMessage(const std::string &message);
	void beginShellMove(const QJsonObject &arguments);
	void beginShellResize(const QJsonObject &arguments);
	[[nodiscard]] bool notifyExternalWindowClosed();
	[[nodiscard]] bool customWindowFrame() const;
	[[nodiscard]] bool transparentWindowBackground() const;
	void announceCustomWindowFrame();
	[[nodiscard]] QMargins windowFrameExtents() const;
	void ensureToplevelFrameExtents();
	void updateWindowFrameExtents();

	bool loadFailed(
		WebKitLoadEvent loadEvent,
		std::string failingUri,
		GLib::Error error);

	void loadChanged(WebKitLoadEvent loadEvent);

	bool decidePolicy(
		WebKitPolicyDecision *decision,
		WebKitPolicyDecisionType decisionType);
	GtkWidget *createAnother(WebKitNavigationAction *action);
	bool scriptDialog(WebKitScriptDialog *dialog);
	void evalNow(std::string js);
	void scheduleQueuedEvals();
	bool authenticate(WebKitAuthenticationRequest *request);

	std::string dataDomain();
	void dataRequest(
		DataResponse resolved,
		QTcpSocket *socket,
		const std::string &resourceId,
		std::int64_t requestedOffset,
		std::int64_t requestedLimit,
		bool headersWritten,
		const std::shared_ptr<HttpServer::Guard> &guard);

	void startProcess();
	void stopProcess();
	void updateHistoryStates();

	void registerMasterMethodHandlers();
	void registerHelperMethodHandlers();
	void scheduleWaylandPopupAnchorExport();
	void ensureWaylandPopupAnchorExport();
	void clearWaylandPopupAnchorExport();
	void setWaylandPopupAnchorFromToplevel(
		std::uint64_t generation,
		GdkToplevel *toplevel,
		QString handle);
	void setWaylandPopupAnchorFromWindow(
		std::uint64_t generation,
		GdkWindow *window,
		QString handle);
	[[nodiscard]] PopupAnchor popupAnchorSnapshot();

	bool _remoting = false;
	WindowMode _mode = WindowMode::Embedded;
	WindowStyle _windowStyle = WindowStyle::Default;
	bool _connected = false;
	Master _master;
	Helper _helper;
	Gio::DBusServer _dbusServer;
	Gio::DBusObjectManagerServer _dbusObjectManager;
	Gio::Subprocess _serviceProcess;

	Platform _platform = Platform::Any;
	Ui::GL::Backend _glBackend;
	::base::unique_qptr<QWidget> _widget;
	QPointer<Compositor> _compositor;
	std::optional<HttpServer> _dataServer;

	GtkWidget *_window = nullptr;
	WebKitWebView *_webview = nullptr;
	GtkCssProvider *_backgroundProvider = nullptr;
	QString _waylandPopupAnchorHandle;
	std::uint64_t _waylandPopupAnchorGeneration = 0;
	bool _waylandPopupAnchorExportScheduled = false;
	bool _waylandPopupAnchorExportAllowed = false;
	bool _waylandPopupAnchorExportPending = false;
	QMargins _windowMargins;
	bool _windowSupportsAlpha = true;
	bool _fullscreen = false;
	GdkToplevel *_frameExtentsToplevel = nullptr;
	gulong _frameExtentsComputeSizeHandler = 0;

	bool _debug = false;
	std::function<void(Message)> _messageHandler;
	std::function<bool(std::string,bool)> _navigationStartHandler;
	std::function<void(bool)> _navigationDoneHandler;
	std::function<void()> _externalWindowCloseHandler;
	std::function<DialogResult(DialogArgs)> _dialogHandler;
	AsyncDialogHandler _asyncDialogHandler;
	rpl::variable<NavigationHistoryState> _navigationHistoryState;
	std::function<DataResult(DataRequest)> _dataRequestHandler;
	Fn<void()> _interactionHandler;
	std::uint16_t _dataPort = 0;
	std::string _dataPassword;
	std::string _shellMessageToken;
	int _scriptDialogDepth = 0;
	std::vector<std::string> _queuedScriptDialogEvals;
	bool _loadFailed = false;
	bool _externalWindowCloseAllowed = false;
	bool _externalWindowClosePending = false;

};

Instance::Instance(bool remoting, WindowMode mode)
: _remoting(remoting)
, _mode(mode) {
	if (_remoting) {
		if (_mode == WindowMode::External) {
			_platform = Platform::Any;
		} else {
			_platform = ::Platform::IsX11()
				? Platform::X11
#ifdef DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
				: Platform::Wayland;
#else // DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
				: Platform::Any;
#endif // !DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
		}
		_glBackend = Ui::GL::ChooseBackendDefault(Ui::GL::CheckCapabilities());
		startProcess();
	}
}

Instance::~Instance() {
	if (_remoting) {
		stopProcess();
	}
	if (_backgroundProvider) {
		g_object_unref(_backgroundProvider);
	}
	if (_window) {
		if (_frameExtentsToplevel && _frameExtentsComputeSizeHandler) {
			g_signal_handler_disconnect(
				_frameExtentsToplevel,
				_frameExtentsComputeSizeHandler);
		}
		clearWaylandPopupAnchorExport();
		if (gtk_window_destroy) {
			gtk_window_destroy(GTK_WINDOW(_window));
		} else {
			gtk_widget_destroy(_window);
		}
	}
}

bool Instance::create(Config config) {
	if (_remoting) {
		const auto resolveResult = resolve();
		if (resolveResult != ResolveResult::Success) {
			LOG(("WebView Error: %1.").arg(
				resolveResult == ResolveResult::NoLibrary
					? "No library"
					: resolveResult == ResolveResult::CantInit
					? "Could not initialize GTK"
					: resolveResult == ResolveResult::IPCFailure
					? "Inter-process communication failure"
					: "Unknown error"));
			return false;
		}

#ifdef DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
		if (_compositor) {
			auto widget = qobject_cast<QQuickWidget*>(_widget);
			if (!widget) {
				[[maybe_unused]] static const auto Inited = [&] {
					switch (_glBackend) {
					case Ui::GL::Backend::Raster:
						SetGraphicsApi(QSGRendererInterface::Software);
						break;
					case Ui::GL::Backend::OpenGL:
						SetGraphicsApi(QSGRendererInterface::OpenGL);
						break;
					}
					return true;
				}();
				_widget = ::base::make_unique_q<QQuickWidget>(config.parent);
				widget = static_cast<QQuickWidget*>(_widget.get());
				_compositor->setWidget(widget);
			}
			widget->setClearColor(config.opaqueBg);
			widget->show();
			const auto since = crl::now();
			while (crl::now() - since < 1000) {
				_compositor->processWaylandEvents();
				GLib::MainContext::default_().iteration(false);
			}
		}
#else // DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
		if (_compositor) {
			_platform = Platform::Any;
			stopProcess();
			startProcess();
			return create(std::move(config));
		}
#endif // !DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
	}

	_debug = config.debug;
	_messageHandler = std::move(config.messageHandler);
	_navigationStartHandler = std::move(config.navigationStartHandler);
	_navigationDoneHandler = std::move(config.navigationDoneHandler);
	_externalWindowCloseHandler = std::move(config.externalWindowCloseHandler);
	_dialogHandler = std::move(config.dialogHandler);
	_asyncDialogHandler = std::move(config.asyncDialogHandler);
	_dataRequestHandler = std::move(config.dataRequestHandler);
	_windowStyle = config.windowStyle;
	_windowMargins = config.windowMargins;
	_shellMessageToken = std::move(config.shellMessageToken);

	if (_remoting) {
		if (!_helper) {
			return false;
		}

		const ::base::has_weak_ptr guard;
		std::optional<bool> success;
		const auto debug = _debug;
		const auto r = config.opaqueBg.red();
		const auto g = config.opaqueBg.green();
		const auto b = config.opaqueBg.blue();
		const auto a = config.opaqueBg.alpha();
		const auto path = config.userDataPath;
		const auto mode = int(config.mode);
		const auto windowStyle = int(config.windowStyle);
		const auto shellMessageToken = _shellMessageToken;
		const auto margins = config.windowMargins;
		const auto initialSize = config.initialSize;
		_helper.call_create(
			debug,
			r,
			g,
			b,
			a,
			path,
			mode,
			windowStyle,
			shellMessageToken,
			margins.left(),
			margins.right(),
			margins.top(),
			margins.bottom(),
			initialSize.width(),
			initialSize.height(),
			crl::guard(&guard, [&](
					GObject::Object source_object,
					Gio::AsyncResult res) {
				success = _helper.call_create_finish(res, nullptr);
				GLib::MainContext::default_().wakeup();
			}));

		while (!success && _connected) {
			GLib::MainContext::default_().iteration(true);
		}

		if (!success.value_or(false)) {
			return false;
		}

		const auto createPlaceholder = [&](bool forwardResize) {
			_widget = ::base::make_unique_q<QWidget>(config.parent);
			if (forwardResize) {
				::base::install_event_filter(_widget, [=](
						not_null<QEvent*> e) {
					if (e->type() == QEvent::Resize) {
						const auto size = static_cast<QResizeEvent*>(
							e.get()
						)->size();
						resize(size.width(), size.height());
					}
					return ::base::EventFilterResult::Continue;
				});
			}
			if (_mode != WindowMode::External) {
				_widget->show();
			}
		};

		switch (_platform) {
		case Platform::Any:
			createPlaceholder(_mode != WindowMode::External);
			break;
		case Platform::X11:
			if (_mode == WindowMode::External) {
				createPlaceholder(false);
				break;
			}
			const auto window = QPointer(QWindow::fromWinId(WId(winId())));
			::base::install_event_filter(window, [=](
					not_null<QEvent*> e) {
				if (e->type() == QEvent::Show) {
					GLib::timeout_add_seconds_once(1, crl::guard(window, [=] {
						const auto size = window->size();
						window->resize(0, 0);
						window->resize(size);
					}));
				}
				return ::base::EventFilterResult::Continue;
			});
			_widget.reset(
				QWidget::createWindowContainer(
					window,
					config.parent,
					Qt::FramelessWindowHint));
			_widget->show();
			break;
		}

		return true;
	}

	_window = (_platform == Platform::X11)
		&& (_mode != WindowMode::External)
		? gtk_plug_new(0)
		: gtk_window_new(GTK_WINDOW_TOPLEVEL);
	if (_mode == WindowMode::External) {
		if (customWindowFrame()) {
			gtk_window_set_decorated(GTK_WINDOW(_window), FALSE);
		}
		if (config.initialSize.width() > 0 && config.initialSize.height() > 0) {
			gtk_window_set_default_size(
				GTK_WINDOW(_window),
				config.initialSize.width(),
				config.initialSize.height());
		}
		const auto windowType = G_OBJECT_TYPE(_window);
		if (g_signal_lookup("close-request", windowType)) {
			g_signal_connect_swapped(
				_window,
				"close-request",
				G_CALLBACK(+[](Instance *instance) -> gboolean {
					return instance->notifyExternalWindowClosed();
				}),
				this);
		} else if (g_signal_lookup("delete-event", windowType)) {
			g_signal_connect_swapped(
				_window,
				"delete-event",
				G_CALLBACK(+[](Instance *instance, GdkEvent*) -> gboolean {
					return instance->notifyExternalWindowClosed();
				}),
				this);
		}
	}
	const auto customPainting = (_mode != WindowMode::External)
		|| customWindowFrame();
	if (customPainting && gtk_widget_set_app_paintable) {
		gtk_widget_set_app_paintable(_window, TRUE);
	}
	_windowSupportsAlpha = customPainting ? SetupWindowAlpha(_window) : false;
	if (customPainting) {
		if (gtk_widget_add_css_class) {
			gtk_widget_add_css_class(_window, "webviewWindow");
		} else {
			gtk_style_context_add_class(
				gtk_widget_get_style_context(_window),
				"webviewWindow");
		}
	}
	_backgroundProvider = gtk_css_provider_new();
	if (gtk_style_context_add_provider_for_display) {
		gtk_style_context_add_provider_for_display(
			gtk_widget_get_display(_window),
			GTK_STYLE_PROVIDER(_backgroundProvider),
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	} else {
		gtk_style_context_add_provider_for_screen(
			gtk_widget_get_screen(_window),
			GTK_STYLE_PROVIDER(_backgroundProvider),
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	}
	setOpaqueBg(config.opaqueBg);

	const auto base = config.userDataPath;
	const auto baseCache = base + "/cache";
	const auto baseData = base + "/data";

	if (webkit_network_session_new) {
		WebKitNetworkSession *session = webkit_network_session_new(
			baseData.c_str(),
			baseCache.c_str());
		_webview = WEBKIT_WEB_VIEW(g_object_new(
			WEBKIT_TYPE_WEB_VIEW,
			"network-session",
			session,
			nullptr));
		g_object_unref(session);
	} else {
		WebKitWebsiteDataManager *data = webkit_website_data_manager_new(
			"base-cache-directory", baseCache.c_str(),
			"base-data-directory", baseData.c_str(),
			nullptr);
		WebKitWebContext *context
			= webkit_web_context_new_with_website_data_manager(data);
		g_object_unref(data);

		_webview = WEBKIT_WEB_VIEW(webkit_web_view_new_with_context(context));
		g_object_unref(context);
	}

	WebKitUserContentManager *manager =
		webkit_web_view_get_user_content_manager(_webview);
	g_signal_connect_swapped(
		manager,
		"script-message-received::external",
		G_CALLBACK(+[](
			Instance *instance,
			void *message) {
			instance->scriptMessageReceived(message);
		}),
		this);
	g_signal_connect_swapped(
		_window,
		"destroy",
		G_CALLBACK(+[](Instance *instance) {
			instance->clearWaylandPopupAnchorExport();
			instance->_window = nullptr;
			Gio::Application::get_default().quit();
		}),
		this);
	g_signal_connect_swapped(
		_window,
		"realize",
		G_CALLBACK(+[](Instance *instance) {
			instance->announceCustomWindowFrame();
			instance->updateWindowFrameExtents();
		}),
		this);
	g_signal_connect_swapped(
		_window,
		"map",
		G_CALLBACK(+[](Instance *instance) {
			instance->scheduleWaylandPopupAnchorExport();
		}),
		this);
	g_signal_connect_swapped(
		_window,
		"unmap",
		G_CALLBACK(+[](Instance *instance) {
			instance->_waylandPopupAnchorExportAllowed = false;
			instance->clearWaylandPopupAnchorExport();
		}),
		this);
	g_signal_connect_swapped(
		_webview,
		"web-process-terminated",
		G_CALLBACK(+[](
				Instance *instance,
				WebKitWebProcessTerminationReason reason) {
			LOG(("WebView Error: Web process terminated: %1.").arg(
				int(reason)));
			Gio::Application::get_default().quit();
		}),
		this);
	g_signal_connect_swapped(
		_webview,
		"notify::is-web-process-responsive",
		G_CALLBACK(+[](
				Instance *instance,
				GParamSpec *pspec) {
			if (!webkit_web_view_get_is_web_process_responsive(
					instance->_webview)) {
				LOG(("WebView Error: Web process became unresponsive."));
				Gio::Application::get_default().quit();
			}
		}),
		this);
	g_signal_connect_swapped(
		_webview,
		"load-failed",
		G_CALLBACK(+[](
			Instance *instance,
			WebKitLoadEvent loadEvent,
			char *failingUri,
			GError *error) -> gboolean {
			return instance->loadFailed(
				loadEvent,
				failingUri,
				GLib::Error(g_error_copy(error)));
		}),
		this);
	g_signal_connect_swapped(
		_webview,
		"load-changed",
		G_CALLBACK(+[](
			Instance *instance,
			WebKitLoadEvent loadEvent) {
			instance->loadChanged(loadEvent);
		}),
		this);
	g_signal_connect_swapped(
		_webview,
		"notify::uri",
		G_CALLBACK(+[](
			Instance *instance,
			GParamSpec *pspec) {
			instance->updateHistoryStates();
		}),
		this);
	g_signal_connect_swapped(
		_webview,
		"notify::title",
		G_CALLBACK(+[](
			Instance *instance,
			GParamSpec *pspec) {
			instance->updateHistoryStates();
		}),
		this);
	g_signal_connect_swapped(
		_webview,
		"decide-policy",
		G_CALLBACK(+[](
			Instance *instance,
			WebKitPolicyDecision *decision,
			WebKitPolicyDecisionType decisionType) -> gboolean {
			return instance->decidePolicy(decision, decisionType);
		}),
		this);
	g_signal_connect_swapped(
		_webview,
		"create",
		G_CALLBACK(+[](
			Instance *instance,
			WebKitNavigationAction *action) -> GtkWidget* {
			return instance->createAnother(action);
		}),
		this);
	g_signal_connect_swapped(
		_webview,
		"script-dialog",
		G_CALLBACK(+[](
			Instance *instance,
			WebKitScriptDialog *dialog) -> gboolean {
			return instance->scriptDialog(dialog);
		}),
		this);
	g_signal_connect_swapped(
		_webview,
		"authenticate",
		G_CALLBACK(+[](
			Instance *instance,
			WebKitAuthenticationRequest *request) -> gboolean {
			return instance->authenticate(request);
		}),
		this);
	if (gtk_widget_add_controller
		&& gtk_gesture_click_new
		&& gtk_event_controller_key_new
		&& gtk_event_controller_get_type) {
		const auto click = gtk_gesture_click_new();
		g_signal_connect_swapped(
			click,
			"pressed",
			G_CALLBACK(+[](
				Instance *instance,
				int,
				double,
				double) {
				if (instance->_master) {
					instance->_master.call_user_interaction(nullptr);
				}
			}),
			this);
		gtk_widget_add_controller(
			GTK_WIDGET(_webview),
			GTK_EVENT_CONTROLLER(click));
		const auto key = gtk_event_controller_key_new();
		g_signal_connect_swapped(
			key,
			"key-pressed",
			G_CALLBACK(+[](
				Instance *instance,
				guint,
				guint,
				GdkModifierType) -> gboolean {
				if (instance->_master) {
					instance->_master.call_user_interaction(nullptr);
				}
				return FALSE;
			}),
			this);
		gtk_widget_add_controller(
			GTK_WIDGET(_webview),
			key);
	} else {
		g_signal_connect_swapped(
			_webview,
			"button-press-event",
			G_CALLBACK(+[](
				Instance *instance,
				GdkEventButton*) -> gboolean {
				if (instance->_master) {
					instance->_master.call_user_interaction(nullptr);
				}
				return FALSE;
			}),
			this);
		g_signal_connect_swapped(
			_webview,
			"key-press-event",
			G_CALLBACK(+[](
				Instance *instance,
				GdkEventKey*) -> gboolean {
				if (instance->_master) {
					instance->_master.call_user_interaction(nullptr);
				}
				return FALSE;
			}),
			this);
	}
	webkit_user_content_manager_register_script_message_handler(
		manager,
		"external",
		nullptr);
	init(std::string("window.TelegramDesktopWindowAlphaSupported = ")
		+ (_windowSupportsAlpha ? "true" : "false")
		+ ";");
	const auto fallback = customWindowFrame() && !_windowSupportsAlpha;
	const GdkRGBA rgba = fallback
		? GdkRGBA{ 238.f / 255.f, 238.f / 255.f, 238.f / 255.f, 1.f }
		: transparentWindowBackground()
		? GdkRGBA{ 0.f, 0.f, 0.f, 0.f }
		: GdkRGBA{
			float(config.opaqueBg.redF()),
			float(config.opaqueBg.greenF()),
			float(config.opaqueBg.blueF()),
			float(config.opaqueBg.alphaF()) };
	webkit_web_view_set_background_color(_webview, &rgba);
	if (_debug) {
		WebKitSettings *settings = webkit_web_view_get_settings(_webview);
		webkit_settings_set_enable_developer_extras(settings, true);
	}
	if (gtk_window_set_child) {
		gtk_window_set_child(GTK_WINDOW(_window), GTK_WIDGET(_webview));
	} else if (_platform == Platform::X11) {
		const auto x11SizeFix = gtk_scrolled_window_new(nullptr, nullptr);
		if (gtk_scrolled_window_set_shadow_type) {
			gtk_scrolled_window_set_shadow_type(
				x11SizeFix,
				GTK_SHADOW_NONE);
		}
		gtk_container_add(GTK_CONTAINER(x11SizeFix), GTK_WIDGET(_webview));
		gtk_container_add(GTK_CONTAINER(_window), x11SizeFix);
	} else {
		gtk_container_add(GTK_CONTAINER(_window), GTK_WIDGET(_webview));
	}
	if (!gtk_widget_show_all) {
		gtk_widget_set_visible(_window, true);
	} else {
		gtk_widget_show_all(_window);
	}
	updateWindowFrameExtents();
	init(R"(
if (window === window.top) {
	const external = Object.freeze({
		invoke: function(s) {
			window.webkit.messageHandlers.external.postMessage(s);
		}
	});
	Object.defineProperty(window, 'external', {
		value: external,
		configurable: false,
		writable: false
	});
})");

	return webkit_web_view_get_is_web_process_responsive(_webview);
}

void Instance::scriptMessageReceived(void *message) {
	const auto text = JavascriptMessageText(message);
	if (text.size() > kMaxScriptMessageBytes) {
		return;
	}
	if (handleShellControlMessage(text)) {
		return;
	}
	if (!_master) {
		return;
	}
	_master.call_message_received(text, nullptr);
}

bool Instance::handleShellControlMessage(const std::string &message) {
	if (_mode != WindowMode::External) {
		return false;
	}
	const auto parsed = ParseShellControlMessage(message, _shellMessageToken);
	if (parsed.status == ShellControlParseStatus::NotShellControl) {
		return false;
	} else if (parsed.status == ShellControlParseStatus::Invalid
		|| !_window
		|| _shellMessageToken.empty()) {
		return true;
	}
	switch (parsed.action) {
	case ShellControlAction::BeginMove:
		beginShellMove(parsed.arguments);
		return true;
	case ShellControlAction::BeginResize:
		beginShellResize(parsed.arguments);
		return true;
	case ShellControlAction::None:
		return false;
	}
	return false;
}

void Instance::beginShellMove(const QJsonObject &arguments) {
	if (gdk_toplevel_begin_move && gtk_native_get_surface) {
		if (const auto toplevel = GdkToplevelFromSurface(
				GtkNativeSurface(_window))) {
			const auto [x, y] = ShellControlSurfacePosition(arguments);
			gdk_toplevel_begin_move(
				toplevel,
				nullptr,
				ShellControlButton(arguments),
				x,
				y,
				ShellControlTimestamp(arguments));
			return;
		}
	}
	if (gtk_window_begin_move_drag) {
		if (const auto position = ShellControlRootPosition(arguments)) {
			gtk_window_begin_move_drag(
				GTK_WINDOW(_window),
				ShellControlButton(arguments),
				position->first,
				position->second,
				ShellControlTimestamp(arguments));
		}
	}
}

void Instance::beginShellResize(const QJsonObject &arguments) {
	const auto edge = ShellControlResizeEdge(arguments);
	if (!edge) {
		return;
	}
	if (gdk_toplevel_begin_resize && gtk_native_get_surface) {
		if (const auto toplevel = GdkToplevelFromSurface(
				GtkNativeSurface(_window))) {
			const auto [x, y] = ShellControlSurfacePosition(arguments);
			gdk_toplevel_begin_resize(
				toplevel,
				static_cast<GdkSurfaceEdge>(*edge),
				nullptr,
				ShellControlButton(arguments),
				x,
				y,
				ShellControlTimestamp(arguments));
			return;
		}
	}
	if (gtk_window_begin_resize_drag) {
		if (const auto position = ShellControlRootPosition(arguments)) {
			gtk_window_begin_resize_drag(
				GTK_WINDOW(_window),
				static_cast<GdkWindowEdge>(*edge),
				ShellControlButton(arguments),
				position->first,
				position->second,
				ShellControlTimestamp(arguments));
		}
	}
}

bool Instance::customWindowFrame() const {
	return (_mode == WindowMode::External)
		&& (_windowStyle == WindowStyle::Frameless);
}

bool Instance::transparentWindowBackground() const {
	return _windowSupportsAlpha
		&& (customWindowFrame()
			|| (_mode != WindowMode::External
				&& _platform == Platform::Wayland));
}

void Instance::announceCustomWindowFrame() {
	if (!customWindowFrame()
		|| !gtk_widget_get_window
		|| !gdk_wayland_window_announce_csd) {
		return;
	}
	if (const auto gdkWindow = gtk_widget_get_window(_window);
		IsGdkWaylandWindow(gdkWindow)) {
		gdk_wayland_window_announce_csd(gdkWindow);
	}
}

QMargins Instance::windowFrameExtents() const {
	return (!_fullscreen && customWindowFrame() && _windowSupportsAlpha)
		? _windowMargins
		: QMargins();
}

void Instance::ensureToplevelFrameExtents() {
	if (!gdk_toplevel_size_set_shadow_width
		|| !gtk_native_get_surface
		|| !_window) {
		return;
	}
	const auto toplevel = GdkToplevelFromSurface(GtkNativeSurface(_window));
	if (!toplevel || toplevel == _frameExtentsToplevel) {
		return;
	}
	if (_frameExtentsToplevel && _frameExtentsComputeSizeHandler) {
		g_signal_handler_disconnect(
			_frameExtentsToplevel,
			_frameExtentsComputeSizeHandler);
	}
	_frameExtentsToplevel = toplevel;
	_frameExtentsComputeSizeHandler = g_signal_connect_after(
		toplevel,
		"compute-size",
		G_CALLBACK(+[](
				GdkToplevel*,
				GdkToplevelSize *size,
				Instance *instance) {
			const auto margins = instance->windowFrameExtents();
			gdk_toplevel_size_set_shadow_width(
				size,
				std::max(margins.left(), 0),
				std::max(margins.right(), 0),
				std::max(margins.top(), 0),
				std::max(margins.bottom(), 0));
		}),
		this);
}

void Instance::updateWindowFrameExtents() {
	if (!customWindowFrame() || !_window) {
		return;
	}
	ensureToplevelFrameExtents();
	SetFrameExtents(_window, windowFrameExtents());
}

bool Instance::loadFailed(
		WebKitLoadEvent loadEvent,
		std::string failingUri,
		GLib::Error error) {
	_loadFailed = true;
	return false;
}

void Instance::loadChanged(WebKitLoadEvent loadEvent) {
	if (loadEvent == WEBKIT_LOAD_STARTED) {
		_loadFailed = false;
	} else if (loadEvent == WEBKIT_LOAD_FINISHED) {
		if (_master) {
			_master.call_navigation_done(!_loadFailed, nullptr);
		}
	}
	updateHistoryStates();
}

bool Instance::decidePolicy(
		WebKitPolicyDecision *decision,
		WebKitPolicyDecisionType decisionType) {
	if (decisionType != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
		return false;
	}
	WebKitNavigationPolicyDecision *navigationDecision
		= WEBKIT_NAVIGATION_POLICY_DECISION(decision);
	WebKitNavigationAction *action
		= webkit_navigation_policy_decision_get_navigation_action(
			navigationDecision);
	WebKitURIRequest *request = webkit_navigation_action_get_request(action);
	const gchar *uri = webkit_uri_request_get_uri(request);
	bool result = false;
	if (_master) {
		auto loop = GLib::MainLoop::new_();
		_master.call_navigation_started(uri, false, [&](
				GObject::Object source_object,
				Gio::AsyncResult res) {
			if (const auto ret = _master.call_navigation_started_finish(
					res)) {
				result = std::get<1>(*ret);
			}
			loop.quit();
		});
		loop.run();
	}
	if (!result) {
		webkit_policy_decision_ignore(decision);
	}
	GLib::timeout_add_seconds_once(1, crl::guard(this, [=] {
		if (!webkit_web_view_is_loading(_webview)) {
			if (_master) {
				_master.call_navigation_done(!_loadFailed, nullptr);
			}
		}
	}));
	return !result;
}

GtkWidget *Instance::createAnother(WebKitNavigationAction *action) {
	WebKitURIRequest *request = webkit_navigation_action_get_request(action);
	const gchar *uri = webkit_uri_request_get_uri(request);
	if (_master) {
		_master.call_navigation_started(uri, true, nullptr);
	}
	return nullptr;
}

bool Instance::scriptDialog(WebKitScriptDialog *dialog) {
	const auto type = webkit_script_dialog_get_dialog_type(dialog);
	const auto text = webkit_script_dialog_get_message(dialog);
	const auto value = (type == WEBKIT_SCRIPT_DIALOG_PROMPT)
		? webkit_script_dialog_prompt_get_default_text(dialog)
		: nullptr;
	bool accepted = false;
	std::string result;
	if (_master) {
		auto loop = GLib::MainLoop::new_();
		++_scriptDialogDepth;
		const auto guard = gsl::finally([&] {
			if (--_scriptDialogDepth == 0) {
				scheduleQueuedEvals();
			}
		});
		_master.call_script_dialog(
			type,
			text ? text : "",
			value ? value : "",
			[&](GObject::Object source_object, Gio::AsyncResult res) {
				if (const auto ret = _master.call_script_dialog_finish(res)) {
					std::tie(std::ignore, accepted, result) = *ret;
				}
				loop.quit();
			});
		loop.run();
	}
	if (type == WEBKIT_SCRIPT_DIALOG_PROMPT) {
		webkit_script_dialog_prompt_set_text(
			dialog,
			accepted ? result.c_str() : nullptr);
	} else if (type != WEBKIT_SCRIPT_DIALOG_ALERT) {
		webkit_script_dialog_confirm_set_confirmed(dialog, false);
	}
	return true;
}

bool Instance::authenticate(WebKitAuthenticationRequest *request) {
	if (strcmp(webkit_authentication_request_get_host(request), kDataHost)
			|| webkit_authentication_request_get_port(request) != _dataPort) {
		return false;
	}
	const auto credential = webkit_credential_new(
		"",
		_dataPassword.c_str(),
		WEBKIT_CREDENTIAL_PERSISTENCE_FOR_SESSION);
	webkit_authentication_request_authenticate(request, credential);
	webkit_credential_free(credential);
	return true;
}

// https://bugs.webkit.org/show_bug.cgi?id=146351
bool Instance::startDataServer() {
	if (_dataServer) {
		return true;
	}

	_dataServer.emplace(
		(_dataPassword = GLib::uuid_string_random()).c_str(),
		[=](
				QTcpSocket *socket,
				const QByteArray &id,
				const ::base::flat_map<QByteArray, QByteArray> &headers,
				const std::shared_ptr<HttpServer::Guard> &guard) {
			if (!_dataRequestHandler) {
				return;
			}
			const auto resourceId = id.toStdString();
			auto prepared = DataRequest{
				.id = resourceId,
			};
			const auto getHeader = [&](const QByteArray &key) {
				const auto it = headers.find(key);
				return it != headers.end()
					? it->second
					: QByteArray();
			};
			const auto rangeHeader = getHeader("Range");
			if (!rangeHeader.isEmpty()) {
				ParseRangeHeaderFor(prepared, rangeHeader.toStdString());
			}
			const auto requestedOffset = prepared.offset;
			const auto requestedLimit = prepared.limit;
			prepared.done = crl::guard(socket, [=](DataResponse resolved) {
				dataRequest(
					std::move(resolved),
					socket,
					resourceId,
					requestedOffset,
					requestedLimit,
					false,
					guard);
			});
			_dataRequestHandler(prepared);
		});

	if (!_dataServer->listen(QHostAddress::LocalHost)) {
		LOG(("WebView Error: %1").arg(_dataServer->errorString()));
		_dataServer.reset();
		return false;
	}

	_dataPort = _dataServer->serverPort();

	if (_master) {
		_master.emit_data_server_started(_dataPort, _dataPassword);
	}

	return true;
}

std::string Instance::dataDomain() {
	return std::format("http://{}:{}/", kDataHost, std::to_string(_dataPort));
}

void Instance::dataRequest(
		DataResponse resolved,
		QTcpSocket *socket,
		const std::string &resourceId,
		std::int64_t requestedOffset,
		std::int64_t requestedLimit,
		bool headersWritten,
		const std::shared_ptr<HttpServer::Guard> &guard) {
	auto &stream = resolved.stream;
	if (!stream) {
		return;
	}
	const auto length = stream->size();
	Assert(length > 0);

	const auto offset = resolved.streamOffset;
	if (requestedOffset >= offset + length || offset > requestedOffset) {
		return;
	}

	auto bytes = QByteArray();
	bytes.resize(length);
	const auto read = stream->read(bytes.data(), length);
	Assert(read == length);

	const auto useOffset = (requestedOffset - offset);
	const auto useLength = (requestedLimit > 0)
		? std::min(requestedLimit, (length - useOffset))
		: (length - useOffset);

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
	bytes.slice(useOffset, useLength);
#else // Qt >= 6.8.0
	bytes = std::move(bytes.mid(useOffset, useLength));
#endif // Qt < 6.8.0

	const auto total = resolved.totalSize ? resolved.totalSize : length;
	const auto partial = (requestedOffset > 0) || (requestedLimit > 0);
	if (requestedLimit <= 0) {
		requestedLimit = (total - requestedOffset);
	}

	if (!headersWritten) {
		socket->write("HTTP/1.1 ");
		socket->write(partial ? "206 Partial Content\r\n" : "200 OK\r\n");

		const auto mime = QByteArray(stream->mime());
		socket->write("Content-Type: " + mime + "\r\n");
		socket->write("Accept-Ranges: bytes\r\n");
		socket->write("Cache-Control: no-store\r\n");
		socket->write("Content-Length: "
			+ QByteArray::number(requestedLimit)
			+ "\r\n");

		if (partial) {
			socket->write("Content-Range: bytes "
				+ QByteArray::number(requestedOffset)
				+ '-'
				+ QByteArray::number(requestedOffset + requestedLimit - 1)
				+ '/'
				+ QByteArray::number(total)
				+ "\r\n");
		}

		socket->write("\r\n");
		headersWritten = true;
	}

	socket->write(bytes);
	if (requestedLimit == useLength) {
		return;
	}

	requestedOffset += useLength;
	requestedLimit -= useLength;

	_dataRequestHandler({
		.id = resourceId,
		.offset = requestedOffset,
		.limit = requestedLimit,
		.done = crl::guard(socket, [=](DataResponse resolved) {
			dataRequest(
				std::move(resolved),
				socket,
				resourceId,
				requestedOffset,
				requestedLimit,
				headersWritten,
				guard);
		}),
	});
}

ResolveResult Instance::resolve() {
	if (_remoting) {
		if (!_helper) {
			return ResolveResult::IPCFailure;
		}

		const ::base::has_weak_ptr guard;
		std::optional<ResolveResult> result;
		_helper.call_resolve(int(_mode), crl::guard(&guard, [&](
				GObject::Object source_object,
				Gio::AsyncResult res) {
			const auto reply = _helper.call_resolve_finish(res);
			if (reply) {
				result = ResolveResult(std::get<1>(*reply));
			}
			GLib::MainContext::default_().wakeup();
		}));

		while (!result && _connected) {
			GLib::MainContext::default_().iteration(true);
		}

		if (_platform != Platform::Any
				&& result
				&& *result != ResolveResult::Success) {
			_platform = Platform::Any;
			stopProcess();
			startProcess();
			return resolve();
		}

		return result.value_or(ResolveResult::IPCFailure);
	}

	return Resolve(_platform, _mode);
}

void Instance::navigate(std::string url) {
	if (_remoting) {
		if (!_helper) {
			return;
		}

		_helper.call_navigate(url, nullptr);
		return;
	}

	webkit_web_view_load_uri(_webview, url.c_str());
}

void Instance::navigateToData(std::string id) {
	startDataServer();
	navigate(dataDomain() + id);
}

void Instance::reload() {
	if (_remoting) {
		if (!_helper) {
			return;
		}

		_helper.call_reload(nullptr);
		return;
	}

	webkit_web_view_reload_bypass_cache(_webview);
}

void Instance::init(std::string js) {
	if (_remoting) {
		if (!_helper) {
			return;
		}

		_helper.call_init(js, nullptr);
		return;
	}

	WebKitUserContentManager *manager
		= webkit_web_view_get_user_content_manager(_webview);
	webkit_user_content_manager_add_script(
		manager,
		webkit_user_script_new(
			js.c_str(),
			WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
			WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
			nullptr,
			nullptr));
}

void Instance::eval(std::string js) {
	if (_remoting) {
		if (!_helper) {
			return;
		}

		_helper.call_eval(js, nullptr);
		return;
	}

	if (_scriptDialogDepth > 0) {
		_queuedScriptDialogEvals.push_back(std::move(js));
		return;
	}
	evalNow(std::move(js));
}

void Instance::evalNow(std::string js) {
	if (webkit_web_view_evaluate_javascript) {
		webkit_web_view_evaluate_javascript(
			_webview,
			js.c_str(),
			-1,
			nullptr,
			nullptr,
			nullptr,
			nullptr,
			nullptr);
	} else {
		webkit_web_view_run_javascript(
			_webview,
			js.c_str(),
			nullptr,
			nullptr,
			nullptr);
	}
}

void Instance::scheduleQueuedEvals() {
	if (_queuedScriptDialogEvals.empty()) {
		return;
	}
	struct QueuedEvals {
		::base::weak_ptr<Instance> instance;
		std::vector<std::string> scripts;
	};
	auto queued = std::make_unique<QueuedEvals>();
	queued->instance = this;
	queued->scripts = std::move(_queuedScriptDialogEvals);
	_queuedScriptDialogEvals = {};
	g_idle_add_full(
		G_PRIORITY_DEFAULT_IDLE,
		+[](gpointer userData) -> gboolean {
			const auto queued = std::unique_ptr<QueuedEvals>(
				static_cast<QueuedEvals*>(userData));
			if (const auto instance = queued->instance.get()) {
				for (auto &script : queued->scripts) {
					instance->eval(std::move(script));
				}
			}
			return G_SOURCE_REMOVE;
		},
		queued.release(),
		nullptr);
}

void Instance::focus() {
	if (const auto widget = _widget.get()) {
		widget->activateWindow();
	}
}

void Instance::setInteractionHandler(Fn<void()> handler) {
	_interactionHandler = std::move(handler);
}

QWidget *Instance::widget() {
	return _widget.get();
}

void Instance::scheduleWaylandPopupAnchorExport() {
	struct WaylandPopupAnchorSchedule {
		::base::weak_ptr<Instance> instance;
	};
	if (!_window
		|| _waylandPopupAnchorExportAllowed
		|| _waylandPopupAnchorExportScheduled
		|| _waylandPopupAnchorExportPending
		|| !_waylandPopupAnchorHandle.isEmpty()) {
		return;
	}
	_waylandPopupAnchorExportScheduled = true;
	const auto schedule = new WaylandPopupAnchorSchedule{
		.instance = this,
	};
	g_idle_add_full(
		G_PRIORITY_DEFAULT_IDLE,
		+[](gpointer userData) -> gboolean {
			const auto schedule = std::unique_ptr<WaylandPopupAnchorSchedule>(
				static_cast<WaylandPopupAnchorSchedule*>(userData));
			if (const auto instance = schedule->instance.get()) {
				if (!instance->_waylandPopupAnchorExportScheduled) {
					return G_SOURCE_REMOVE;
				}
				instance->_waylandPopupAnchorExportScheduled = false;
				instance->_waylandPopupAnchorExportAllowed = true;
				instance->ensureWaylandPopupAnchorExport();
			}
			return G_SOURCE_REMOVE;
		},
		schedule,
		nullptr);
}

void Instance::ensureWaylandPopupAnchorExport() {
	struct WaylandPopupAnchorRequest {
		::base::weak_ptr<Instance> instance;
		std::uint64_t generation = 0;
	};
	const auto destroyRequest = +[](gpointer userData) {
		delete static_cast<WaylandPopupAnchorRequest*>(userData);
	};
	if (!_window
		|| !_waylandPopupAnchorExportAllowed
		|| _waylandPopupAnchorExportPending
		|| !_waylandPopupAnchorHandle.isEmpty()) {
		return;
	}
	if (gtk_native_get_surface && gdk_wayland_toplevel_export_handle) {
		if (const auto toplevel = GdkWaylandToplevelFromSurface(
				GtkNativeSurface(_window))) {
			const auto generation = ++_waylandPopupAnchorGeneration;
			auto request = std::make_unique<WaylandPopupAnchorRequest>(
				WaylandPopupAnchorRequest{
					.instance = this,
					.generation = generation,
				});
			_waylandPopupAnchorExportPending = true;
			const auto exported = gdk_wayland_toplevel_export_handle(
				toplevel,
				+[](
						GdkToplevel *toplevel,
						const char *handle,
						gpointer userData) {
					const auto request = static_cast<WaylandPopupAnchorRequest*>(
						userData);
					if (const auto instance = request->instance.get()) {
						instance->setWaylandPopupAnchorFromToplevel(
							request->generation,
							toplevel,
							handle ? QString::fromUtf8(handle) : QString());
					}
				},
				request.get(),
				destroyRequest);
			if (exported) {
				request.release();
			} else {
				_waylandPopupAnchorExportPending = false;
			}
			return;
		}
	}
	if (gtk_widget_get_window && gdk_wayland_window_export_handle) {
		if (const auto gdkWindow = gtk_widget_get_window(_window);
			IsGdkWaylandWindow(gdkWindow)) {
			const auto generation = ++_waylandPopupAnchorGeneration;
			auto request = std::make_unique<WaylandPopupAnchorRequest>(
				WaylandPopupAnchorRequest{
					.instance = this,
					.generation = generation,
				});
			_waylandPopupAnchorExportPending = true;
			const auto exported = gdk_wayland_window_export_handle(
				gdkWindow,
				+[](
						GdkWindow *window,
						const char *handle,
						gpointer userData) {
					const auto request = static_cast<WaylandPopupAnchorRequest*>(
						userData);
					if (const auto instance = request->instance.get()) {
						instance->setWaylandPopupAnchorFromWindow(
							request->generation,
							window,
							handle ? QString::fromUtf8(handle) : QString());
					}
				},
				request.get(),
				destroyRequest);
			if (exported) {
				request.release();
			} else {
				_waylandPopupAnchorExportPending = false;
			}
		}
	}
}

void Instance::clearWaylandPopupAnchorExport() {
	const auto hadExport = _waylandPopupAnchorExportPending
		|| !_waylandPopupAnchorHandle.isEmpty();
	_waylandPopupAnchorExportScheduled = false;
	const auto handle = _waylandPopupAnchorHandle;
	_waylandPopupAnchorExportPending = false;
	_waylandPopupAnchorHandle = QString();
	if (!hadExport) {
		return;
	}
	++_waylandPopupAnchorGeneration;
	if (!_window) {
		return;
	}
	if (gtk_native_get_surface) {
		if (const auto toplevel = GdkWaylandToplevelFromSurface(
				GtkNativeSurface(_window))) {
			if (!handle.isEmpty()) {
				if (gdk_wayland_toplevel_drop_exported_handle) {
					const auto data = handle.toUtf8();
					gdk_wayland_toplevel_drop_exported_handle(
						toplevel,
						data.constData());
				} else if (gdk_wayland_toplevel_unexport_handle) {
					gdk_wayland_toplevel_unexport_handle(toplevel);
				}
			}
			return;
		}
	}
	if (gtk_widget_get_window && gdk_wayland_window_unexport_handle) {
		if (const auto gdkWindow = gtk_widget_get_window(_window);
			IsGdkWaylandWindow(gdkWindow)
			&& !handle.isEmpty()) {
			gdk_wayland_window_unexport_handle(gdkWindow);
		}
	}
}

void Instance::setWaylandPopupAnchorFromToplevel(
		std::uint64_t generation,
		GdkToplevel *toplevel,
		QString handle) {
	if (generation != _waylandPopupAnchorGeneration) {
		if (!handle.isEmpty()) {
			if (gdk_wayland_toplevel_drop_exported_handle) {
				const auto data = handle.toUtf8();
				gdk_wayland_toplevel_drop_exported_handle(
					toplevel,
					data.constData());
			} else if (gdk_wayland_toplevel_unexport_handle) {
				gdk_wayland_toplevel_unexport_handle(toplevel);
			}
		}
		return;
	}
	_waylandPopupAnchorExportPending = false;
	_waylandPopupAnchorHandle = std::move(handle);
}

void Instance::setWaylandPopupAnchorFromWindow(
		std::uint64_t generation,
		GdkWindow *window,
		QString handle) {
	if (generation != _waylandPopupAnchorGeneration) {
		if (!handle.isEmpty() && gdk_wayland_window_unexport_handle) {
			gdk_wayland_window_unexport_handle(window);
		}
		return;
	}
	_waylandPopupAnchorExportPending = false;
	_waylandPopupAnchorHandle = std::move(handle);
}

void *Instance::winId() {
	if (_remoting) {
		if (!_helper) {
			return nullptr;
		}

		const ::base::has_weak_ptr guard;
		std::optional<void*> ret;
		_helper.call_get_win_id(crl::guard(&guard, [&](
				GObject::Object source_object,
				Gio::AsyncResult res) {
			const auto reply = _helper.call_get_win_id_finish(res);
			ret = reply
				? reinterpret_cast<void*>(std::get<1>(*reply))
				: nullptr;
			GLib::MainContext::default_().wakeup();
		}));

		while (!ret && _connected) {
			GLib::MainContext::default_().iteration(true);
		}

		return ret.value_or(nullptr);
	}

	if (_mode == WindowMode::External) {
		const auto xid = X11WindowId(_window);
		return xid ? reinterpret_cast<void*>(xid) : nullptr;
	}

	return (_platform == Platform::X11)
		? reinterpret_cast<void*>(gtk_plug_get_id(GTK_PLUG(_window)))
		: nullptr;
}

PopupAnchor Instance::popupAnchorSnapshot() {
	auto result = PopupAnchor();
	if (!_window) {
		return result;
	}
	if (gtk_native_get_surface && gdk_surface_get_width && gdk_surface_get_height) {
		if (const auto surface = GtkNativeSurface(_window)) {
			const auto width = gdk_surface_get_width(surface);
			const auto height = gdk_surface_get_height(surface);
			if (width > 0 && height > 0) {
				result.outerSize = QSize(width, height);
			}
		}
	} else if (gtk_window_get_size) {
		auto width = gint(0);
		auto height = gint(0);
		gtk_window_get_size(GTK_WINDOW(_window), &width, &height);
		if (width > 0 && height > 0) {
			result.outerSize = QSize(width, height);
		}
	}
	if (const auto nativeId = X11WindowId(_window)) {
		clearWaylandPopupAnchorExport();
		result.transientParent = {
			.type = Ui::Platform::ForeignParent::Type::X11,
			.x11 = nativeId,
		};
		return result;
	}
	if (gtk_native_get_surface) {
		if (GdkWaylandToplevelFromSurface(GtkNativeSurface(_window))) {
			ensureWaylandPopupAnchorExport();
			if (!_waylandPopupAnchorHandle.isEmpty()) {
				result.transientParent = {
					.type = Ui::Platform::ForeignParent::Type::Wayland,
					.wayland = _waylandPopupAnchorHandle,
				};
			}
			return result;
		}
	}
	if (gtk_widget_get_window) {
		if (const auto gdkWindow = gtk_widget_get_window(_window);
			IsGdkWaylandWindow(gdkWindow)) {
			ensureWaylandPopupAnchorExport();
			if (!_waylandPopupAnchorHandle.isEmpty()) {
				result.transientParent = {
					.type = Ui::Platform::ForeignParent::Type::Wayland,
					.wayland = _waylandPopupAnchorHandle,
				};
			}
			return result;
		}
	}
	clearWaylandPopupAnchorExport();
	return result;
}

bool Instance::notifyExternalWindowClosed() {
	if (_mode != WindowMode::External) {
		return false;
	} else if (_externalWindowCloseAllowed) {
		return false;
	} else if (_externalWindowClosePending) {
		return true;
	} else if (!_master) {
		return false;
	}
	_externalWindowClosePending = true;
	const auto weak = ::base::make_weak(this);
	_master.call_external_window_closed([=](
			GObject::Object,
			Gio::AsyncResult res) {
		if (const auto instance = weak.get()) {
			instance->_externalWindowClosePending = false;
			if (instance->_master) {
				instance->_master.call_external_window_closed_finish(res);
			}
			const auto window = instance->_window;
			if (!window) {
				return;
			}
			instance->_externalWindowCloseAllowed = true;
			if (gtk_window_destroy) {
				gtk_window_destroy(GTK_WINDOW(window));
			} else {
				gtk_widget_destroy(window);
			}
		}
	});
	return true;
}

PopupAnchor Instance::popupAnchor() {
	if (_remoting) {
		if (!_helper) {
			return {};
		}

		const ::base::has_weak_ptr guard;
		std::optional<PopupAnchor> ret;
		_helper.call_get_window_anchor(crl::guard(&guard, [&](
				GObject::Object source_object,
				Gio::AsyncResult res) {
			auto result = PopupAnchor();
			if (const auto reply = _helper.call_get_window_anchor_finish(res)) {
				if (const auto parent = PopupAnchorParent(
						std::get<1>(*reply),
						std::get<2>(*reply),
						std::get<3>(*reply))) {
					result.transientParent = parent;
				}
				if (const auto geometry = PopupAnchorGeometry(
						std::get<4>(*reply),
						std::get<5>(*reply),
						std::get<6>(*reply),
						std::get<7>(*reply),
						std::get<8>(*reply))) {
					result.geometry = *geometry;
				}
				if (const auto outerSize = PopupAnchorOuterSize(
						std::get<9>(*reply),
						std::get<10>(*reply),
						std::get<11>(*reply))) {
					result.outerSize = *outerSize;
				}
			}
			ret = std::move(result);
			GLib::MainContext::default_().wakeup();
		}));

		while (!ret && _connected) {
			GLib::MainContext::default_().iteration(true);
		}

		return ret.value_or(PopupAnchor());
	}

	return popupAnchorSnapshot();
}

void Instance::refreshNavigationHistoryState() {
	// Not needed here, there are events.
}

auto Instance::navigationHistoryState()
-> rpl::producer<NavigationHistoryState> {
	return _navigationHistoryState.value();
}

void Instance::setOpaqueBg(QColor opaqueBg) {
	if (_remoting) {
#ifdef DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
		if (const auto widget = qobject_cast<QQuickWidget*>(_widget.get())) {
			widget->setClearColor(opaqueBg);
		}
#endif // DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR

		if (!_helper) {
			return;
		}

		_helper.call_set_opaque_bg(
			opaqueBg.red(),
			opaqueBg.green(),
			opaqueBg.blue(),
			opaqueBg.alpha(),
			nullptr);

		return;
	}

	auto background = std::format(R"(
		.webviewWindow,
		window.webviewWindow,
		window.webviewWindow.background {{
			background: {};
			box-shadow: none;
		}}
	)",
		transparentWindowBackground()
			? "transparent"
			: customWindowFrame()
			? kExternalShellFallbackBackground
			: opaqueBg.name().toStdString());

	if (customWindowFrame()) {
		background += R"(
		window.webviewWindow.csd,
		window.webviewWindow.solid-csd,
		window.webviewWindow.ssd,
		window.webviewWindow decoration {
			box-shadow: none;
			margin: 0;
		}
	)";
	}

	if (gtk_css_provider_load_from_string) {
		gtk_css_provider_load_from_string(
			_backgroundProvider,
			background.c_str());
	} else {
		gtk_css_provider_load_from_data(
			_backgroundProvider,
			background.c_str(),
			-1,
			nullptr);
	}
	updateWindowFrameExtents();
}

void Instance::resize(int w, int h) {
	if (_remoting) {
		if (!_helper) {
			return;
		}

		_helper.call_resize(w, h, nullptr);
		return;
	}

	if (_mode == WindowMode::External) {
		gtk_window_set_default_size(GTK_WINDOW(_window), w, h);
		return;
	}
	gtk_widget_set_size_request(_window, w, h);
	GLib::timeout_add_seconds_once(1, crl::guard(this, [=] {
		if (_window) {
			gtk_widget_set_size_request(_window, -1, -1);
		}
	}));
}

void Instance::setFullscreen(bool fullscreen) {
	if (_remoting) {
		if (!_helper) {
			return;
		}

		_helper.call_set_fullscreen(fullscreen, nullptr);
		return;
	}
	if (!_window) {
		return;
	} else if (!gtk_window_fullscreen || !gtk_window_unfullscreen) {
		return;
	} else if (fullscreen) {
		gtk_window_fullscreen(GTK_WINDOW(_window));
	} else {
		gtk_window_unfullscreen(GTK_WINDOW(_window));
	}
	_fullscreen = fullscreen;
	updateWindowFrameExtents();
}

void Instance::startProcess() {
	auto loop = GLib::MainLoop::new_();

	auto serviceLauncher = Gio::SubprocessLauncher::new_(
		Gio::SubprocessFlags::NONE_);

	if (_platform == Platform::Wayland
			&& _glBackend == Ui::GL::Backend::Raster) {
		serviceLauncher.setenv("LIBGL_ALWAYS_SOFTWARE", "1", true);
		serviceLauncher.setenv("GSK_RENDERER", "cairo", true);
		serviceLauncher.setenv("GDK_DISABLE", "gl", true);
		serviceLauncher.setenv("GDK_DEBUG", "gl-disable", true);
		serviceLauncher.setenv("GDK_GL", "disable", true);
		serviceLauncher.setenv("WEBKIT_DISABLE_COMPOSITING_MODE", "1", true);
	}

	int pipefd[2] = { -1, -1 };
	GError *error = nullptr;
	if (!g_unix_open_pipe(pipefd, O_CLOEXEC, &error)
			&& (error || !g_unix_open_pipe(pipefd, FD_CLOEXEC, &error))) {
		LOG(("WebView Error: %1").arg(error->message));
		g_clear_error(&error);
		return;
	}

	serviceLauncher.take_fd(pipefd[0], 3);
	auto pipeGuard = std::make_optional(gsl::finally([&] {
		GLib::close(pipefd[1]);
	}));

	auto serviceProcess = serviceLauncher.spawnv({
		::base::Integration::Instance().executablePath().toStdString(),
		std::string("-webviewhelper"),
		SocketPath,
	});

	if (!serviceProcess) {
		LOG(("WebView Error: %1").arg(
			serviceProcess.error().message_().c_str()));
		return;
	}

	_serviceProcess = *serviceProcess;

	const auto socketPath = std::vformat(
		std::string_view(SocketPath),
		std::make_format_args(
			static_cast<const std::string>(
				_serviceProcess.get_identifier())));

	if (socketPath.empty()) {
		LOG(("WebView Error: IPC socket path is not set."));
		return;
	}

	if (_platform == Platform::Wayland && !_compositor) {
		_compositor = new Compositor(
			QByteArray::fromStdString(
				GLib::path_get_basename(socketPath + "-wayland")));
	}

	auto authObserver = Gio::DBusAuthObserver::new_();
	authObserver.signal_authorize_authenticated_peer().connect([=](
			Gio::DBusAuthObserver,
			Gio::IOStream stream,
			Gio::Credentials credentials) {
		return credentials.get_unix_pid(nullptr)
			== std::stoi(_serviceProcess.get_identifier());
	});

	auto dbusServer = Gio::DBusServer::new_sync(
		SocketPathToDBusAddress(socketPath),
		Gio::DBusServerFlags::NONE_,
		Gio::dbus_generate_guid(),
		authObserver,
		{});

	if (!dbusServer) {
		LOG(("WebView Error: %1.").arg(
			dbusServer.error().message_().c_str()));
		return;
	}

	_dbusServer = *dbusServer;
	_dbusServer.start();
	const ::base::has_weak_ptr guard;
	auto started = ulong();
	const auto newConnection = _dbusServer.signal_new_connection().connect(
		[&](
			Gio::DBusServer,
			Gio::DBusConnection connection) {
		_master = MasterSkeleton::new_();
		auto object = ObjectSkeleton::new_(kMasterObjectPath);
		object.set_master(_master);
		_dbusObjectManager = Gio::DBusObjectManagerServer::new_(kObjectPath);
		_dbusObjectManager.export_(object);
		_dbusObjectManager.set_connection(connection);
		registerMasterMethodHandlers();

		HelperProxy::new_(
			connection,
			Gio::DBusProxyFlags::NONE_,
			kHelperObjectPath,
			crl::guard(&guard, [&](
					GObject::Object source_object,
					Gio::AsyncResult res) {
				auto helper = HelperProxy::new_finish(res);
				if (!helper) {
					LOG(("WebView Error: %1").arg(
						helper.error().message_().c_str()));
					loop.quit();
					return;
				}

				_helper = *helper;

				started = _helper.signal_started().connect([&](Helper) {
					_connected = true;
					loop.quit();
				});
			}));

		connection.signal_closed().connect(crl::guard(this, [=](
				Gio::DBusConnection,
				bool remotePeerVanished,
				GLib::Error_Ref error) {
			_connected = false;
			_widget = nullptr;
			GLib::MainContext::default_().wakeup();
		}));

		return true;
	});

	// timeout in case something goes wrong
	bool timeoutHappened = false;
	const auto timeout = GLib::timeout_add_seconds_once(5, [&] {
		timeoutHappened = true;
		loop.quit();
	});

	pipeGuard.reset();
	loop.run();
	if (timeoutHappened) {
		LOG(("WebView Error: Timed out waiting for WebView helper process."));
	} else {
		GLib::Source::remove(timeout);
	}
	if (_helper && started) {
		_helper.disconnect(started);
	}
	_dbusServer.disconnect(newConnection);
}

void Instance::stopProcess() {
	if (_serviceProcess) {
		_serviceProcess.send_signal(SIGTERM);
	}
	GLib::timeout_add_seconds_once(1, [compositor = _compositor] {
		if (compositor) {
			compositor->deleteLater();
		}
	});
	_compositor = nullptr;
}

void Instance::updateHistoryStates() {
	const auto url = webkit_web_view_get_uri(_webview);
	const auto title = webkit_web_view_get_title(_webview);
	if (((_platform == Platform::Any) || (_mode == WindowMode::External))
		&& _window) {
		gtk_window_set_title(GTK_WINDOW(_window), title ? title : "");
	}
	_master.call_navigation_state_update(
		url ? url : "",
		title ? title : "",
		webkit_web_view_can_go_back(_webview),
		webkit_web_view_can_go_forward(_webview),
		nullptr);
}

void Instance::registerMasterMethodHandlers() {
	if (!_master) {
		return;
	}

	_master.signal_handle_get_start_data().connect([=](
			Master,
			Gio::DBusMethodInvocation invocation) {
		_master.complete_get_start_data(
			invocation,
			int(_platform),
			_compositor ? _compositor->socketName().toStdString() : "",
			[] {
				if (auto app = Gio::Application::get_default()) {
					if (const auto appId = app.get_application_id()) {
						return std::string(appId);
					}
				}

				const auto qtAppId = QGuiApplication::desktopFileName()
					.toStdString();

				if (Gio::Application::id_is_valid(qtAppId)) {
					return qtAppId;
				}

				return std::string();
			}());
		return true;
	});

	_master.signal_handle_message_received().connect([=](
			Master,
			Gio::DBusMethodInvocation invocation,
			const std::string &message) {
		if (_messageHandler) {
			_messageHandler(Message{ .text = message });
			_master.complete_message_received(invocation);
		} else {
			invocation.return_gerror(MethodError());
		}
		return true;
	});

	_master.signal_handle_navigation_started().connect([=](
			Master,
			Gio::DBusMethodInvocation invocation,
			const std::string &uri,
			bool newWindow) {
		if (newWindow) {
			if (_navigationStartHandler
					&& _navigationStartHandler(uri, true)) {
				QDesktopServices::openUrl(QString::fromStdString(uri));
			}
			_master.complete_navigation_started(invocation, false);
		} else if (!uri.starts_with(dataDomain())
				&& _navigationStartHandler
				&& !_navigationStartHandler(uri, false)) {
			_master.complete_navigation_started(invocation, false);
		} else {
			_master.complete_navigation_started(invocation, true);
		}
		return true;
	});

	_master.signal_handle_navigation_done().connect([=](
			Master,
			Gio::DBusMethodInvocation invocation,
			bool success) {
		if (_navigationDoneHandler) {
			_navigationDoneHandler(success);
			_master.complete_navigation_done(invocation);
		} else {
			invocation.return_gerror(MethodError());
		}
		return true;
	});

	_master.signal_handle_external_window_closed().connect([=](
			Master,
			Gio::DBusMethodInvocation invocation) {
		if (_externalWindowCloseHandler) {
			_externalWindowCloseHandler();
		}
		_master.complete_external_window_closed(invocation);
		return true;
	});

	_master.signal_handle_script_dialog().connect([=](
			Master,
			Gio::DBusMethodInvocation invocation,
			int type,
			const std::string &text,
			const std::string &value) {
		if (!_dialogHandler) {
			invocation.return_gerror(MethodError());
			return true;
		}

		const auto dialogType = (type == WEBKIT_SCRIPT_DIALOG_PROMPT)
			? DialogType::Prompt
			: (type == WEBKIT_SCRIPT_DIALOG_ALERT)
			? DialogType::Alert
			: DialogType::Confirm;

		auto args = DialogArgs{
			.type = dialogType,
			.value = value,
			.text = text,
		};

		if (_asyncDialogHandler) {
			const auto weak = ::base::make_weak(this);
			const auto handled = _asyncDialogHandler(args, [=](
					DialogResult result) mutable {
				if (!weak || !_master) {
					return;
				}
				_master.complete_script_dialog(
					invocation,
					result.accepted,
					result.text);
			});
			if (handled) {
				return true;
			}
		}

		const auto result = _dialogHandler(std::move(args));

		_master.complete_script_dialog(
			invocation,
			result.accepted,
			result.text);

		return true;
	});

	_master.signal_handle_navigation_state_update().connect([=](
			Master,
			Gio::DBusMethodInvocation invocation,
			const std::string &url,
			const std::string &title,
			bool canGoBack,
			bool canGoForward) {
		_navigationHistoryState = NavigationHistoryState{
			.url = url,
			.title = title,
			.canGoBack = canGoBack,
			.canGoForward = canGoForward,
		};
		_master.complete_navigation_state_update(invocation);
		return true;
	});

	_master.signal_handle_user_interaction().connect([=](
			Master,
			Gio::DBusMethodInvocation invocation) {
		if (_interactionHandler) {
			_interactionHandler();
		}
		_master.complete_user_interaction(invocation);
		return true;
	});
}

int Instance::exec() {
	auto app = Gio::Application::new_(
		Gio::ApplicationFlags::NON_UNIQUE_);

	app.signal_startup().connect([=](Gio::Application) {
		_helper.emit_started();
	});

	app.signal_activate().connect([](Gio::Application) {});

	app.hold();

	auto loop = GLib::MainLoop::new_();

	std::uint8_t dummy{};
#if __has_include(<giounix/giounix.hpp>)
	GioUnix::InputStream::new_(3, true).read_all(&dummy, 1);
#else // __has_include(<giounix/giounix.hpp>)
	Gio::UnixInputStream::new_(3, true).read_all(&dummy, 1);
#endif // !__has_include(<giounix/giounix.hpp>)

	auto connection = Gio::DBusConnection::new_for_address_sync(
		SocketPathToDBusAddress(
			std::vformat(
				std::string_view(SocketPath),
				std::make_format_args(
					static_cast<const std::string>(
						std::to_string(getpid()))))),
		Gio::DBusConnectionFlags::AUTHENTICATION_CLIENT_);

	if (!connection) {
		g_critical("%s", connection.error().message_().c_str());
		return 1;
	}

	_helper = HelperSkeleton::new_();
	auto object = ObjectSkeleton::new_(kHelperObjectPath);
	object.set_helper(_helper);
	_dbusObjectManager = Gio::DBusObjectManagerServer::new_(kObjectPath);
	_dbusObjectManager.export_(object);
	_dbusObjectManager.set_connection(*connection);
	registerHelperMethodHandlers();

	bool error = false;
	MasterProxy::new_(
		*connection,
		Gio::DBusProxyFlags::NONE_,
		kMasterObjectPath,
		[&](GObject::Object source_object, Gio::AsyncResult res) {
			auto master = MasterProxy::new_finish(res);
			if (!master) {
				error = true;
				g_critical("%s", master.error().message_().c_str());
				loop.quit();
				return;
			}
			_master = *master;
			_master.call_get_start_data([&](
					GObject::Object source_object,
					Gio::AsyncResult res) {
				const auto settings = _master.call_get_start_data_finish(
					res);
				if (!settings) {
					error = true;
					g_critical("%s", settings.error().message_().c_str());
					loop.quit();
					return;
				}
				_platform = Platform(std::get<1>(*settings));
				if (const auto waylandDisplay = std::get<2>(*settings)
						; !waylandDisplay.empty()) {
					GLib::setenv("WAYLAND_DISPLAY", waylandDisplay, true);
				}
				if (const auto appId = std::get<3>(*settings)
						; !appId.empty()) {
					app.set_application_id(appId);
				}
				loop.quit();
			});
		});

	connection->signal_closed().connect([&](
			Gio::DBusConnection,
			bool remotePeerVanished,
			GLib::Error_Ref error) {
		app.quit();
	});

	loop.run();

	if (error) {
		return 1;
	}

	_master.signal_data_server_started().connect([=](
			Master,
			std::uint16_t port,
			const std::string &password) {
		_dataPort = port;
		_dataPassword = password;
	});

	return app.run({});
}

void Instance::registerHelperMethodHandlers() {
	if (!_helper) {
		return;
	}

	_helper.signal_handle_create().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation,
			bool debug,
			int r,
			int g,
			int b,
			int a,
			const std::string &path,
			int mode,
			int windowStyle,
			const std::string &shellMessageToken,
			int marginLeft,
			int marginRight,
			int marginTop,
			int marginBottom,
			int initialWidth,
			int initialHeight) {
		const auto windowMode = (mode == int(WindowMode::External))
			? WindowMode::External
			: WindowMode::Embedded;
		const auto frameStyle = (windowStyle == int(WindowStyle::Frameless))
			? WindowStyle::Frameless
			: WindowStyle::Default;
		_mode = windowMode;
		if (create({
			.opaqueBg = QColor(r, g, b, a),
			.userDataPath = path,
			.debug = debug,
			.mode = windowMode,
			.windowStyle = frameStyle,
			.windowMargins = QMargins(
				marginLeft,
				marginTop,
				marginRight,
				marginBottom),
			.initialSize = QSize(initialWidth, initialHeight),
			.shellMessageToken = shellMessageToken,
		})) {
			_helper.complete_create(invocation);
		} else {
			invocation.return_gerror(MethodError());
		}
		return true;
	});

	_helper.signal_handle_reload().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation) {
		reload();
		_helper.complete_reload(invocation);
		return true;
	});

	_helper.signal_handle_resolve().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation,
			int mode) {
		_mode = (mode == int(WindowMode::External))
			? WindowMode::External
			: WindowMode::Embedded;
		_helper.complete_resolve(invocation, int(resolve()));
		return true;
	});

	_helper.signal_handle_navigate().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation,
			const std::string &url) {
		navigate(url);
		_helper.complete_navigate(invocation);
		return true;
	});

	_helper.signal_handle_resize().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation,
			int w,
			int h) {
		resize(w, h);
		_helper.complete_resize(invocation);
		return true;
	});

	_helper.signal_handle_set_fullscreen().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation,
			bool fullscreen) {
		setFullscreen(fullscreen);
		_helper.complete_set_fullscreen(invocation);
		return true;
	});

	_helper.signal_handle_init().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation,
			const std::string &js) {
		init(js);
		_helper.complete_init(invocation);
		return true;
	});

	_helper.signal_handle_eval().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation,
			const std::string &js) {
		eval(js);
		_helper.complete_eval(invocation);
		return true;
	});

	_helper.signal_handle_set_opaque_bg().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation,
			int r,
			int g,
			int b,
			int a) {
		setOpaqueBg(QColor(r, g, b, a));
		_helper.complete_set_opaque_bg(invocation);
		return true;
	});

	_helper.signal_handle_get_win_id().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation) {
		_helper.complete_get_win_id(
			invocation,
			reinterpret_cast<uint64>(winId()));
		return true;
	});

	_helper.signal_handle_get_window_anchor().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation) {
		const auto anchor = popupAnchorSnapshot();
		const auto geometry = anchor.geometry.value_or(QRect());
		const auto outerSize = anchor.outerSize.value_or(QSize());
		_helper.complete_get_window_anchor(
			invocation,
			int(anchor.transientParent.type),
			uint64(anchor.transientParent.x11),
			anchor.transientParent.wayland.toStdString(),
			anchor.geometry.has_value(),
			geometry.x(),
			geometry.y(),
			geometry.width(),
			geometry.height(),
			anchor.outerSize.has_value(),
			outerSize.width(),
			outerSize.height());
		return true;
	});
}

} // namespace

Available Availability() {
	Instance instance;
	const auto resolved = instance.resolve();
	if (resolved == ResolveResult::NoLibrary) {
		return Available{
			.error = Available::Error::NoWebKitGTK,
			.details = "Please install WebKitGTK "
			"(webkit2gtk-4.1/webkit2gtk-4.0) "
			"from your package manager.",
		};
	}
	const auto success = (resolved == ResolveResult::Success)
		&& instance.startDataServer();
	return Available{
		.customSchemeRequests = success,
		.customRangeRequests = success,
		.customReferer = success,
	};
}

std::unique_ptr<Interface> CreateInstance(Config config) {
	auto result = std::make_unique<Instance>(true, config.mode);
	if (!result->create(std::move(config))) {
		return nullptr;
	}
	return result;
}

int Exec() {
	return Instance(false).exec();
}

void SetSocketPath(const std::string &socketPath) {
	SocketPath = socketPath;
}

} // namespace Webview::WebKitGTK
