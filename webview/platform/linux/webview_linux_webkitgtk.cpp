// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/linux/webview_linux_webkitgtk.h"

#include "webview/platform/linux/webview_linux_webkitgtk_library.h"
#include "webview/platform/linux/webview_linux_compositor.h"
#include "base/platform/base_platform_info.h"
#include "base/debug_log.h"
#include "base/integration.h"
#include "base/unique_qptr.h"
#include "base/weak_ptr.h"
#include "ui/gl/gl_detection.h"

#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>
#include <QtQuickWidgets/QQuickWidget>

#include <webview/webview.hpp>
#include <crl/crl.h>
#include <regex>

namespace Webview::WebKitGTK {
namespace {

using namespace gi::repository;
using namespace gi::repository::Webview;
using namespace Library;
namespace GObject = gi::repository::GObject;

constexpr auto kObjectPath = "/org/desktop_app/GtkIntegration/Webview";
constexpr auto kMasterObjectPath = "/org/desktop_app/GtkIntegration/Webview/Master";
constexpr auto kHelperObjectPath = "/org/desktop_app/GtkIntegration/Webview/Helper";

void (* const SetGraphicsApi)(QSGRendererInterface::GraphicsApi) =
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	QQuickWindow::setGraphicsApi;
#else // Qt >= 6.0.0
	QQuickWindow::setSceneGraphBackend;
#endif // Qt < 6.0.0

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

bool PreferWayland() {
	if (!Platform::IsX11()) {
		return true;
	}
	const auto platform = Platform::GetWindowManager().toLower();
	return platform.contains("mutter") || platform.contains("gnome");
}

class Instance final : public Interface, public ::base::has_weak_ptr {
public:
	Instance(bool remoting = true);
	~Instance();

	bool create(Config config);

	ResolveResult resolve();

	bool finishEmbedding() override;

	void navigate(std::string url) override;
	void navigateToData(std::string id) override;
	void reload() override;

	void resizeToWindow() override;

	void init(std::string js) override;
	void eval(std::string js) override;

	void focus() override;

	QWidget *widget() override;
	void *winId() override;

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

	void startProcess();
	void stopProcess();

	void registerMasterMethodHandlers();
	void registerHelperMethodHandlers();

	bool _remoting = false;
	bool _connected = false;
	Master _master;
	Helper _helper;
	Gio::DBusServer _dbusServer;
	Gio::DBusObjectManagerServer _dbusObjectManager;
	Gio::Subprocess _serviceProcess;

	bool _wayland = false;
	::base::unique_qptr<QWidget> _widget;
	QPointer<Compositor> _compositor;

	GtkWidget *_window = nullptr;
	GtkWidget *_webview = nullptr;
	GtkCssProvider *_backgroundProvider = nullptr;

