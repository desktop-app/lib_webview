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
#include "base/platform/linux/base_linux_xcb_utilities.h"
#include "base/platform/linux/base_linux_xdg_activation_token.h"
#include "base/algorithm.h"
#include "base/debug_log.h"
#include "base/integration.h"
#include "base/invoke_queued.h"
#include "base/random.h"
#include "base/unique_qptr.h"
#include "base/weak_ptr.h"
#include "base/event_filter.h"
#include "ui/gl/gl_detection.h"
#include "ui/style/style_core_scale.h"

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

using namespace gi::repository::Webview;
using namespace Library;
namespace Gio = gi::repository::Gio;
#if __has_include(<giounix/giounix.hpp>)
namespace GioUnix = gi::repository::GioUnix;
#endif // __has_include(<giounix/giounix.hpp>)
namespace GLib = gi::repository::GLib;
namespace GObjectCpp = gi::repository::GObject;

constexpr auto kObjectPath = "/org/desktop_app/GtkIntegration/Webview";
constexpr auto kMasterObjectPath
	= "/org/desktop_app/GtkIntegration/Webview/Master";
constexpr auto kHelperObjectPath
	= "/org/desktop_app/GtkIntegration/Webview/Helper";
constexpr auto kDataHost = "127.0.0.1";
constexpr auto kExternalShellFallbackBackground = "#eeeeee";
constexpr auto kMaxScriptMessageBytes = 2 * 1024 * 1024;
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

[[nodiscard]] std::string GenerateSecret() {
	auto bytes = QByteArray();
	bytes.resize(32);
	::base::RandomFill(bytes.data(), bytes.size());
	return bytes.toHex().toStdString();
}

[[nodiscard]] bool SetCookiePolicy(
		WebKitCookieManager *manager,
		WebKitCookieAcceptPolicy policy) {
	if (!manager) {
		return false;
	}
	webkit_cookie_manager_set_accept_policy(manager, policy);
	return true;
}

[[nodiscard]] bool AllowThirdPartyCookies(WebKitCookieManager *manager) {
	return SetCookiePolicy(manager, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);
}

[[nodiscard]] bool BlockDownloads(::GObject *owner) {
	if (!owner
			|| !g_signal_lookup("download-started", G_OBJECT_TYPE(owner))) {
		return false;
	}
	g_signal_connect(
		owner,
		"download-started",
		G_CALLBACK(+[](
				::GObject*,
				WebKitDownload *download,
				gpointer) {
			webkit_download_cancel(download);
		}),
		nullptr);
	return true;
}

[[nodiscard]] bool BlockNativePrompts(WebKitWebView *webview) {
	const auto type = G_OBJECT_TYPE(webview);
	if (!g_signal_lookup("run-file-chooser", type)) {
		return false;
	}
	for (const auto signal : {
		"run-file-chooser",
		"run-color-chooser",
		"print",
	}) {
		if (!g_signal_lookup(signal, type)) {
			continue;
		}
		g_signal_connect(
			webview,
			signal,
			G_CALLBACK(+[](
					WebKitWebView*,
					gpointer,
					gpointer) -> gboolean {
				return true;
			}),
			nullptr);
	}
	return true;
}

[[nodiscard]] bool ApplyRestrictedSettings(WebKitSettings *settings) {
	if (!settings) {
		return false;
	}
	webkit_settings_set_auto_load_images(settings, false);
	webkit_settings_set_enable_dns_prefetching(settings, false);
	webkit_settings_set_enable_fullscreen(settings, false);
	webkit_settings_set_enable_html5_database(settings, false);
	webkit_settings_set_enable_html5_local_storage(settings, false);
	webkit_settings_set_enable_hyperlink_auditing(settings, false);
	webkit_settings_set_enable_media(settings, false);
	webkit_settings_set_enable_offline_web_application_cache(settings, false);
	webkit_settings_set_enable_page_cache(settings, false);
	webkit_settings_set_enable_webaudio(settings, false);
	webkit_settings_set_enable_webgl(settings, false);
	webkit_settings_set_javascript_can_access_clipboard(settings, false);
	webkit_settings_set_javascript_can_open_windows_automatically(
		settings,
		false);
	webkit_settings_set_media_playback_requires_user_gesture(settings, true);
	if (webkit_settings_set_enable_webrtc) {
		webkit_settings_set_enable_webrtc(settings, false);
	}
	webkit_settings_set_enable_media_stream(settings, false);
	return true;
}

inline std::string SocketPathToDBusAddress(const std::string &socketPath) {
	return "unix:path=" + Gio::dbus_address_escape_value(socketPath);
}

