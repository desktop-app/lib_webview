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

#include <QtCore/QUrl>
#include <QtNetwork/QTcpSocket>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>
#include <QtGui/QtEvents>
#include <QtWidgets/QWidget>

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

class Instance final : public Interface, public ::base::has_weak_ptr {
public:
	Instance(bool remoting = true);
	~Instance();

	bool create(Config config);
	ResolveResult resolve();
	bool startDataServer();

	void resize(int w, int h);

	void navigate(std::string url) override;
	void navigateToData(std::string id) override;
	void reload() override;

	void init(std::string js) override;
	void eval(std::string js) override;

	void focus() override;

	QWidget *widget() override;

	void refreshNavigationHistoryState() override;
	auto navigationHistoryState()
		-> rpl::producer<NavigationHistoryState> override;

	void setOpaqueBg(QColor opaqueBg) override;

	int exec();

private:
	void scriptMessageReceived(void *message);

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

	void *winId();

	bool _remoting = false;
	bool _connected = false;
	Master _master;
	Helper _helper;
	Gio::DBusServer _dbusServer;
	Gio::DBusObjectManagerServer _dbusObjectManager;
	Gio::Subprocess _serviceProcess;

	Platform _platform = Platform::Any;
	::base::unique_qptr<QWidget> _widget;
	QPointer<Compositor> _compositor;
	std::optional<HttpServer> _dataServer;

	GtkWidget *_window = nullptr;
	WebKitWebView *_webview = nullptr;
	GtkCssProvider *_backgroundProvider = nullptr;

	bool _debug = false;
	std::function<void(std::string)> _messageHandler;
	std::function<bool(std::string,bool)> _navigationStartHandler;
	std::function<void(bool)> _navigationDoneHandler;
	std::function<DialogResult(DialogArgs)> _dialogHandler;
	rpl::variable<NavigationHistoryState> _navigationHistoryState;
	std::function<DataResult(DataRequest)> _dataRequestHandler;
	std::uint16_t _dataPort = 0;
	std::string _dataPassword;
	bool _loadFailed = false;

};

Instance::Instance(bool remoting)
: _remoting(remoting) {
	if (_remoting) {
		_platform = ::Platform::IsX11()
			? Platform::X11
#ifdef DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
			: Platform::Wayland;
#else // DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
			: Platform::Any;
#endif // !DESKTOP_APP_WEBVIEW_WAYLAND_COMPOSITOR
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
				if (Ui::GL::ChooseBackendDefault(Ui::GL::CheckCapabilities())
						!= Ui::GL::Backend::OpenGL) {
					_platform = Platform::Any;
					stopProcess();
					startProcess();
					return create(std::move(config));
				}
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
	_dialogHandler = std::move(config.dialogHandler);
	_dataRequestHandler = std::move(config.dataRequestHandler);

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
		_helper.call_create(debug, r, g, b, a, path, crl::guard(&guard, [&](
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

		switch (_platform) {
		case Platform::Any:
			_widget = ::base::make_unique_q<QWidget>(config.parent);
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
			_widget->show();
			break;
		case Platform::X11:
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

	_window = _platform == Platform::X11
		? gtk_plug_new(0)
		: gtk_window_new(GTK_WINDOW_TOPLEVEL);
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
			instance->_window = nullptr;
			Gio::Application::get_default().quit();
		}),
		this);
	g_signal_connect_swapped(
		_webview,
		"web-process-terminated",
		G_CALLBACK(+[](
			Instance *instance,
			WebKitWebProcessTerminationReason reason) {
			Gio::Application::get_default().quit();
		}),
		this);
	g_signal_connect_swapped(
		_webview,
		"notify::is-web-process-responsive",
		G_CALLBACK(+[](
			Instance *instance,
			GParamSpec *pspec) {
			Gio::Application::get_default().quit();
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
	webkit_user_content_manager_register_script_message_handler(
		manager,
		"external",
		nullptr);
	const GdkRGBA rgba{ 0.f, 0.f, 0.f, 0.f, };
	webkit_web_view_set_background_color(_webview, &rgba);
	if (_debug) {
		WebKitSettings *settings = webkit_web_view_get_settings(_webview);
		webkit_settings_set_enable_developer_extras(settings, true);
	}
	if (gtk_window_set_child) {
		gtk_window_set_child(GTK_WINDOW(_window), GTK_WIDGET(_webview));
	} else if (_platform == Platform::X11) {
		const auto x11SizeFix = gtk_scrolled_window_new(nullptr, nullptr);
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
	init(R"(
window.external = {
	invoke: function(s) {
		window.webkit.messageHandlers.external.postMessage(s);
	}
};)");

	return webkit_web_view_get_is_web_process_responsive(_webview);
}

void Instance::scriptMessageReceived(void *message) {
	if (!_master) {
		return;
	}
	_master.call_message_received([&] {
		const auto s = jsc_value_to_string(
			!webkit_javascript_result_get_js_value
				? reinterpret_cast<JSCValue*>(message)
				: webkit_javascript_result_get_js_value(
					reinterpret_cast<WebKitJavascriptResult*>(message)));
		const auto guard = gsl::finally([&] {
			g_free(s);
		});
		return std::string(s);
	}(), nullptr);
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
		_helper.call_resolve(crl::guard(&guard, [&](
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
	if (const auto widget = _widget.get()) {
		widget->activateWindow();
	}
}

QWidget *Instance::widget() {
	return _widget.get();
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

	if (_platform != Platform::X11) {
		return nullptr;
	}

	return reinterpret_cast<void*>(gtk_plug_get_id(GTK_PLUG(_window)));
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
		_platform == Platform::Wayland
			? "transparent"
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

	gtk_widget_set_size_request(_window, w, h);
	GLib::timeout_add_seconds_once(1, crl::guard(this, [=] {
		gtk_widget_set_size_request(_window, -1, -1);
	}));
}

void Instance::startProcess() {
	auto loop = GLib::MainLoop::new_();

	auto serviceLauncher = Gio::SubprocessLauncher::new_(
		Gio::SubprocessFlags::NONE_);

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
			_messageHandler(message);
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

		const auto result = _dialogHandler(DialogArgs{
			.type = dialogType,
			.value = value,
			.text = text,
		});

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
			const std::string &path) {
		if (create({
			.opaqueBg = QColor(r, g, b, a),
			.userDataPath = path,
			.debug = debug,
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

	_helper.signal_handle_resize().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation,
			int w,
			int h) {
		resize(w, h);
		_helper.complete_resize(invocation);
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
	auto result = std::make_unique<Instance>();
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
