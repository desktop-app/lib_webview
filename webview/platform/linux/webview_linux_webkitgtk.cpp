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
#include "base/integration.h"
#include "base/unique_qptr.h"
#include "base/weak_ptr.h"
#include "ui/gl/gl_detection.h"

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

std::string SocketPath;

inline std::string SocketPathToDBusAddress(const std::string &socketPath) {
	return "unix:path=" + socketPath;
}

class Instance final : public Interface, public ::base::has_weak_ptr {
public:
	Instance(bool remoting = true);
	~Instance();

	bool create(Config config);

	bool resolve();

	bool finishEmbedding() override;

	void navigate(std::string url) override;
	void reload() override;

	void resizeToWindow() override;

	void init(std::string js) override;
	void eval(std::string js) override;

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

	void registerMasterMethodHandlers();
	void registerHelperMethodHandlers();

	bool _remoting = false;
	Master _master;
	Helper _helper;
	Gio::DBusObjectManagerServer _dbusObjectManager;
	Gio::Subprocess _serviceProcess;

	bool _wayland = false;
	::base::unique_qptr<QQuickWidget> _compositorWidget;
	::base::unique_qptr<Compositor> _compositor;

	GtkWidget *_window = nullptr;
	GtkWidget *_webview = nullptr;

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
		if ((_wayland = ProvidesQWidget())) {
			[[maybe_unused]] static const auto Inited = [] {
				const auto backend = Ui::GL::ChooseBackendDefault(
					Ui::GL::CheckCapabilities(nullptr));
				switch (backend) {
				case Ui::GL::Backend::Raster:
					QQuickWindow::setGraphicsApi(
						QSGRendererInterface::Software);
					break;
				case Ui::GL::Backend::OpenGL:
					QQuickWindow::setGraphicsApi(
						QSGRendererInterface::OpenGL);
					break;
				}
				return true;
			}();
		}

		startProcess();
	}
}