enum class ShellControlAction {
	None,
	SetDragRegions,
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
	return (command == "shell_set_drag_regions")
		? ShellControlAction::SetDragRegions
		: ShellControlAction::None;
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

[[nodiscard]] std::optional<std::vector<QRectF>> ShellRegions(
		const QJsonObject &arguments,
		const char *key) {
	const auto value = arguments.value(QString::fromLatin1(key));
	if (!value.isArray()) {
		return std::nullopt;
	}
	const auto isNumber = [](const QJsonValue &number) {
		return number.isDouble();
	};
	auto result = std::vector<QRectF>();
	for (const auto &entry : value.toArray()) {
		const auto values = entry.toArray();
		if (values.size() != 4
				|| !std::all_of(values.begin(), values.end(), isNumber)) {
			return std::nullopt;
		}
		result.emplace_back(
			values[0].toDouble(),
			values[1].toDouble(),
			values[2].toDouble(),
			values[3].toDouble());
	}
	return result;
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

[[nodiscard]] std::optional<QSize> PopupAnchorOuterSize(
		bool hasOuterSize,
		int width,
		int height) {
	return (hasOuterSize && ValidPopupAnchorSize(width, height))
		? std::make_optional(QSize(width, height))
		: std::nullopt;
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
		&& GTK_IS_NATIVE(window)
		? gtk_native_get_surface(GTK_NATIVE(window))
		: nullptr;
}

[[nodiscard]] GdkToplevel *GdkToplevelFromSurface(GdkSurface *surface) {
	return surface && GDK_IS_TOPLEVEL(surface)
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
	} else if (gtk_native_get_surface) {
		const auto surface = GtkNativeSurface(window);
		return IsGdkX11Surface(surface)
			? gdk_x11_surface_get_xid(surface)
			: 0;
	}
	const auto gdkWindow = gtk_widget_get_window(window);
	return IsGdkX11Window(gdkWindow)
		? gdk_x11_window_get_xid(gdkWindow)
		: 0;
}

// XMapEvent from Xlib.h, which we avoid to include.
struct X11MapEvent {
	int type = 0;
	unsigned long serial = 0;
	int sendEvent = 0;
	void *display = nullptr;
	unsigned long event = 0;
	unsigned long window = 0;
};

constexpr auto kX11MapNotify = 19;

// XSetWindowAttributes from Xlib.h, which we avoid to include.
struct X11WindowAttributes {
	unsigned long backgroundPixmap = 0;
	unsigned long backgroundPixel = 0;
	unsigned long borderPixmap = 0;
	unsigned long borderPixel = 0;
	int bitGravity = 0;
	int winGravity = 0;
	int backingStore = 0;
	unsigned long backingPlanes = 0;
	unsigned long backingPixel = 0;
	int saveUnder = 0;
	long eventMask = 0;
	long doNotPropagateMask = 0;
	int overrideRedirect = 0;
	unsigned long colormap = 0;
	unsigned long cursor = 0;
};

constexpr auto kX11CWOverrideRedirect = 1UL << 9;
constexpr auto kX11RevertToParent = 2;

void FocusX11Window(WId window) {
	using namespace ::base::Platform::XCB::Library;
	static const auto xcb_set_input_focus_checked = LoadSymbol<
		xcb_void_cookie_t(
			xcb_connection_t*,
			uint8_t,
			xcb_window_t,
			xcb_timestamp_t)>("xcb_set_input_focus_checked");
	const ::base::Platform::XCB::Connection connection;
	if (!connection || xcb_connection_has_error(connection)) {
		return;
	}
	free(
		xcb_request_check(
			connection,
			xcb_set_input_focus_checked(
				connection,
				XCB_INPUT_FOCUS_PARENT,
				window,
				XCB_CURRENT_TIME)));
}

[[nodiscard]] bool SetupWindowAlpha(GtkWidget *window) {
	if (!window) {
		return false;
	}
	if (gdk_display_is_composited) {
		if (const auto display = gtk_widget_get_display(window)) {
			return gdk_display_is_composited(display);
		}
		return true;
	}
	const auto screen = gtk_widget_get_screen(window);
	if (!screen) {
		return false;
	}
	const auto visual = gdk_screen_is_composited(screen)
		? gdk_screen_get_rgba_visual(screen)
		: nullptr;
	if (!visual) {
		return false;
	}
	gtk_widget_set_visual(window, visual);
	return true;
}

// WebKit zooms the page by the font DPI on its own, so a CSS pixel is
// larger than a GTK logical one, see refreshInternalScaling() in WebKit.
[[nodiscard]] double PageScale(GtkWidget *window) {
	auto dpi = 0.;
	if (!gdk_screen_get_resolution) {
		auto value = gint();
		g_object_get(
			gtk_settings_get_default(),
			"gtk-xft-dpi",
			&value,
			nullptr);
		dpi = value / 1024.;
	} else if (const auto screen = gtk_widget_get_screen(window)) {
		dpi = gdk_screen_get_resolution(screen);
	}
	return (dpi > 0.) ? (dpi / 96.) : 1.;
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

	void navigate(std::string url) override;
	void navigateToData(std::string id) override;
	void loadHtml(std::string html, std::string baseUrl) override;
	void reload() override;

	void init(std::string js) override;
	void initAllFrames(std::string js) override;
	void eval(std::string js) override;

	void focus() override;
	void setInteractionHandler(Fn<void()> handler) override;
	void setFullscreen(bool fullscreen) override;
	void setInputBlocked(bool blocked) override;
	void setVisible(bool visible) override;

	QWidget *widget() override;
	PopupAnchor popupAnchor() override;

	void refreshNavigationHistoryState() override;
	auto navigationHistoryState()
		-> rpl::producer<NavigationHistoryState> override;

	void setOpaqueBg(QColor opaqueBg) override;

	int exec();

private:
	void scriptMessageReceived(void *message);
	void addUserScript(
		std::string js,
		WebKitUserContentInjectedFrames frames);
	bool handleShellControlMessage(const std::string &message);
	void setShellDragRegions(const QJsonObject &arguments);
	[[nodiscard]] std::optional<GdkSurfaceEdge> shellResizeEdge(
		QPointF point,
		QSizeF size) const;
	[[nodiscard]] bool shellMoveArea(QPointF point) const;
	void pressed(GtkGesture *gesture, double x, double y);
	[[nodiscard]] bool pressed(GdkEvent *event);
	void takeX11InputFocus(guint32 time);
	[[nodiscard]] bool notifyExternalWindowClosed();
	void fullscreenChanged(bool fullscreen);
	[[nodiscard]] bool customWindowFrame() const;
	[[nodiscard]] bool transparentWindowBackground() const;
	void announceCustomWindowFrame();
	[[nodiscard]] QMargins windowFrameExtents() const;
	void setupToplevelFrameExtents();
	void updateWindowFrameExtents();
	void showWindow();
	void setupX11Embedding();

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
	bool authenticate(WebKitAuthenticationRequest *request);
	bool permissionRequest(WebKitPermissionRequest *request);

	std::string dataDomain();
	void dataRequest(
		DataResponse resolved,
		QTcpSocket *socket,
		const std::string &resourceId,
		std::int64_t requestedOffset,
		std::int64_t requestedLimit,
		bool headersWritten,
		const std::shared_ptr<HttpServer::Guard> &guard);

	void resize(int w, int h);
	void startProcess();
	void stopProcess();
	void updateHistoryStates();

	void registerMasterMethodHandlers();
	void registerHelperMethodHandlers();
	void registerHelperSignalHandlers();
	void exportWaylandPopupAnchor();
	[[nodiscard]] void *winId();
	[[nodiscard]] PopupAnchor popupAnchorSnapshot();

	bool _remoting = false;
	WindowMode _mode = WindowMode::Embedded;
	WindowStyle _windowStyle = WindowStyle::Default;
	Master _master;
	Helper _helper;
	GLib::MainLoop _mainLoop;
	Gio::DBusServer _dbusServer;
	Gio::DBusObjectManagerServer _dbusObjectManager;
	Gio::Subprocess _serviceProcess;

	Platform _platform = Platform::Any;
	Ui::GL::Backend _glBackend;
	::base::unique_qptr<QWidget> _widget;
	::base::unique_qptr<QObject> _x11FocusReturnFilter;
	::base::unique_qptr<Compositor> _compositor;
	std::optional<HttpServer> _dataServer;

	GtkApplication *_application = nullptr;
	GtkWidget *_window = nullptr;
	WebKitWebView *_webview = nullptr;
	GtkCssProvider *_backgroundProvider = nullptr;
	QString _waylandPopupAnchorHandle;
	QMargins _windowMargins;
	std::vector<QRectF> _shellDragRegions;
	std::vector<QRectF> _shellNoDragRegions;
	bool _windowSupportsAlpha = true;
	bool _fullscreen = false;
	bool _inputBlocked = false;
	gulong _xftDpiChangedHandler = 0;
	gulong _x11EventHandler = 0;
	std::string _applicationId;
	std::string _xdgActivationToken;

	bool _debug = false;
	std::function<void(Message)> _messageHandler;
	std::function<bool(std::string,bool)> _navigationPolicyHandler;
	std::function<void()> _navigationStartHandler;
	std::function<void(bool)> _navigationDoneHandler;
	std::function<void()> _externalWindowCloseHandler;
	std::function<void(bool)> _fullscreenChangedHandler;
	std::function<DialogResult(DialogArgs)> _dialogHandler;
	AsyncDialogHandler _asyncDialogHandler;
	PermissionHandler _permissionHandler;
	rpl::variable<NavigationHistoryState> _navigationHistoryState;
	std::function<DataResult(DataRequest)> _dataRequestHandler;
	Fn<void()> _interactionHandler;
	std::string _dataRequestRedirectHost;
	std::string _restrictedOrigin;
	std::string _restrictedContentSecurityPolicy;
	std::uint16_t _dataPort = 0;
	std::string _dataPassword;
	std::string _shellMessageToken;
	std::string _messageToken = GenerateSecret();
	bool _loadFailed = false;
	bool _externalWindowCloseAllowed = false;
	bool _externalWindowClosePending = false;

};

Instance::Instance(bool remoting, WindowMode mode)
: _remoting(remoting)
, _mode(mode) {
	if (!_remoting) {
		return;
	}
	_platform = ::Platform::IsX11()
		? Platform::X11
#ifdef DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
		: (_mode == WindowMode::Embedded
			? Platform::Wayland
			: Platform::Any);
#else // DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
		: Platform::Any;
#endif // !DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
	_glBackend = Ui::GL::ChooseBackendDefault(Ui::GL::CheckCapabilities());
	startProcess();
}

Instance::~Instance() {
	if (_remoting) {
		stopProcess();
	}
	if (_backgroundProvider) {
		g_object_unref(_backgroundProvider);
	}
	if (_xftDpiChangedHandler) {
		g_signal_handler_disconnect(
			gtk_settings_get_default(),
			_xftDpiChangedHandler);
	}
	if (_x11EventHandler) {
		g_signal_handler_disconnect(
			gtk_widget_get_display(_window),
			_x11EventHandler);
	}
	if (_window) {
		if (gtk_window_destroy) {
			gtk_window_destroy(GTK_WINDOW(_window));
		} else {
			gtk_widget_destroy(_window);
		}
	}
	if (_application) {
		g_object_unref(_application);
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

	_restrictedOrigin = std::move(config.restrictedOrigin);
	_restrictedContentSecurityPolicy = std::move(
		config.restrictedContentSecurityPolicy);
	if (!_restrictedOrigin.empty()
			&& _restrictedContentSecurityPolicy.empty()) {
		return false;
	}
	_debug = config.debug && _restrictedOrigin.empty();
	_messageHandler = std::move(config.messageHandler);
	_navigationPolicyHandler = std::move(config.navigationPolicyHandler);
	_navigationStartHandler = std::move(config.navigationStartHandler);
	_navigationDoneHandler = std::move(config.navigationDoneHandler);
	_externalWindowCloseHandler = std::move(config.externalWindowCloseHandler);
	_fullscreenChangedHandler = std::move(config.fullscreenChangedHandler);
	_dialogHandler = std::move(config.dialogHandler);
	_asyncDialogHandler = std::move(config.asyncDialogHandler);
	_permissionHandler = std::move(config.permissionHandler);
	_dataRequestHandler = std::move(config.dataRequestHandler);
	_dataRequestRedirectHost = std::move(config.dataRequestRedirectHost);
	_windowStyle = config.windowStyle;
	_windowMargins = config.windowMargins;
	_shellMessageToken = std::move(config.shellMessageToken);

	if (_remoting) {
		if (!_helper) {
			return false;
		}

		auto loop = GLib::MainLoop::new_();
		auto success = false;
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
		const auto allowThirdPartyCookies = config.allowThirdPartyCookies;
		const auto restrictedOrigin = _restrictedOrigin;
		const auto restrictedContentSecurityPolicy
			= _restrictedContentSecurityPolicy;
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
			allowThirdPartyCookies,
			restrictedOrigin,
			restrictedContentSecurityPolicy,
			[&](GObjectCpp::Object source_object, Gio::AsyncResult res) {
				success = _helper.call_create_finish(res, nullptr);
				loop.quit();
			});


		loop.run();
		if (!success) {
			return false;
		}

		if (_mode == WindowMode::External) {
			_widget = ::base::make_unique_q<QWidget>(config.parent);
			return true;
		} else if (_mode == WindowMode::Hidden) {
			return true;
		}

		switch (_platform) {
		case Platform::Any:
			_widget = ::base::make_unique_q<QWidget>(config.parent);
			::base::install_event_filter(_widget, [=](
					not_null<QEvent*> e) {
				if (e->type() == QEvent::Resize) {
					const auto size = static_cast<QResizeEvent*>(
						e.get()
					)->size() * 100. / style::Scale();
					resize(size.width(), size.height());
				}
				return ::base::EventFilterResult::Continue;
			});
			_widget->show();
			break;
		case Platform::X11:
			_widget.reset(
				QWidget::createWindowContainer(
					QWindow::fromWinId(WId(winId())),
					config.parent,
					Qt::FramelessWindowHint));
			::base::install_event_filter(_widget, [=](
					not_null<QEvent*> e) {
				const auto window = (e->type() == QEvent::Show)
					? _widget->window()->windowHandle()
					: nullptr;
				if (!window) {
					return ::base::EventFilterResult::Continue;
				}
				// KWin ignores activation of the active window, so take the focus back.
				_x11FocusReturnFilter.reset(::base::install_event_filter(
					window,
					[=](not_null<QEvent*> event) {
						if (event->type() == QEvent::MouseButtonPress
								&& !QGuiApplication::focusWindow()) {
							FocusX11Window(window->winId());
						}
						return ::base::EventFilterResult::Continue;
					}).get());
				return ::base::EventFilterResult::Continue;
			});
			_widget->show();
			break;
		}

		return true;
	}

	_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	if (_platform == Platform::Wayland && _mode == WindowMode::Embedded) {
		// GTK takes the scale on creation: bind the new output, get its scale.
		gdk_display_sync(gtk_widget_get_display(_window));
		gdk_display_sync(gtk_widget_get_display(_window));
	}
	if (_mode == WindowMode::External) {
		if (!_applicationId.empty()) {
			// GTK gives windows an application id only from GtkApplication.
			_application = gtk_application_new(
				_applicationId.c_str(),
				G_APPLICATION_NON_UNIQUE);
			g_application_register(
				G_APPLICATION(_application),
				nullptr,
				nullptr);
			gtk_window_set_application(GTK_WINDOW(_window), _application);
		}
		if (customWindowFrame()) {
			gtk_window_set_decorated(GTK_WINDOW(_window), FALSE);
		}
		if (config.initialSize.width() > 0 && config.initialSize.height() > 0) {
			const auto size = config.initialSize * PageScale(_window);
			gtk_window_set_default_size(
				GTK_WINDOW(_window),
				size.width(),
				size.height());
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
		if (gtk_window_is_fullscreen) {
			g_signal_connect_swapped(
				_window,
				"notify::fullscreened",
				G_CALLBACK(+[](Instance *instance) {
					instance->fullscreenChanged(gtk_window_is_fullscreen(
						GTK_WINDOW(instance->_window)));
				}),
				this);
		} else {
			g_signal_connect_swapped(
				_window,
				"window-state-event",
				G_CALLBACK(+[](Instance *instance, GdkEvent*) -> gboolean {
					const auto window = gtk_widget_get_window(
						instance->_window);
					instance->fullscreenChanged(window
						&& (gdk_window_get_state(window)
							& GDK_WINDOW_STATE_FULLSCREEN));
					return FALSE;
				}),
				this);
		}
	}
	_windowSupportsAlpha = customWindowFrame() ? SetupWindowAlpha(_window) : false;
	if (gtk_widget_add_css_class) {
		gtk_widget_add_css_class(_window, "webviewWindow");
	} else {
		gtk_style_context_add_class(
			gtk_widget_get_style_context(_window),
			"webviewWindow");
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

	const auto restricted = !_restrictedOrigin.empty();
	if (restricted
			&& !webkit_web_view_get_default_content_security_policy) {
		return false;
	}
	if (webkit_network_session_new) {
		const auto session = restricted
			? webkit_network_session_new_ephemeral()
			: webkit_network_session_new(
				baseData.c_str(),
				baseCache.c_str());
		if (!session) {
			return false;
		}
		if (restricted || config.allowThirdPartyCookies) {
			const auto manager = webkit_network_session_get_cookie_manager(
				session);
			const auto policySet = restricted
				? SetCookiePolicy(
					manager,
					WEBKIT_COOKIE_POLICY_ACCEPT_NEVER)
				: AllowThirdPartyCookies(manager);
			if (!policySet) {
				g_critical("Cookie policy API is unavailable.");
				g_object_unref(session);
				return false;
			}
		}
		if (!BlockDownloads(G_OBJECT(session)) && restricted) {
			g_object_unref(session);
			return false;
		}
		_webview = restricted
			? WEBKIT_WEB_VIEW(g_object_new(
				WEBKIT_TYPE_WEB_VIEW,
				"network-session",
				session,
				"default-content-security-policy",
				_restrictedContentSecurityPolicy.c_str(),
				nullptr))
			: WEBKIT_WEB_VIEW(g_object_new(
				WEBKIT_TYPE_WEB_VIEW,
				"network-session",
				session,
				nullptr));
		g_object_unref(session);
	} else {
		const auto data = restricted
			? webkit_website_data_manager_new_ephemeral()
			: webkit_website_data_manager_new(
				"base-cache-directory", baseCache.c_str(),
				"base-data-directory", baseData.c_str(),
				nullptr);
		if (!data) {
			return false;
		}
		if (restricted || config.allowThirdPartyCookies) {
			const auto manager = webkit_website_data_manager_get_cookie_manager(
				data);
			const auto policySet = restricted
				? SetCookiePolicy(
					manager,
					WEBKIT_COOKIE_POLICY_ACCEPT_NEVER)
				: AllowThirdPartyCookies(manager);
			if (!policySet) {
				g_critical("Cookie policy API is unavailable.");
				g_object_unref(data);
				return false;
			}
		}
		const auto context
			= webkit_web_context_new_with_website_data_manager(data);
		g_object_unref(data);
		if (restricted) {
			webkit_web_context_set_sandbox_enabled(context, true);
		}
		if (!BlockDownloads(G_OBJECT(context)) && restricted) {
			g_object_unref(context);
			return false;
		}

		_webview = restricted
			? WEBKIT_WEB_VIEW(g_object_new(
				WEBKIT_TYPE_WEB_VIEW,
				"web-context",
				context,
				"default-content-security-policy",
				_restrictedContentSecurityPolicy.c_str(),
				nullptr))
			: WEBKIT_WEB_VIEW(webkit_web_view_new_with_context(context));
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
			instance->_window = nullptr;
			instance->_mainLoop.quit();
		}),
		this);
	g_signal_connect_swapped(
		_window,
		"realize",
		G_CALLBACK(+[](Instance *instance) {
			instance->announceCustomWindowFrame();
			instance->setupToplevelFrameExtents();
			instance->updateWindowFrameExtents();
		}),
		this);
	if (!gtk_native_get_surface) {
		// GTK 3 does it on each allocation, one call grows Wayland windows.
		g_signal_connect_swapped(
			_window,
			"size-allocate",
			G_CALLBACK(+[](Instance *instance) {
				instance->updateWindowFrameExtents();
			}),
			this);
	}
	_xftDpiChangedHandler = g_signal_connect_swapped(
		gtk_settings_get_default(),
		"notify::gtk-xft-dpi",
		G_CALLBACK(+[](Instance *instance) {
			// GTK 4 takes the shadow width only in compute-size.
			instance->updateWindowFrameExtents();
			gtk_widget_queue_resize(instance->_window);
		}),
		this);
	g_signal_connect_swapped(
		_window,
		"map",
		G_CALLBACK(+[](Instance *instance) {
			instance->exportWaylandPopupAnchor();
		}),
		this);
	g_signal_connect_swapped(
		_webview,
		"web-process-terminated",
		G_CALLBACK(+[](
				Instance *instance,
				WebKitWebProcessTerminationReason reason) {
			g_critical("Web process terminated: %d.", reason);
			instance->_mainLoop.quit();
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
	if (restricted && !BlockNativePrompts(_webview)) {
		return false;
	}
	g_signal_connect_swapped(
		_webview,
		"authenticate",
		G_CALLBACK(+[](
			Instance *instance,
			WebKitAuthenticationRequest *request) -> gboolean {
			return instance->authenticate(request);
		}),
		this);
	g_signal_connect_swapped(
		_webview,
		"permission-request",
		G_CALLBACK(+[](
			Instance *instance,
			WebKitPermissionRequest *request) -> gboolean {
			return instance->permissionRequest(request);
		}),
		this);
	if (gtk_widget_add_controller) {
		// Ahead of WebKit's own gestures, so the page doesn't get the press.
		const auto click = gtk_gesture_click_new();
		gtk_event_controller_set_propagation_phase(
			GTK_EVENT_CONTROLLER(click),
			GTK_PHASE_CAPTURE);
		g_signal_connect_swapped(
			click,
			"pressed",
			G_CALLBACK(+[](
				Instance *instance,
				int,
				double x,
				double y,
				GtkGesture *gesture) {
				instance->pressed(gesture, x, y);
			}),
			this);
		gtk_widget_add_controller(_window, GTK_EVENT_CONTROLLER(click));
		const auto key = gtk_event_controller_key_new();
		g_signal_connect_swapped(
			key,
			"key-pressed",
			G_CALLBACK(+[](
				Instance *instance,
				guint,
				guint,
				GdkModifierType) -> gboolean {
				instance->_helper.emit_user_interaction();
				return FALSE;
			}),
			this);
		gtk_widget_add_controller(
			GTK_WIDGET(_webview),
			key);
	} else {
		for (const auto signal : { "button-press-event", "touch-event" }) {
			g_signal_connect_swapped(
				_webview,
				signal,
				G_CALLBACK(+[](
					Instance *instance,
					GdkEvent *event) -> gboolean {
					return instance->pressed(event);
				}),
				this);
		}
		g_signal_connect_swapped(
			_webview,
			"key-press-event",
			G_CALLBACK(+[](
				Instance *instance,
				GdkEventKey*) -> gboolean {
				instance->_helper.emit_user_interaction();
				return FALSE;
			}),
			this);
	}
	webkit_user_content_manager_register_script_message_handler(
		manager,
		"external",
		nullptr);
	if (customWindowFrame()) {
		init(std::string("window.TelegramDesktopWindowAlphaSupported = ")
			+ (_windowSupportsAlpha ? "true" : "false")
			+ ";");
	}
	const GdkRGBA rgba{ 0.f, 0.f, 0.f, 0.f, };
	webkit_web_view_set_background_color(_webview, &rgba);
	const auto settings = webkit_web_view_get_settings(_webview);
	if (_debug) {
		webkit_settings_set_enable_developer_extras(settings, true);
	}
	if (restricted) {
		if (!ApplyRestrictedSettings(settings)) {
			return false;
		}
		webkit_web_view_set_is_muted(_webview, true);
	}
	if (gtk_window_set_child) {
		if (gtk_graphics_offload_new) {
			gtk_window_set_child(
				GTK_WINDOW(_window),
				gtk_graphics_offload_new(GTK_WIDGET(_webview)));
		} else {
			gtk_window_set_child(GTK_WINDOW(_window), GTK_WIDGET(_webview));
		}
	} else {
		gtk_container_add(GTK_CONTAINER(_window), GTK_WIDGET(_webview));
	}
	if (_platform == Platform::X11 && _mode == WindowMode::Embedded) {
		setupX11Embedding();
	} else if (_mode != WindowMode::Hidden) {
		showWindow();
	}
	init(std::string(R"(
if (window === window.top) {
	const messageToken = ')") + _messageToken + R"(';
	const handler = window.webkit.messageHandlers.external;
	const postMessage = handler.postMessage.bind(handler);
	const external = Object.freeze({
		invoke: function(s) {
			postMessage(messageToken + s);
		}
	});
	Object.defineProperty(window, 'external', {
		value: external,
		configurable: false,
		writable: false
	});
})");

	return true;
}

void Instance::scriptMessageReceived(void *message) {
	const auto received = JavascriptMessageText(message);
	if (received.size() > kMaxScriptMessageBytes + _messageToken.size()
			|| !received.starts_with(_messageToken)) {
		return;
	}
	const auto text = received.substr(_messageToken.size());
	if (handleShellControlMessage(text)) {
		return;
	}
	_helper.emit_message_received(text, webkit_web_view_get_uri(_webview));
}

bool Instance::handleShellControlMessage(const std::string &message) {
	if (_mode != WindowMode::External) {
		return false;
	}
	const auto parsed = ParseShellControlMessage(message, _shellMessageToken);
	if (parsed.status == ShellControlParseStatus::NotShellControl) {
		return false;
	} else if (parsed.status == ShellControlParseStatus::Invalid
			|| _shellMessageToken.empty()) {
		return true;
	}
	switch (parsed.action) {
	case ShellControlAction::SetDragRegions:
		setShellDragRegions(parsed.arguments);
		return true;
	case ShellControlAction::None:
		return false;
	}
	return false;
}

void Instance::setShellDragRegions(const QJsonObject &arguments) {
	auto drag = ShellRegions(arguments, "drag");
	auto noDrag = ShellRegions(arguments, "noDrag");
	if (!drag || !noDrag) {
		return;
	}
	_shellDragRegions = std::move(*drag);
	_shellNoDragRegions = std::move(*noDrag);
}

std::optional<GdkSurfaceEdge> Instance::shellResizeEdge(
		QPointF point,
		QSizeF size) const {
	if (_fullscreen) {
		return std::nullopt;
	}
	const auto margins = QMarginsF(_windowMargins) * PageScale(_window);
	const auto left = (point.x() < margins.left());
	const auto right = (point.x() >= size.width() - margins.right());
	if (point.y() < margins.top()) {
		return left
			? GDK_SURFACE_EDGE_NORTH_WEST
			: right
			? GDK_SURFACE_EDGE_NORTH_EAST
			: GDK_SURFACE_EDGE_NORTH;
	} else if (point.y() >= size.height() - margins.bottom()) {
		return left
			? GDK_SURFACE_EDGE_SOUTH_WEST
			: right
			? GDK_SURFACE_EDGE_SOUTH_EAST
			: GDK_SURFACE_EDGE_SOUTH;
	} else if (left) {
		return GDK_SURFACE_EDGE_WEST;
	} else if (right) {
		return GDK_SURFACE_EDGE_EAST;
	}
	return std::nullopt;
}

bool Instance::shellMoveArea(QPointF point) const {
	if (_fullscreen) {
		return false;
	}
	const auto css = point / PageScale(_window);
	const auto inside = [&](const std::vector<QRectF> &regions) {
		return std::any_of(regions.begin(), regions.end(), [&](
				const QRectF &region) {
			return region.contains(css);
		});
	};
	return inside(_shellDragRegions) && !inside(_shellNoDragRegions);
}

void Instance::pressed(GtkGesture *gesture, double x, double y) {
	if (_inputBlocked) {
		return;
	}
	_helper.emit_user_interaction();
	takeX11InputFocus(gtk_event_controller_get_current_event_time(
		GTK_EVENT_CONTROLLER(gesture)));
	if (!customWindowFrame()) {
		return;
	}
	const auto surface = GtkNativeSurface(_window);
	const auto toplevel = GdkToplevelFromSurface(surface);
	if (!toplevel) {
		return;
	}
	const auto point = QPointF(x, y);
	const auto size = QSizeF(
		gdk_surface_get_width(surface),
		gdk_surface_get_height(surface));
	const auto edge = shellResizeEdge(point, size);
	if (!edge && !shellMoveArea(point)) {
		return;
	}
	const auto controller = GTK_EVENT_CONTROLLER(gesture);
	const auto device = gtk_gesture_get_device(gesture);
	const auto time = gtk_event_controller_get_current_event_time(controller);
	gtk_gesture_set_state(gesture, GTK_EVENT_SEQUENCE_CLAIMED);
	if (edge) {
		gdk_toplevel_begin_resize(
			toplevel,
			*edge,
			device,
			GDK_BUTTON_PRIMARY,
			x,
			y,
			time);
	} else {
		gdk_toplevel_begin_move(
			toplevel,
			device,
			GDK_BUTTON_PRIMARY,
			x,
			y,
			time);
	}
	gtk_event_controller_reset(controller);
}

bool Instance::pressed(GdkEvent *event) {
	auto button = guint();
	const auto touch = (gdk_event_get_event_type(event) == GDK_TOUCH_BEGIN);
	if (!touch && !gdk_event_get_button(event, &button)) {
		return false;
	}
	_helper.emit_user_interaction();
	takeX11InputFocus(gdk_event_get_time(event));
	auto x = 0.;
	auto y = 0.;
	auto rootX = 0.;
	auto rootY = 0.;
	if (!customWindowFrame()
			|| (!touch && button != GDK_BUTTON_PRIMARY)
			|| !gdk_event_get_coords(event, &x, &y)
			|| !gdk_event_get_root_coords(event, &rootX, &rootY)) {
		return false;
	}
	auto width = gint();
	auto height = gint();
	gtk_window_get_size(GTK_WINDOW(_window), &width, &height);
	const auto point = QPointF(x, y);
	const auto edge = shellResizeEdge(point, QSizeF(width, height));
	if (!edge && !shellMoveArea(point)) {
		return false;
	}
	const auto window = gtk_widget_get_window(_window);
	const auto device = gdk_event_get_device(event);
	const auto time = gdk_event_get_time(event);
	if (edge) {
		gdk_window_begin_resize_drag_for_device(
			window,
			static_cast<GdkWindowEdge>(*edge),
			device,
			GDK_BUTTON_PRIMARY,
			int(rootX),
			int(rootY),
			time);
	} else {
		gdk_window_begin_move_drag_for_device(
			window,
			device,
			GDK_BUTTON_PRIMARY,
			int(rootX),
			int(rootY),
			time);
	}
	return true;
}

void Instance::takeX11InputFocus(guint32 time) {
	if (_platform != Platform::X11 || _mode != WindowMode::Embedded) {
		return;
	}
	// Qt keeps the focus on its window, so keys go here only under pointer.
	XSetInputFocus(
		gdk_x11_display_get_xdisplay(gtk_widget_get_display(_window)),
		X11WindowId(_window),
		kX11RevertToParent,
		time);
}

bool Instance::customWindowFrame() const {
	return (_mode == WindowMode::External)
		&& (_windowStyle == WindowStyle::Frameless);
}

bool Instance::transparentWindowBackground() const {
	return _windowSupportsAlpha
		&& (customWindowFrame()
			|| (_mode == WindowMode::Embedded
				&& _platform == Platform::Wayland));
}

void Instance::announceCustomWindowFrame() {
	if (!customWindowFrame() || !gdk_wayland_window_announce_csd) {
		return;
	}
	const auto gdkWindow = gtk_widget_get_window(_window);
	if (!IsGdkWaylandWindow(gdkWindow)) {
		return;
	}
	gdk_wayland_window_announce_csd(gdkWindow);
}

QMargins Instance::windowFrameExtents() const {
	return (!_fullscreen && customWindowFrame() && _windowSupportsAlpha)
		? _windowMargins * PageScale(_window)
		: QMargins();
}

void Instance::setupToplevelFrameExtents() {
	if (!customWindowFrame() || !gtk_native_get_surface) {
		return;
	}
	g_signal_connect_after(
		GtkNativeSurface(_window),
		"compute-size",
		G_CALLBACK(+[](
				GdkToplevel*,
				GdkToplevelSize *size,
				Instance *instance) {
			const auto margins = instance->windowFrameExtents();
			gdk_toplevel_size_set_shadow_width(
				size,
				margins.left(),
				margins.right(),
				margins.top(),
				margins.bottom());

			// GTK fits the shadow into the bounds too, give it back.
			auto width = gint();
			auto height = gint();
			gtk_window_get_default_size(
				GTK_WINDOW(instance->_window),
				&width,
				&height);
			auto boundsWidth = 0;
			auto boundsHeight = 0;
			gdk_toplevel_size_get_bounds(size, &boundsWidth, &boundsHeight);
			if (width < boundsWidth && height < boundsHeight) {
				return;
			}
			const auto fit = [](int value, int bounds, int shadow) {
				return (value < bounds) ? value : std::max(value, bounds + shadow);
			};
			gdk_toplevel_size_set_size(
				size,
				fit(width, boundsWidth, margins.left() + margins.right()),
				fit(height, boundsHeight, margins.top() + margins.bottom()));
		}),
		this);
}

void Instance::updateWindowFrameExtents() {
	if (!customWindowFrame() || !gdk_window_set_shadow_width) {
		return;
	}
	const auto gdkWindow = gtk_widget_get_window(_window);
	if (!gdkWindow) {
		return;
	}
	const auto margins = windowFrameExtents();
	gdk_window_set_shadow_width(
		gdkWindow,
		margins.left(),
		margins.right(),
		margins.top(),
		margins.bottom());
}

void Instance::showWindow() {
	if (!gtk_widget_show_all) {
		gtk_widget_set_visible(_window, true);
	} else {
		gtk_widget_show_all(_window);
	}
}

void Instance::setupX11Embedding() {
	// Qt maps the window after reparenting, so no window manager sees it.
	gtk_widget_realize(_window);
	// The window manager would apply GDK's initial size after reparenting.
	auto attributes = X11WindowAttributes{ .overrideRedirect = true };
	XChangeWindowAttributes(
		gdk_x11_display_get_xdisplay(gtk_widget_get_display(_window)),
		X11WindowId(_window),
		kX11CWOverrideRedirect,
		&attributes);
	if (gtk_native_get_surface) {
		gdk_x11_surface_set_frame_sync_enabled(
			GtkNativeSurface(_window),
			false);
		// GDK 4 doesn't track the window mapped by someone else.
		_x11EventHandler = g_signal_connect_swapped(
			gtk_widget_get_display(_window),
			"xevent",
			G_CALLBACK(+[](
					Instance *instance,
					const X11MapEvent *event) -> gboolean {
				if (event->type != kX11MapNotify
						|| event->window != X11WindowId(instance->_window)) {
					return false;
				}
				g_signal_handler_disconnect(
					gtk_widget_get_display(instance->_window),
					instance->_x11EventHandler);
				instance->_x11EventHandler = 0;
				GLib::idle_add_once(crl::guard(instance, [=] {
					instance->showWindow();
				}));
				return false;
			}),
			this);
		return;
	}
	gdk_x11_window_set_frame_sync_enabled(
		gtk_widget_get_window(_window),
		false);
	// GTK 3 would bring back its own size, while the embedder owns it.
	g_signal_connect(
		_window,
		"configure-event",
		G_CALLBACK(+[](GtkWidget *window) -> gboolean {
			const auto gdkWindow = gtk_widget_get_window(window);
			gtk_window_resize(
				GTK_WINDOW(window),
				gdk_window_get_width(gdkWindow),
				gdk_window_get_height(gdkWindow));
			return false;
		}),
		nullptr);
	g_signal_connect_swapped(
		_window,
		"map-event",
		G_CALLBACK(+[](Instance *instance) -> gboolean {
			instance->showWindow();
			return false;
		}),
		this);
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
		_helper.emit_navigation_started();
	} else if (loadEvent == WEBKIT_LOAD_FINISHED) {
		_helper.emit_navigation_done(!_loadFailed);
	}
	updateHistoryStates();
}

bool Instance::decidePolicy(
		WebKitPolicyDecision *decision,
		WebKitPolicyDecisionType decisionType) {
	if (decisionType == WEBKIT_POLICY_DECISION_TYPE_RESPONSE
			&& !_restrictedOrigin.empty()
			&& webkit_response_policy_decision_is_main_frame_main_resource) {
		const auto responseDecision = WEBKIT_RESPONSE_POLICY_DECISION(decision);
		if (!webkit_response_policy_decision_is_main_frame_main_resource(
				responseDecision)) {
			return false;
		}
		const auto response = webkit_response_policy_decision_get_response(
			responseDecision);
		const auto mime = response
			? webkit_uri_response_get_mime_type(response)
			: nullptr;
		if (!mime || g_ascii_strcasecmp(mime, "text/html")) {
			webkit_policy_decision_ignore(decision);
			_loadFailed = true;
			return true;
		}
		return false;
	}
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
	g_object_ref(decision);
	_master.call_navigation_policy(uri, false, [=](
			GObjectCpp::Object source_object,
			Gio::AsyncResult res) {
		const auto result = _master.call_navigation_policy_finish(res);
		// Fallback to default decision on object destruction
		if (!result || !std::get<1>(*result)) {
			webkit_policy_decision_ignore(decision);
		}
		g_object_unref(decision);
	});
	return true;
}

GtkWidget *Instance::createAnother(WebKitNavigationAction *action) {
	if (!_restrictedOrigin.empty()) {
		return nullptr;
	}
	WebKitURIRequest *request = webkit_navigation_action_get_request(action);
	const std::string uri = webkit_uri_request_get_uri(request);
	_master.call_navigation_policy(uri, true, [=](
			GObjectCpp::Object source_object,
			Gio::AsyncResult res) {
		const auto ret = _master.call_navigation_policy_finish(res);
		if (!ret || !std::get<1>(*ret)) {
			return;
		}
		if (gtk_uri_launcher_new && gtk_uri_launcher_launch) {
			const auto launcher = gtk_uri_launcher_new(uri.c_str());
			gtk_uri_launcher_launch(
				launcher,
				GTK_WINDOW(_window),
				nullptr,
				nullptr,
				nullptr);
			g_object_unref(launcher);
		} else if (gtk_show_uri_on_window) {
			gtk_show_uri_on_window(
				GTK_WINDOW(_window),
				uri.c_str(),
				GDK_CURRENT_TIME,
				nullptr);
		} else {
			gtk_show_uri(GTK_WINDOW(_window), uri.c_str(), GDK_CURRENT_TIME);
		}
	});
	return nullptr;
}

bool Instance::scriptDialog(WebKitScriptDialog *dialog) {
	const auto type = webkit_script_dialog_get_dialog_type(dialog);
	if (!_restrictedOrigin.empty()) {
		if (type == WEBKIT_SCRIPT_DIALOG_PROMPT) {
			webkit_script_dialog_prompt_set_text(dialog, nullptr);
		} else if (type != WEBKIT_SCRIPT_DIALOG_ALERT) {
			webkit_script_dialog_confirm_set_confirmed(dialog, false);
		}
		return true;
	}
	const auto text = webkit_script_dialog_get_message(dialog);
	const auto value = (type == WEBKIT_SCRIPT_DIALOG_PROMPT)
		? webkit_script_dialog_prompt_get_default_text(dialog)
		: nullptr;
	webkit_script_dialog_ref(dialog);
	// Script dialogs wait for the user longer than 25 seconds.
	auto proxy = gi::object_cast<MasterProxy>(_master);
	const auto timeout = proxy.get_default_timeout();
	proxy.set_default_timeout(G_MAXINT);
	_master.call_script_dialog(
		type,
		text ? text : "",
		value ? value : "",
		[=](GObjectCpp::Object source_object, Gio::AsyncResult res) {
			bool accepted = false;
			std::string result;
			if (const auto ret = _master.call_script_dialog_finish(res)) {
				std::tie(std::ignore, accepted, result) = *ret;
			}
			if (type == WEBKIT_SCRIPT_DIALOG_PROMPT) {
				webkit_script_dialog_prompt_set_text(
					dialog,
					accepted ? result.c_str() : nullptr);
			} else if (type != WEBKIT_SCRIPT_DIALOG_ALERT) {
				webkit_script_dialog_confirm_set_confirmed(dialog, accepted);
			}
			webkit_script_dialog_unref(dialog);
		});
	proxy.set_default_timeout(timeout);
	return true;
}

bool Instance::authenticate(WebKitAuthenticationRequest *request) {
	if (!_restrictedOrigin.empty()) {
		webkit_authentication_request_cancel(request);
		return true;
	}
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

bool Instance::permissionRequest(WebKitPermissionRequest *request) {
	if (!_restrictedOrigin.empty()) {
		webkit_permission_request_deny(request);
		return true;
	}
	// navigator.clipboard.read/readText() asks for this one. Never let the
	// page read the clipboard on its own, the embedder is expected to expose
	// its own method for that, gated on a real user interaction.
	//
	// WebKitGTK denies unhandled requests by default, we make it explicit.
	if (webkit_clipboard_permission_request_get_type
			&& WEBKIT_IS_CLIPBOARD_PERMISSION_REQUEST(request)) {
		webkit_permission_request_deny(request);
		return true;
	}
	const auto type = [&]() -> std::optional<PermissionType> {
		if (WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST(request)) {
			return PermissionType::Geolocation;
		} else if (!WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(request)) {
			return std::nullopt;
		}
		const auto media = WEBKIT_USER_MEDIA_PERMISSION_REQUEST(request);
		const auto audio = webkit_user_media_permission_is_for_audio_device(
			media);
		const auto video = webkit_user_media_permission_is_for_video_device(
			media);
		if (audio && video) {
			return PermissionType::CameraAndMicrophone;
		} else if (audio) {
			return PermissionType::Microphone;
		} else if (video) {
			return PermissionType::Camera;
		}
		return std::nullopt;
	}();
	if (!type) {
		return false;
	}
	g_object_ref(request);
	// Permission requests wait for the user longer than 25 seconds
	auto proxy = gi::object_cast<MasterProxy>(_master);
	const auto timeout = proxy.get_default_timeout();
	proxy.set_default_timeout(G_MAXINT);
	_master.call_permission_request(int(*type), [=](
			GObjectCpp::Object source_object,
			Gio::AsyncResult res) {
		const auto result = _master.call_permission_request_finish(res);
		if (result && std::get<1>(*result)) {
			webkit_permission_request_allow(request);
		} else {
			webkit_permission_request_deny(request);
		}
		g_object_unref(request);
	});
	proxy.set_default_timeout(timeout);
	return true;
}

// https://bugs.webkit.org/show_bug.cgi?id=146351
bool Instance::startDataServer() {
	if (_dataServer) {
		return true;
	}

	_dataServer.emplace(
		(_dataPassword = GenerateSecret()).c_str(),
		QByteArray::fromStdString(_dataRequestRedirectHost),
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

		auto loop = GLib::MainLoop::new_();
		std::optional<ResolveResult> result;
		_helper.call_resolve([&](
				GObjectCpp::Object source_object,
				Gio::AsyncResult res) {
			const auto reply = _helper.call_resolve_finish(res);
			if (reply) {
				result = ResolveResult(std::get<1>(*reply));
			}
			loop.quit();
		});

		loop.run();
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

	return Resolve(_platform);
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

void Instance::loadHtml(std::string html, std::string baseUrl) {
	if (_remoting) {
		if (!_helper) {
			return;
		}

		_helper.call_load_html(html, baseUrl, nullptr);
		return;
	}

	webkit_web_view_load_alternate_html(
		_webview,
		html.c_str(),
		baseUrl.c_str(),
		baseUrl.c_str());
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

	addUserScript(std::move(js), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME);
}

void Instance::initAllFrames(std::string js) {
	if (_remoting) {
		if (!_helper) {
			return;
		}

		_helper.call_init_all_frames(js, nullptr);
		return;
	}

	addUserScript(std::move(js), WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES);
}

void Instance::addUserScript(
		std::string js,
		WebKitUserContentInjectedFrames frames) {
	WebKitUserContentManager *manager
		= webkit_web_view_get_user_content_manager(_webview);
	webkit_user_content_manager_add_script(
		manager,
		webkit_user_script_new(
			js.c_str(),
			frames,
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

void Instance::focus() {
	if (_mode != WindowMode::External) {
		_widget->activateWindow();
		return;
	}

	if (_remoting) {
		if (!_helper) {
			return;
		}

		// Give Wayland QPA time to process focus window change
		// in case we're called by a click on unfocused window
		InvokeQueued(qApp, crl::guard(this, [=] {
			::base::Platform::RunWithXdgActivationToken(crl::guard(
				this,
				[=](const QString &token) {
					_helper.call_focus(token.toStdString(), nullptr);
				}));
		}));
		return;
	}

	const auto window = GTK_WINDOW(_window);
	const auto startupId = [&] {
		if (!_xdgActivationToken.empty()) {
			return ::base::take(_xdgActivationToken);
		} else if (gtk_native_get_surface) {
			const auto surface = GtkNativeSurface(_window);
			if (IsGdkX11Surface(surface)) {
				return std::string("_TIME")
					+ std::to_string(gdk_x11_get_server_time(surface));
			}
		} else if (const auto gdkWindow = gtk_widget_get_window(_window)
				; IsGdkX11Window(gdkWindow)) {
			return std::string("_TIME")
				+ std::to_string(gdk_x11_get_server_time(gdkWindow));
		}
		return std::string();
	}();

	if (!startupId.empty()) {
		gtk_window_set_startup_id(window, startupId.c_str());
	}
	gtk_window_present(window);
}

void Instance::setInteractionHandler(Fn<void()> handler) {
	_interactionHandler = std::move(handler);
}

QWidget *Instance::widget() {
	return _widget.get();
}

void Instance::exportWaylandPopupAnchor() {
	using Weak = ::base::weak_ptr<Instance>;
	const auto destroyWeak = +[](gpointer userData) {
		delete static_cast<Weak*>(userData);
	};
	if (_mode != WindowMode::External) {
		return;
	}
	auto weak = std::make_unique<Weak>(this);
	if (gtk_native_get_surface) {
		const auto toplevel = GdkWaylandToplevelFromSurface(
			GtkNativeSurface(_window));
		if (!toplevel) {
			return;
		}
		const auto exported = gdk_wayland_toplevel_export_handle(
			toplevel,
			+[](GdkToplevel*, const char *handle, gpointer userData) {
				if (const auto instance = static_cast<Weak*>(userData)->get()) {
					instance->_waylandPopupAnchorHandle = QString::fromUtf8(
						handle);
				}
			},
			weak.get(),
			destroyWeak);
		if (exported) {
			weak.release();
		}
	} else {
		const auto gdkWindow = gtk_widget_get_window(_window);
		if (!IsGdkWaylandWindow(gdkWindow)) {
			return;
		}
		const auto exported = gdk_wayland_window_export_handle(
			gdkWindow,
			+[](GdkWindow*, const char *handle, gpointer userData) {
				if (const auto instance = static_cast<Weak*>(userData)->get()) {
					instance->_waylandPopupAnchorHandle = QString::fromUtf8(
						handle);
				}
			},
			weak.get(),
			destroyWeak);
		if (exported) {
			weak.release();
		}
	}
}

void *Instance::winId() {
	if (_remoting) {
		if (!_helper) {
			return nullptr;
		}

		auto loop = GLib::MainLoop::new_();
		void *ret = nullptr;
		_helper.call_get_win_id([&](
				GObjectCpp::Object source_object,
				Gio::AsyncResult res) {
			const auto reply = _helper.call_get_win_id_finish(res);
			if (reply) {
				ret = reinterpret_cast<void*>(std::get<1>(*reply));
			}
			loop.quit();
		});

		loop.run();
		return ret;
	}

	return reinterpret_cast<void*>(X11WindowId(_window));
}

PopupAnchor Instance::popupAnchorSnapshot() {
	auto result = PopupAnchor();
	auto width = gint(0);
	auto height = gint(0);
	if (gtk_native_get_surface) {
		if (const auto surface = GtkNativeSurface(_window)) {
			width = gdk_surface_get_width(surface);
			height = gdk_surface_get_height(surface);
		}
	} else {
		gtk_window_get_size(GTK_WINDOW(_window), &width, &height);
	}
	if (width > 0 && height > 0) {
		result.outerSize = QSize(width, height).shrunkBy(
			windowFrameExtents()
		) / PageScale(_window);
	}
	if (const auto nativeId = X11WindowId(_window)) {
		result.transientParent = {
			.type = Ui::Platform::ForeignParent::Type::X11,
			.x11 = nativeId,
		};
	} else if (!_waylandPopupAnchorHandle.isEmpty()) {
		result.transientParent = {
			.type = Ui::Platform::ForeignParent::Type::Wayland,
			.wayland = _waylandPopupAnchorHandle,
		};
	}
	return result;
}

bool Instance::notifyExternalWindowClosed() {
	if (_mode != WindowMode::External) {
		return false;
	} else if (_externalWindowCloseAllowed) {
		return false;
	} else if (_externalWindowClosePending) {
		return true;
	}
	_externalWindowClosePending = true;
	_master.call_external_window_closed([=](
			GObjectCpp::Object,
			Gio::AsyncResult res) {
		_externalWindowClosePending = false;
		_master.call_external_window_closed_finish(res);
		_externalWindowCloseAllowed = true;
		if (gtk_window_destroy) {
			gtk_window_destroy(GTK_WINDOW(_window));
		} else {
			gtk_widget_destroy(_window);
		}
	});
	return true;
}

void Instance::fullscreenChanged(bool fullscreen) {
	if (_fullscreen == fullscreen) {
		return;
	}
	_fullscreen = fullscreen;
	updateWindowFrameExtents();
	_helper.emit_fullscreen_changed(fullscreen);
}

PopupAnchor Instance::popupAnchor() {
	if (_remoting) {
		if (!_helper) {
			return {};
		}

		auto loop = GLib::MainLoop::new_();
		auto result = PopupAnchor();
		_helper.call_get_window_anchor([&](
				GObjectCpp::Object source_object,
				Gio::AsyncResult res) {
			if (const auto reply = _helper.call_get_window_anchor_finish(res)) {
				if (const auto parent = PopupAnchorParent(
						std::get<1>(*reply),
						std::get<2>(*reply),
						std::get<3>(*reply))) {
					result.transientParent = parent;
				}
				if (const auto outerSize = PopupAnchorOuterSize(
						std::get<4>(*reply),
						std::get<5>(*reply),
						std::get<6>(*reply))) {
					result.outerSize = *outerSize;
				}
			}
			loop.quit();
		});

		loop.run();
		return result;
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

	const auto background = std::format(
		".webviewWindow {{background: {};}}",
		transparentWindowBackground()
			? "transparent"
			: customWindowFrame()
			? kExternalShellFallbackBackground
			: opaqueBg.name().toStdString());

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
}

void Instance::resize(int w, int h) {
	if (_remoting) {
		if (!_helper) {
			return;
		}

		_helper.call_resize(w, h, nullptr);
		return;
	}

	const auto size = QSize(w, h) * PageScale(_window);
	if (!gtk_window_resize) {
		gtk_window_set_default_size(
			GTK_WINDOW(_window),
			size.width(),
			size.height());
	} else {
		gtk_window_resize(GTK_WINDOW(_window), size.width(), size.height());
	}
}

void Instance::setFullscreen(bool fullscreen) {
	if (_remoting) {
		if (!_helper) {
			return;
		}

		_helper.call_set_fullscreen(fullscreen, nullptr);
		return;
	}

	if (fullscreen) {
		gtk_window_fullscreen(GTK_WINDOW(_window));
	} else {
		gtk_window_unfullscreen(GTK_WINDOW(_window));
	}
}

void Instance::setInputBlocked(bool blocked) {
	if (_remoting) {
		if (!_helper) {
			return;
		}

		_helper.call_set_input_blocked(blocked, nullptr);
		return;
	}

	_inputBlocked = blocked;
	// GTK 4 stops tracking the activity of an insensitive window.
	if (gtk_widget_set_can_target) {
		gtk_widget_set_can_target(_window, !blocked);
		return;
	}
	gtk_widget_set_sensitive(_window, !blocked);
	if (!blocked) {
		// GTK takes the focus from an insensitive widget.
		gtk_widget_grab_focus(GTK_WIDGET(_webview));
	}
}

void Instance::setVisible(bool visible) {
	if (_remoting) {
		if (!_helper) {
			return;
		}

		_helper.call_set_visible(visible, nullptr);
		return;
	}

	if (visible) {
		showWindow();
	} else {
		gtk_widget_set_visible(_window, false);
	}
}

void Instance::startProcess() {
	auto loop = GLib::MainLoop::new_();

	auto serviceLauncher = Gio::SubprocessLauncher::new_(
		Gio::SubprocessFlags::NONE_);

	if (_platform == Platform::Wayland
			&& _mode == WindowMode::Embedded
			&& _glBackend == Ui::GL::Backend::Raster) {
		serviceLauncher.setenv("LIBGL_ALWAYS_SOFTWARE", "1", true);
		serviceLauncher.setenv("GSK_RENDERER", "cairo", true);
		serviceLauncher.setenv("GDK_DISABLE", "gl", true);
		serviceLauncher.setenv("GDK_DEBUG", "gl-disable", true);
		serviceLauncher.setenv("GDK_GL", "disable", true);
		serviceLauncher.setenv("WEBKIT_DISABLE_COMPOSITING_MODE", "1", true);
	} else if (_platform == Platform::Any || _mode == WindowMode::External) {
		const auto token = ::base::Platform::XdgActivationToken().toStdString();
		if (!token.empty()) {
			serviceLauncher.setenv("XDG_ACTIVATION_TOKEN", token, true);
		}
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

	Gio::File::new_for_path(socketPath).delete_();

	if (_platform == Platform::Wayland && _mode == WindowMode::Embedded) {
		_compositor.emplace(
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

		auto helper = HelperProxy::new_sync(
			connection,
			Gio::DBusProxyFlags::DO_NOT_LOAD_PROPERTIES_,
			kHelperObjectPath);

		if (!helper) {
			LOG(("WebView Error: %1").arg(
				helper.error().message_().c_str()));
			loop.quit();
			return true;
		}

		_helper = *helper;
		registerHelperSignalHandlers();

		_helper.call_set_start_data(
			int(_platform),
			int(_mode),
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
			}(),
			crl::guard(&guard, [&](
					GObjectCpp::Object source_object,
					Gio::AsyncResult res) {
				const auto result = _helper.call_set_start_data_finish(res);
				if (!result) {
					LOG(("WebView Error: %1").arg(
						result.error().message_().c_str()));
				}
				loop.quit();
			}));

		connection.signal_closed().connect(crl::guard(this, [=](
				Gio::DBusConnection,
				bool remotePeerVanished,
				GLib::Error_Ref error) {
			_widget = nullptr;
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
	_dbusServer.disconnect(newConnection);
}

void Instance::stopProcess() {
	if (_dbusServer) {
		_dbusServer.stop();
	}
	if (_serviceProcess) {
		_serviceProcess.send_signal(SIGTERM);
	}
	_compositor = nullptr;
}

void Instance::updateHistoryStates() {
	const auto title = webkit_web_view_get_title(_webview);
	if ((_platform == Platform::Any) || (_mode == WindowMode::External)) {
		gtk_window_set_title(GTK_WINDOW(_window), title ? title : "");
	}
	_helper.emit_navigation_state_update(
		webkit_web_view_get_uri(_webview),
		title ? title : "",
		webkit_web_view_can_go_back(_webview),
		webkit_web_view_can_go_forward(_webview));
}

void Instance::registerMasterMethodHandlers() {
	_master.signal_handle_navigation_policy().connect([=](
			Master,
			Gio::DBusMethodInvocation invocation,
			const std::string &uri,
			bool newWindow) {
		if (newWindow) {
			if (_navigationPolicyHandler
					&& _navigationPolicyHandler(uri, true)) {
				if (_platform == Platform::Any
						|| _mode == WindowMode::External) {
					_master.complete_navigation_policy(invocation, true);
					return true;
				}
				QDesktopServices::openUrl(QString::fromStdString(uri));
			}
			_master.complete_navigation_policy(invocation, false);
		} else if (!uri.starts_with(dataDomain())
				&& _navigationPolicyHandler
				&& !_navigationPolicyHandler(uri, false)) {
			_master.complete_navigation_policy(invocation, false);
		} else {
			_master.complete_navigation_policy(invocation, true);
		}
		return true;
	});

	_master.signal_handle_external_window_closed().connect([=](
			Master,
			Gio::DBusMethodInvocation invocation) {
		if (!_externalWindowCloseHandler) {
			return false;
		}
		_externalWindowCloseHandler();
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
			return false;
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
			const auto handled = _asyncDialogHandler(args, crl::guard(this, [=](
					DialogResult result) mutable {
				_master.complete_script_dialog(
					invocation,
					result.accepted,
					result.text);
			}));
			if (handled) {
				return true;
			}
		}

		// The handler spins a nested event loop, and anything running in it
		// may destroy this instance together with `_master`.
		const auto weak = ::base::make_weak(this);
		const auto result = _dialogHandler(std::move(args));
		if (!weak) {
			return false;
		}

		_master.complete_script_dialog(
			invocation,
			result.accepted,
			result.text);

		return true;
	});

	_master.signal_handle_permission_request().connect([=](
			Master,
			Gio::DBusMethodInvocation invocation,
			int type) {
		if (!_permissionHandler) {
			return false;
		}
		_permissionHandler(
			PermissionType(type),
			crl::guard(this, [=](bool allowed) mutable {
				_master.complete_permission_request(invocation, allowed);
			}));
		return true;
	});
}

int Instance::exec() {
	_mainLoop = GLib::MainLoop::new_();

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
		Gio::DBusConnectionFlags::AUTHENTICATION_CLIENT_
			| Gio::DBusConnectionFlags::DELAY_MESSAGE_PROCESSING_);

	if (!connection) {
		g_error("%s", connection.error().message_().c_str());
	}

	_helper = HelperSkeleton::new_();
	auto object = ObjectSkeleton::new_(kHelperObjectPath);
	object.set_helper(_helper);
	_dbusObjectManager = Gio::DBusObjectManagerServer::new_(kObjectPath);
	_dbusObjectManager.export_(object);
	_dbusObjectManager.set_connection(*connection);
	registerHelperMethodHandlers();
	connection->start_message_processing();

	auto master = MasterProxy::new_sync(
		*connection,
		Gio::DBusProxyFlags::DO_NOT_LOAD_PROPERTIES_,
		kMasterObjectPath);

	if (!master) {
		g_error("%s", master.error().message_().c_str());
	}

	_master = *master;
	_master.signal_data_server_started().connect([=](
			Master,
			std::uint16_t port,
			const std::string &password) {
		_dataPort = port;
		_dataPassword = password;
	});

	connection->signal_closed().connect([&](
			Gio::DBusConnection,
			bool remotePeerVanished,
			GLib::Error_Ref error) {
		_mainLoop.quit();
	});

	_mainLoop.run();
	return 0;
}

void Instance::registerHelperMethodHandlers() {
	_helper.signal_handle_set_start_data().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation,
			int platform,
			int mode,
			const std::string &waylandDisplay,
			const std::string &appId) {
		_platform = Platform(platform);
		_mode = WindowMode(mode);
		if (!waylandDisplay.empty()) {
			GLib::setenv("WAYLAND_DISPLAY", waylandDisplay, true);
		}
		if (!appId.empty()) {
			_applicationId = appId;
		}
		_helper.complete_set_start_data(invocation);
		return true;
	});

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
			int initialHeight,
			bool allowThirdPartyCookies,
			const std::string &restrictedOrigin,
			const std::string &restrictedContentSecurityPolicy) {
		if (!create({
			.opaqueBg = QColor(r, g, b, a),
			.userDataPath = path,
			.debug = debug,
			.allowThirdPartyCookies = allowThirdPartyCookies,
			.mode = WindowMode(mode),
			.windowStyle = WindowStyle(windowStyle),
			.windowMargins = QMargins(
				marginLeft,
				marginTop,
				marginRight,
				marginBottom),
			.initialSize = QSize(initialWidth, initialHeight),
			.shellMessageToken = shellMessageToken,
			.restrictedOrigin = restrictedOrigin,
			.restrictedContentSecurityPolicy
				= restrictedContentSecurityPolicy,
		})) {
			return false;
		}
		_helper.complete_create(invocation);
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
			Gio::DBusMethodInvocation invocation) {
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

	_helper.signal_handle_load_html().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation,
			const std::string &html,
			const std::string &baseUrl) {
		loadHtml(html, baseUrl);
		_helper.complete_load_html(invocation);
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

	_helper.signal_handle_set_input_blocked().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation,
			bool blocked) {
		setInputBlocked(blocked);
		_helper.complete_set_input_blocked(invocation);
		return true;
	});

	_helper.signal_handle_set_visible().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation,
			bool visible) {
		setVisible(visible);
		_helper.complete_set_visible(invocation);
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

	_helper.signal_handle_init_all_frames().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation,
			const std::string &js) {
		initAllFrames(js);
		_helper.complete_init_all_frames(invocation);
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

	_helper.signal_handle_focus().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation,
			std::string token) {
		_xdgActivationToken = token;
		focus();
		_helper.complete_focus(invocation);
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
		const auto outerSize = anchor.outerSize.value_or(QSize());
		_helper.complete_get_window_anchor(
			invocation,
			int(anchor.transientParent.type),
			uint64(anchor.transientParent.x11),
			anchor.transientParent.wayland.toStdString(),
			anchor.outerSize.has_value(),
			outerSize.width(),
			outerSize.height());
		return true;
	});
}

void Instance::registerHelperSignalHandlers() {
	_helper.signal_message_received().connect([=](
			Helper,
			const std::string &message,
			const std::string &sourceUrl) {
		if (_messageHandler) {
			_messageHandler(Message{
				.text = message,
				.sourceUrl = sourceUrl,
			});
		}
	});

	_helper.signal_navigation_started().connect([=](Helper) {
		if (_navigationStartHandler) {
			_navigationStartHandler();
		}
	});

	_helper.signal_navigation_done().connect([=](Helper, bool success) {
		if (_navigationDoneHandler) {
			_navigationDoneHandler(success);
		}
	});

	_helper.signal_fullscreen_changed().connect([=](Helper, bool fullscreen) {
		if (_fullscreenChangedHandler) {
			_fullscreenChangedHandler(fullscreen);
		}
	});

	_helper.signal_navigation_state_update().connect([=](
			Helper,
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
	});

	_helper.signal_user_interaction().connect([=](Helper) {
		if (_interactionHandler) {
			_interactionHandler();
		}
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
			"(webkitgtk-6.0/webkit2gtk-4.1/webkit2gtk-4.0) "
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

bool HiddenSupported() {
	Instance instance(true, WindowMode::Hidden);
	return instance.resolve() == ResolveResult::Success;
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