	bool _debug = false;
	std::function<void(std::string)> _messageHandler;
	std::function<bool(std::string,bool)> _navigationStartHandler;
	std::function<void(bool)> _navigationDoneHandler;
	std::function<DialogResult(DialogArgs)> _dialogHandler;
	bool _loadFailed = false;

};

Instance::Instance(bool remoting)
: _remoting(remoting) {
	if (_remoting) {
		_wayland = PreferWayland();
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
	if (_webview) {
		if (!gtk_widget_destroy) {
			g_object_unref(_webview);
		} else {
			gtk_widget_destroy(_webview);
		}
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
	_debug = config.debug;
	_messageHandler = std::move(config.messageHandler);
	_navigationStartHandler = std::move(config.navigationStartHandler);
	_navigationDoneHandler = std::move(config.navigationDoneHandler);
	_dialogHandler = std::move(config.dialogHandler);

	if (_remoting) {
		if (resolve() != ResolveResult::Success) {
			return false;
		}

		if (_compositor && !qobject_cast<QQuickWidget*>(_widget)) {
			[[maybe_unused]] static const auto Inited = [] {
				const auto backend = Ui::GL::ChooseBackendDefault(
					Ui::GL::CheckCapabilities(nullptr));
				switch (backend) {
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
			const auto widget = static_cast<QQuickWidget*>(_widget.get());
			widget->setAttribute(Qt::WA_AlwaysStackOnTop);
			widget->setClearColor(Qt::transparent);
			_compositor->setWidget(widget);
		}

		if (!_helper) {
			return false;
		}

		const ::base::has_weak_ptr guard;
		std::optional<bool> success;
		const auto r = config.opaqueBg.red();
		const auto g = config.opaqueBg.green();
		const auto b = config.opaqueBg.blue();
		const auto a = config.opaqueBg.alpha();
		_helper.call_create(_debug, r, g, b, a, crl::guard(&guard, [&](
				GObject::Object source_object,
				Gio::AsyncResult res) {
			success = _helper.call_create_finish(res, nullptr);
			GLib::MainContext::default_().wakeup();
		}));

		while (!success && _connected) {
			GLib::MainContext::default_().iteration(true);
		}

		if (success.value_or(false) && !_compositor) {
			_widget.reset(
				QWidget::createWindowContainer(
					QWindow::fromWinId(WId(winId())),
					config.parent,
					Qt::FramelessWindowHint));
		}
		return success.value_or(false);
	}

	_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
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
	gtk_window_set_decorated(GTK_WINDOW(_window), false);
	if (!gtk_widget_show_all) {
		gtk_widget_set_visible(_window, true);
	} else {
		gtk_widget_show_all(_window);
	}
	_webview = webkit_web_view_new();
	WebKitUserContentManager *manager =
		webkit_web_view_get_user_content_manager(WEBKIT_WEB_VIEW(_webview));
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
				GLib::Error(error));
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
	webkit_user_content_manager_register_script_message_handler(
		manager,
		"external",
		nullptr);
	const GdkRGBA rgba{ 0.f, 0.f, 0.f, 0.f, };
	webkit_web_view_set_background_color(WEBKIT_WEB_VIEW(_webview), &rgba);
	init(R"(
window.external = {
	invoke: function(s) {
		window.webkit.messageHandlers.external.postMessage(s);
	}
};)");

	return true;
}

void Instance::scriptMessageReceived(void *message) {
	auto result = std::string();
	if (!webkit_javascript_result_get_js_value && jsc_value_to_string) {
		const auto s = jsc_value_to_string(
			reinterpret_cast<JSCValue*>(message));
		result = s;
		g_free(s);
	} else if (webkit_javascript_result_get_js_value && jsc_value_to_string) {
		const auto s = jsc_value_to_string(
			webkit_javascript_result_get_js_value(
				reinterpret_cast<WebKitJavascriptResult*>(message)));
		result = s;
		g_free(s);
	} else {
		auto jsResult = reinterpret_cast<WebKitJavascriptResult*>(message);
		JSGlobalContextRef ctx
			= webkit_javascript_result_get_global_context(jsResult);
		JSValueRef value = webkit_javascript_result_get_value(jsResult);
		JSStringRef js = JSValueToStringCopy(ctx, value, NULL);
		size_t n = JSStringGetMaximumUTF8CStringSize(js);
		result.resize(n, char(0));
		JSStringGetUTF8CString(js, result.data(), n);
		JSStringRelease(js);
	}
	if (_master) {
		_master.call_message_received(result, nullptr);
	}
}

bool Instance::loadFailed(
		WebKitLoadEvent loadEvent,
		std::string failingUri,
		GLib::Error error) {
	_loadFailed = true;
	return false;
}

void Instance::loadChanged(WebKitLoadEvent loadEvent) {
	if (loadEvent == WEBKIT_LOAD_FINISHED) {
		const auto success = !_loadFailed;
		_loadFailed = false;
		if (_master) {
			_master.call_navigation_done(success, nullptr);
		}
	}
}

bool Instance::decidePolicy(
		WebKitPolicyDecision *decision,
		WebKitPolicyDecisionType decisionType) {
	if (decisionType != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
		return false;
	}
	WebKitURIRequest *request = nullptr;
	WebKitNavigationPolicyDecision *navigationDecision
		= WEBKIT_NAVIGATION_POLICY_DECISION(decision);
	if (webkit_navigation_policy_decision_get_navigation_action
		&& webkit_navigation_action_get_request) {
		WebKitNavigationAction *action
			= webkit_navigation_policy_decision_get_navigation_action(
				navigationDecision);
		request = webkit_navigation_action_get_request(action);
	} else {
		request = webkit_navigation_policy_decision_get_request(
			navigationDecision);
	}
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

ResolveResult Instance::resolve() {
	if (_remoting) {
		if (!_helper) {
			return ResolveResult::OtherError;
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

		if (!_wayland && result && *result == ResolveResult::CantInit) {
			_wayland = true;
			stopProcess();
			startProcess();
			return resolve();
		}
		return result.value_or(ResolveResult::OtherError);
	}

	return Resolve(_wayland);
}

bool Instance::finishEmbedding() {
	if (_remoting) {
		if (!_helper) {
			return false;
		}

		const ::base::has_weak_ptr guard;
		std::optional<bool> success;
		_helper.call_finish_embedding(crl::guard(&guard, [&](
				GObject::Object source_object,
				Gio::AsyncResult res) {
			success = _helper.call_finish_embedding_finish(res, nullptr);
			GLib::MainContext::default_().wakeup();
		}));

		while (!success && _connected) {
			GLib::MainContext::default_().iteration(true);
		}

		if (success.value_or(false) && _widget) {
			_widget->show();
		}
		return success.value_or(false);
	}

	if (gtk_window_set_child) {
		gtk_window_set_child(GTK_WINDOW(_window), GTK_WIDGET(_webview));
	} else {
		gtk_container_add(GTK_CONTAINER(_window), GTK_WIDGET(_webview));
	}

	if (_debug) {
		WebKitSettings *settings = webkit_web_view_get_settings(
			WEBKIT_WEB_VIEW(_webview));
		//webkit_settings_set_javascript_can_access_clipboard(settings, true);
		webkit_settings_set_enable_developer_extras(settings, true);
	}
	gtk_widget_set_visible(_window, false);
	if (!gtk_widget_show_all) {
		gtk_widget_set_visible(_window, true);
	} else {
		gtk_widget_show_all(_window);
	}
	gtk_widget_grab_focus(GTK_WIDGET(_webview));

	return true;
}

void Instance::navigate(std::string url) {
	if (_remoting) {
		if (!_helper) {
			return;
		}

		_helper.call_navigate(url, nullptr);
		return;
	}

	webkit_web_view_load_uri(WEBKIT_WEB_VIEW(_webview), url.c_str());
}

void Instance::navigateToData(std::string id) {
	Unexpected("WebKitGTK::Instance::navigateToData.");
}

void Instance::reload() {
	if (_remoting) {
		if (!_helper) {
			return;
		}

		_helper.call_reload(nullptr);
		return;
	}

	webkit_web_view_reload_bypass_cache(WEBKIT_WEB_VIEW(_webview));
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
		= webkit_web_view_get_user_content_manager(
			WEBKIT_WEB_VIEW(_webview));
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
			WEBKIT_WEB_VIEW(_webview),
			js.c_str(),
			-1,
			nullptr,
			nullptr,
			nullptr,
			nullptr,
			nullptr);
	} else {
		webkit_web_view_run_javascript(
			WEBKIT_WEB_VIEW(_webview),
			js.c_str(),
			nullptr,
			nullptr,
			nullptr);
	}
}

void Instance::focus() {
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

	if (_wayland) {
		return nullptr;
	}

	if (gdk_x11_surface_get_xid
		&& gtk_widget_get_native
		&& gtk_native_get_surface) {
		return reinterpret_cast<void*>(gdk_x11_surface_get_xid(
			gtk_native_get_surface(
				gtk_widget_get_native(_window))));
	} else {
		return reinterpret_cast<void*>(gdk_x11_window_get_xid(
			gtk_widget_get_window(_window)));
	}
}

void Instance::setOpaqueBg(QColor opaqueBg) {
	if (_remoting) {
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

	const auto background = std::regex_replace(
		".webviewWindow {background: %1;}",
		std::regex("%1"),
		_wayland ? "transparent" : opaqueBg.name().toStdString());

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

void Instance::resizeToWindow() {
	if (_remoting) {
		if (!_helper) {
			return;
		}

		_helper.call_resize_to_window(nullptr);
		return;
	}
}

void Instance::startProcess() {
	auto loop = GLib::MainLoop::new_();

	_serviceProcess = Gio::Subprocess::new_({
		::base::Integration::Instance().executablePath().toStdString(),
		std::string("-webviewhelper"),
		SocketPath,
	}, Gio::SubprocessFlags::NONE_, nullptr);

	if (!_serviceProcess) {
		return;
	}

	const auto socketPath = std::regex_replace(
		SocketPath,
		std::regex("%1"),
		std::string(_serviceProcess.get_identifier()));

	if (socketPath.empty()) {
		return;
	}

	if (_wayland && !_compositor) {
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

	_dbusServer = Gio::DBusServer::new_sync(
		SocketPathToDBusAddress(socketPath),
		Gio::DBusServerFlags::NONE_,
		Gio::dbus_generate_guid(),
		authObserver,
		{},
		nullptr);

	if (!_dbusServer) {
		return;
	}

	_dbusServer.start();
	const ::base::has_weak_ptr guard;
	auto started = ulong();
	const auto newConnection = _dbusServer.signal_new_connection().connect([&](
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
			Gio::DBusProxyFlags::DO_NOT_AUTO_START_AT_CONSTRUCTION_,
			kHelperObjectPath,
			crl::guard(&guard, [&](
					GObject::Object source_object,
					Gio::AsyncResult res) {
				_helper = HelperProxy::new_finish(res, nullptr);
				if (!_helper) {
					loop.quit();
					return;
				}

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
	const auto timeout = GLib::timeout_add_seconds_once(5, [&] {
		loop.quit();
	});

	loop.run();
	GLib::Source::remove(timeout);
	if (_helper && started) {
		_helper.disconnect(started);
	}
	_dbusServer.disconnect(newConnection);
}

void Instance::stopProcess() {
	if (_serviceProcess) {
		_serviceProcess.send_signal(SIGTERM);
	}
	if (_compositor) {
		_compositor->deleteLater();
	}
}

void Instance::registerMasterMethodHandlers() {
	if (!_master) {
		return;
	}

	_master.signal_handle_get_start_data().connect([=](
			Master,
			Gio::DBusMethodInvocation invocation) {
		_master.complete_get_start_data(invocation, [] {
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
		}(), _compositor ? _compositor->socketName().toStdString() : "");
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
		if (!_navigationStartHandler) {
			invocation.return_gerror(MethodError());
			return true;
		}

		_master.complete_navigation_started(invocation, [&] {
			if (newWindow) {
				if (_navigationStartHandler(uri, true)) {
					QDesktopServices::openUrl(QString::fromStdString(uri));
				}
				return false;
			}
			return _navigationStartHandler(uri, false);
		}());

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
}

int Instance::exec() {
	auto app = Gio::Application::new_(
		Gio::ApplicationFlags::NON_UNIQUE_);

	app.signal_startup().connect([=](Gio::Application) {
		_helper.emit_started();
	});

	app.hold();

	auto loop = GLib::MainLoop::new_();

	const auto socketPath = std::regex_replace(
		SocketPath,
		std::regex("%1"),
		std::to_string(getpid()));

	if (socketPath.empty()) {
		return 1;
	}

	auto connection = Gio::DBusConnection::new_for_address_sync(
		SocketPathToDBusAddress(socketPath),
		Gio::DBusConnectionFlags::AUTHENTICATION_CLIENT_,
		nullptr);

	if (!connection) {
		return 1;
	}

	_helper = HelperSkeleton::new_();
	auto object = ObjectSkeleton::new_(kHelperObjectPath);
	object.set_helper(_helper);
	_dbusObjectManager = Gio::DBusObjectManagerServer::new_(kObjectPath);
	_dbusObjectManager.export_(object);
	_dbusObjectManager.set_connection(connection);
	registerHelperMethodHandlers();

	MasterProxy::new_(
		connection,
		Gio::DBusProxyFlags::DO_NOT_AUTO_START_AT_CONSTRUCTION_,
		kMasterObjectPath,
		[&](GObject::Object source_object, Gio::AsyncResult res) {
			_master = MasterProxy::new_finish(res, nullptr);
			if (!_master) {
				std::abort();
			}
			_master.call_get_start_data([&](
					GObject::Object source_object,
					Gio::AsyncResult res) {
				const auto settings = _master.call_get_start_data_finish(res);
				if (settings) {
					if (const auto appId = std::get<1>(*settings)
							; !appId.empty()) {
						app.set_application_id(appId);
					}
					if (const auto waylandDisplay = std::get<2>(*settings)
							; !waylandDisplay.empty()) {
						GLib::setenv(
							"WAYLAND_DISPLAY",
							waylandDisplay,
							true);
						_wayland = true;
					}
				}
				loop.quit();
			});
		});

	connection.signal_closed().connect([&](
			Gio::DBusConnection,
			bool remotePeerVanished,
			GLib::Error_Ref error) {
		app.quit();
	});

	loop.run();

	if (_wayland) {
		// https://bugreports.qt.io/browse/QTBUG-115063
		GLib::setenv("__EGL_VENDOR_LIBRARY_FILENAMES", "", true);
		GLib::setenv("LIBGL_ALWAYS_SOFTWARE", "1", true);
		GLib::setenv("GSK_RENDERER", "cairo", true);
		GLib::setenv("GDK_DEBUG", "gl-disable", true);
		GLib::setenv("GDK_GL", "disable", true);
	}

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
			int a) {
		if (create({ .opaqueBg = QColor(r, g, b, a), .debug = debug })) {
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

	_helper.signal_handle_finish_embedding().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation) {
		if (finishEmbedding()) {
			_helper.complete_finish_embedding(invocation);
		} else {
			invocation.return_gerror(MethodError());
		}
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

	_helper.signal_handle_resize_to_window().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation) {
		resizeToWindow();
		_helper.complete_resize_to_window(invocation);
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
	if (Instance().resolve() == ResolveResult::NoLibrary) {
		return Available{
			.error = Available::Error::NoWebKitGTK,
			.details = "Please install WebKitGTK "
			"(webkitgtk-6.0/webkit2gtk-4.1/webkit2gtk-4.0) "
			"from your package manager.",
		};
	}
	return Available{};
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