Instance::~Instance() {
	if (_serviceProcess) {
		_serviceProcess.send_signal(SIGTERM);
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
		if (_compositor && !_compositorWidget) {
			_compositorWidget = ::base::make_unique_q<QQuickWidget>(
				reinterpret_cast<QWidget*>(config.window));

			_compositor->setWidget(_compositorWidget.get());
		}

		if (!_helper) {
			return false;
		}

		auto loop = GLib::MainLoop::new_();
		auto success = false;
		const auto r = config.opaqueBg.red();
		const auto g = config.opaqueBg.green();
		const auto b = config.opaqueBg.blue();
		const auto a = config.opaqueBg.alpha();
		_helper.call_create(_debug, r, g, b, a, [&](
				GObject::Object source_object,
				Gio::AsyncResult res) {
			success = _helper.call_create_finish(res, nullptr);
			loop.quit();
		});

		loop.run();
		return success;
	}

	if (!resolve()) {
		return false;
	}

	_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
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
	setOpaqueBg(config.opaqueBg);
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

bool Instance::resolve() {
	if (_remoting) {
		if (!_helper) {
			return false;
		}

		auto loop = GLib::MainLoop::new_();
		auto success = false;
		_helper.call_resolve([&](
				GObject::Object source_object,
				Gio::AsyncResult res) {
			success = _helper.call_resolve_finish(res, nullptr);
			loop.quit();
		});

		loop.run();
		return success;
	}

	return Resolve(_wayland);
}

bool Instance::finishEmbedding() {
	if (_remoting) {
		if (!_helper) {
			return false;
		}

		auto loop = GLib::MainLoop::new_();
		auto success = false;
		_helper.call_finish_embedding([&](
				GObject::Object source_object,
				Gio::AsyncResult res) {
			success = _helper.call_finish_embedding_finish(res, nullptr);
			loop.quit();
		});

		loop.run();
		if (success && _compositorWidget) {
			_compositorWidget->show();
		}
		return success;
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

		auto loop = GLib::MainLoop::new_();
		_helper.call_navigate(url, [&](
				GObject::Object source_object,
				Gio::AsyncResult res) {
			loop.quit();
		});

		loop.run();
		return;
	}

	webkit_web_view_load_uri(WEBKIT_WEB_VIEW(_webview), url.c_str());
}

void Instance::reload() {
	if (_remoting) {
		if (!_helper) {
			return;
		}

		auto loop = GLib::MainLoop::new_();
		_helper.call_reload([&](
				GObject::Object source_object,
				Gio::AsyncResult res) {
			loop.quit();
		});

		loop.run();
		return;
	}

	webkit_web_view_reload_bypass_cache(WEBKIT_WEB_VIEW(_webview));
}

void Instance::init(std::string js) {
	if (_remoting) {
		if (!_helper) {
			return;
		}

		auto loop = GLib::MainLoop::new_();
		_helper.call_init(js, [&](
				GObject::Object source_object,
				Gio::AsyncResult res) {
			loop.quit();
		});

		loop.run();
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

		auto loop = GLib::MainLoop::new_();
		_helper.call_eval(js, [&](
				GObject::Object source_object,
				Gio::AsyncResult res) {
			loop.quit();
		});

		loop.run();
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

void *Instance::winId() {
	if (_remoting) {
		if (_compositorWidget) {
			return reinterpret_cast<void*>(_compositorWidget.get());
		}

		if (!_helper) {
			return nullptr;
		}

		auto loop = GLib::MainLoop::new_();
		void *ret = nullptr;
		_helper.call_get_win_id([&](
				GObject::Object source_object,
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

		auto loop = GLib::MainLoop::new_();
		_helper.call_set_opaque_bg(
			opaqueBg.red(),
			opaqueBg.green(),
			opaqueBg.blue(),
			opaqueBg.alpha(),
			[&](GObject::Object source_object, Gio::AsyncResult res) {
				loop.quit();
			});

		loop.run();
		return;
	}

	GdkRGBA rgba{
		opaqueBg.redF(),
		opaqueBg.greenF(),
		opaqueBg.blueF(),
		opaqueBg.alphaF(),
	};
	webkit_web_view_set_background_color(
		WEBKIT_WEB_VIEW(_webview),
		&rgba);
}

void Instance::resizeToWindow() {
	if (_remoting) {
		if (!_helper) {
			return;
		}

		auto loop = GLib::MainLoop::new_();
		_helper.call_resize_to_window([&](
				GObject::Object source_object,
				Gio::AsyncResult res) {
			loop.quit();
		});

		loop.run();
		return;
	}
}

void Instance::startProcess() {
	if (_wayland && !_compositor) {
		_compositor = ::base::make_unique_q<Compositor>();
	}

	auto loop = GLib::MainLoop::new_();

	auto serviceLauncher = Gio::SubprocessLauncher::new_(
		Gio::SubprocessFlags::NONE_);

	if (_compositor) {
		serviceLauncher.setenv(
			"WAYLAND_DISPLAY",
			_compositor->socketName().toStdString(),
			true);
	}

	_serviceProcess = serviceLauncher.spawnv({
		::base::Integration::Instance().executablePath().toStdString(),
		"-webviewhelper",
		SocketPath,
	}, nullptr);

	if (!_serviceProcess) {
		return;
	}

	const auto socketPath = std::regex_replace(
		SocketPath,
		std::regex("%1"),
		_serviceProcess.get_identifier().value_or(""));

	if (socketPath.empty()) {
		return;
	}

	auto socketFile = Gio::File::new_for_path(socketPath);
	socketFile.delete_(nullptr);

	auto socketMonitor = socketFile.monitor(
		Gio::FileMonitorFlags::NONE_,
		nullptr);

	if (!socketMonitor) {
		return;
	}

	socketMonitor.signal_changed().connect([&](
			Gio::FileMonitor,
			Gio::File file,
			Gio::File otherFile,
			Gio::FileMonitorEvent eventType) {
		if (eventType == Gio::FileMonitorEvent::CREATED_) {
			loop.quit();
		}
	});

	// timeout in case something goes wrong
	const auto timeout = GLib::timeout_add_seconds_once(5, [&] {
		loop.quit();
	});

	loop.run();
	GLib::Source::remove(timeout);

	auto connection = Gio::DBusConnection::new_for_address_sync(
		SocketPathToDBusAddress(socketPath),
		Gio::DBusConnectionFlags::AUTHENTICATION_CLIENT_,
		nullptr);

	if (!connection) {
		return;
	}

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
		[&](GObject::Object source_object, Gio::AsyncResult res) {
			_helper = HelperProxy::new_finish(res, nullptr);
			loop.quit();
		});

	loop.run();

	if (!_helper) {
		return;
	}

	connection.signal_closed().connect(crl::guard(this, [=](
			Gio::DBusConnection,
			bool remotePeerVanished,
			GLib::Error error) {
		startProcess();
	}));

	const auto started = _helper.signal_started().connect([&](Helper) {
		loop.quit();
	});

	loop.run();
	_helper.disconnect(started);
}

void Instance::registerMasterMethodHandlers() {
	if (!_master) {
		return;
	}

	const auto methodError = GLib::Error::new_literal(
		Gio::DBusErrorNS_::quark(),
		int(Gio::DBusError::UNKNOWN_METHOD_),
		"Method does not exist.");

	_master.signal_handle_get_start_data().connect([=](
			Master,
			Gio::DBusMethodInvocation invocation) {
		_master.complete_get_start_data(invocation, [] {
			if (auto app = Gio::Application::get_default()) {
				if (const auto appId = app.get_application_id()) {
					return *appId;
				}
			}

			const auto qtAppId = QGuiApplication::desktopFileName()
				.toStdString();

			if (Gio::Application::id_is_valid(qtAppId)) {
				return qtAppId;
			}

			return std::string();
		}(), _wayland);
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
			invocation.return_gerror(methodError);
		}
		return true;
	});

	_master.signal_handle_navigation_started().connect([=](
			Master,
			Gio::DBusMethodInvocation invocation,
			const std::string &uri,
			bool newWindow) {
		if (!_navigationStartHandler) {
			invocation.return_gerror(methodError);
			return true;
		}

		_master.complete_navigation_started(invocation, [&] {
			if (newWindow) {
				if (_navigationStartHandler(uri, true)) {
					Gio::AppInfo::launch_default_for_uri(uri);
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
			invocation.return_gerror(methodError);
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
			invocation.return_gerror(methodError);
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

	auto authObserver = Gio::DBusAuthObserver::new_();
	authObserver.signal_authorize_authenticated_peer().connect([](
			Gio::DBusAuthObserver,
			Gio::IOStream stream,
			Gio::Credentials credentials) {
		return credentials.get_unix_pid(nullptr) == getppid();
	});

	auto dbusServer = Gio::DBusServer::new_sync(
		SocketPathToDBusAddress(socketPath),
		Gio::DBusServerFlags::NONE_,
		Gio::dbus_generate_guid(),
		authObserver,
		{},
		nullptr);

	if (!dbusServer) {
		return 1;
	}

	dbusServer.start();
	const auto newConnection = dbusServer.signal_new_connection().connect([&](
			Gio::DBusServer,
			Gio::DBusConnection connection) {
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
					const auto settings = _master.call_get_start_data_finish(
						res);
					if (settings) {
						app.set_application_id(std::get<1>(*settings));
						_wayland = std::get<2>(*settings);
					}
					loop.quit();
				});
			});

		connection.signal_closed().connect([&](
				Gio::DBusConnection,
				bool remotePeerVanished,
				GLib::Error error) {
			app.quit();
		});

		return true;
	});

	loop.run();
	dbusServer.disconnect(newConnection);

	return app.run(0, nullptr);
}

void Instance::registerHelperMethodHandlers() {
	if (!_helper) {
		return;
	}

	const auto methodError = GLib::Error::new_literal(
		Gio::DBusErrorNS_::quark(),
		int(Gio::DBusError::UNKNOWN_METHOD_),
		"Method does not exist.");

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
			invocation.return_gerror(methodError);
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
		if (resolve()) {
			_helper.complete_resolve(invocation);
		} else {
			invocation.return_gerror(methodError);
		}
		return true;
	});

	_helper.signal_handle_finish_embedding().connect([=](
			Helper,
			Gio::DBusMethodInvocation invocation) {
		if (finishEmbedding()) {
			_helper.complete_finish_embedding(invocation);
		} else {
			invocation.return_gerror(methodError);
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
	if (!Instance().resolve()) {
		return Available{
			.error = Available::Error::NoWebKitGTK,
			.details = "Please install WebKitGTK "
			"(webkitgtk-6.0/webkit2gtk-4.1/webkit2gtk-4.0) "
			"from your package manager.",
		};
	}
	return Available{};
}

bool ProvidesQWidget() {
	if (!Platform::IsX11()) {
		return true;
	}
	const auto platform = Platform::GetWindowManager().toLower();
	return platform.contains("mutter") || platform.contains("gnome");
}

std::unique_ptr<Interface> CreateInstance(Config config) {
	if (!Supported()) {
		return nullptr;
	}
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
