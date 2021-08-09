// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/linux/webview_linux_webkit2gtk.h"

#ifdef DESKTOP_APP_DISABLE_DBUS_INTEGRATION
#error "WebKitGTK support depends on D-Bus integration."
#endif // DESKTOP_APP_DISABLE_DBUS_INTEGRATION

#include "webview/platform/linux/webview_linux_webkit_gtk.h"
#include "base/platform/linux/base_linux_glibmm_helper.h"
#include "base/platform/linux/base_linux_dbus_utilities.h"
#include "base/platform/base_platform_info.h"
#include "base/const_string.h"
#include "base/integration.h"

#include <giomm.h>

namespace Webview::WebKit2Gtk {
namespace {

using namespace WebkitGtk;

constexpr auto kObjectPath = "/org/desktop_app/GtkIntegration/WebviewHelper"_cs;
constexpr auto kInterface = "org.desktop_app.GtkIntegration.WebviewHelper"_cs;
constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties"_cs;

constexpr auto kIntrospectionXML = R"INTROSPECTION(<node>
	<interface name='org.desktop_app.GtkIntegration.WebviewHelper'>
		<method name='Resolve'/>
		<method name='FinishEmbedding'/>
		<method name='Navigate'>
			<arg type='s' name='url' direction='in'/>
		</method>
		<method name='ResizeToWindow'/>
		<method name='Init'>
			<arg type='ay' name='js' direction='in'/>
		</method>
		<method name='Eval'>
			<arg type='ay' name='js' direction='in'/>
		</method>
		<signal name='MessageReceived'>
			<arg type='ay' name='message' direction='out'/>
		</signal>
		<signal name='NavigationStarted'>
			<arg type='s' name='uri' direction='out'/>
		</signal>
		<signal name='NavigationDone'>
			<arg type='b' name='success' direction='out'/>
		</signal>
		<property name='WinId' type='t' access='read'/>
	</interface>
</node>)INTROSPECTION"_cs;

Glib::ustring ServiceName;
std::atomic<uint> ServiceCounter = 0;
bool Remoting = true;

class Instance final : public Interface {
public:
	Instance(Config config);
	~Instance();

	int exec(const std::string &parentDBusName);

	bool resolve();

	bool finishEmbedding() override;

	void navigate(std::string url) override;

	void resizeToWindow() override;

	void init(std::string js) override;
	void eval(std::string js) override;

	void *winId() override;

private:
	void initGtk();

	void scriptMessageReceived(WebKitJavascriptResult *result);

	bool loadFailed(
		WebKitLoadEvent loadEvent,
		std::string failingUri,
		Glib::Error error);

	void loadChanged(WebKitLoadEvent loadEvent);

	bool decidePolicy(
		WebKitPolicyDecision *decision,
		WebKitPolicyDecisionType decisionType);

	void connectToRemoteSignals();
	void runProcess();

	void handleMethodCall(
		const Glib::RefPtr<Gio::DBus::Connection> &connection,
		const Glib::ustring &sender,
		const Glib::ustring &object_path,
		const Glib::ustring &interface_name,
		const Glib::ustring &method_name,
		const Glib::VariantContainerBase &parameters,
		const Glib::RefPtr<Gio::DBus::MethodInvocation> &invocation);

	void handleGetProperty(
		Glib::VariantBase &property,
		const Glib::RefPtr<Gio::DBus::Connection> &connection,
		const Glib::ustring &sender,
		const Glib::ustring &object_path,
		const Glib::ustring &interface_name,
		const Glib::ustring &property_name);

	const Glib::RefPtr<Gio::DBus::Connection> _dbusConnection;
	const Gio::DBus::InterfaceVTable _interfaceVTable;
	Glib::RefPtr<Gio::DBus::NodeInfo> _introspectionData;
	const Glib::ustring _serviceName;
	Glib::ustring _parentDBusName;
	int _servicePid = 0;
	uint _registerId = 0;
	uint _serviceWatcherId = 0;
	uint _parentServiceWatcherId = 0;
	uint _messageHandlerId = 0;
	uint _navigationStartHandlerId = 0;
	uint _navigationDoneHandlerId = 0;

	GtkWidget *_window = nullptr;
	GtkWidget *_webview = nullptr;
	std::function<void(std::string)> _messageHandler;
	std::function<bool(std::string)> _navigationStartHandler;
	std::function<void(bool)> _navigationDoneHandler;
	bool _loadFailed = false;

};

Instance::Instance(Config config)
: _dbusConnection([] {
	try {
		return Gio::DBus::Connection::get_sync(
			Gio::DBus::BusType::BUS_TYPE_SESSION);
	} catch (...) {
		return Glib::RefPtr<Gio::DBus::Connection>();
	}
}())
, _interfaceVTable(
	sigc::mem_fun(this, &Instance::handleMethodCall),
	sigc::mem_fun(this, &Instance::handleGetProperty))
, _serviceName(Remoting ? [] {
	try {
		return Glib::Regex::create("%1")->replace(
			ServiceName,
			0,
			std::to_string(ServiceCounter++),
			static_cast<Glib::RegexMatchFlags>(0));
	} catch (...) {
		return Glib::ustring();
	}
}() : ServiceName)
, _messageHandler(std::move(config.messageHandler))
, _navigationStartHandler(std::move(config.navigationStartHandler))
, _navigationDoneHandler(std::move(config.navigationDoneHandler)) {
	if (Remoting) {
		connectToRemoteSignals();
		runProcess();
	} else if (Resolve()) {
		initGtk();
	}
}

Instance::~Instance() {
	if (_servicePid != 0) {
		kill(_servicePid, SIGTERM);
	}
	if (_dbusConnection) {
		if (_navigationDoneHandlerId != 0) {
			_dbusConnection->signal_unsubscribe(
				_navigationDoneHandlerId);
		}
		if (_navigationStartHandlerId != 0) {
			_dbusConnection->signal_unsubscribe(
				_navigationStartHandlerId);
		}
		if (_messageHandlerId != 0) {
			_dbusConnection->signal_unsubscribe(
				_messageHandlerId);
		}
		if (_parentServiceWatcherId != 0) {
			_dbusConnection->signal_unsubscribe(
				_parentServiceWatcherId);
		}
		if (_serviceWatcherId != 0) {
			_dbusConnection->signal_unsubscribe(
				_serviceWatcherId);
		}
		if (_registerId != 0) {
			_dbusConnection->unregister_object(
				_registerId);
		}
	}
	if (_webview) {
		gtk_widget_destroy(_webview);
	}
	if (_window) {
		gtk_widget_destroy(_window);
	}
}

void Instance::initGtk() {
	_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_decorated(GTK_WINDOW(_window), false);
	gtk_widget_show_all(_window);
	_webview = webkit_web_view_new();
	WebKitUserContentManager *manager =
		webkit_web_view_get_user_content_manager(WEBKIT_WEB_VIEW(_webview));
	g_signal_connect_swapped(
		manager,
		"script-message-received::external",
		G_CALLBACK(+[](
			Instance *instance,
			WebKitJavascriptResult *result) {
			instance->scriptMessageReceived(result);
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
				Glib::Error(error));
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
			return instance->decidePolicy(
				decision,
				decisionType);
		}),
		this);
	webkit_user_content_manager_register_script_message_handler(
		manager,
		"external");
	init(R"(
window.external = {
	invoke: function(s) {
		window.webkit.messageHandlers.external.postMessage(s);
	}
};)");
}

void Instance::scriptMessageReceived(WebKitJavascriptResult *result) {
	auto message = std::string();
	if (webkit_javascript_result_get_js_value && jsc_value_to_string) {
		JSCValue *value = webkit_javascript_result_get_js_value(result);
		const auto s = jsc_value_to_string(value);
		message = s;
		g_free(s);
	} else {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
		JSGlobalContextRef ctx
			= webkit_javascript_result_get_global_context(result);
		JSValueRef value = webkit_javascript_result_get_value(result);
#pragma GCC diagnostic pop
		JSStringRef js = JSValueToStringCopy(ctx, value, NULL);
		size_t n = JSStringGetMaximumUTF8CStringSize(js);
		message.resize(n, char(0));
		JSStringGetUTF8CString(js, message.data(), n);
		JSStringRelease(js);
	}
	if (!_dbusConnection) {
		return;
	}
	try {
		_dbusConnection->emit_signal(
			std::string(kObjectPath),
			std::string(kInterface),
			"MessageReceived",
			_parentDBusName,
			base::Platform::MakeGlibVariant(std::tuple{
				message,
			}));
	} catch (...) {
	}
}

bool Instance::loadFailed(
		WebKitLoadEvent loadEvent,
		std::string failingUri,
		Glib::Error error) {
	_loadFailed = true;
	return false;
}

void Instance::loadChanged(WebKitLoadEvent loadEvent) {
	if (loadEvent == WEBKIT_LOAD_FINISHED) {
		const auto success = !_loadFailed;
		_loadFailed = false;
		if (!_dbusConnection) {
			return;
		}
		try {
			_dbusConnection->emit_signal(
				std::string(kObjectPath),
				std::string(kInterface),
				"NavigationDone",
				_parentDBusName,
				base::Platform::MakeGlibVariant(std::tuple{
					success,
				}));
		} catch (...) {
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
	if (_dbusConnection) {
		try {
			const auto context = Glib::MainContext::create();
			const auto loop = Glib::MainLoop::create(context);
			g_main_context_push_thread_default(context->gobj());
			const auto contextGuard = gsl::finally([&] {
				g_main_context_pop_thread_default(context->gobj());
			});
			bool result = false;

			const auto resultId = _dbusConnection->signal_subscribe(
				[&](
					const Glib::RefPtr<Gio::DBus::Connection> &connection,
					const Glib::ustring &sender_name,
					const Glib::ustring &object_path,
					const Glib::ustring &interface_name,
					const Glib::ustring &signal_name,
					Glib::VariantContainerBase parameters) {
					try {
						auto parametersCopy = parameters;

						result = base::Platform::GlibVariantCast<
							bool>(parametersCopy.get_child(0));
					} catch (...) {
					}

					loop->quit();
				},
				_parentDBusName,
				std::string(kInterface),
				"NavigationStartedResult",
				std::string(kObjectPath));

			const auto resultGuard = gsl::finally([&] {
				if (resultId != 0) {
					_dbusConnection->signal_unsubscribe(resultId);
				}
			});

			_dbusConnection->emit_signal(
				std::string(kObjectPath),
				std::string(kInterface),
				"NavigationStarted",
				_parentDBusName,
				base::Platform::MakeGlibVariant(std::tuple{
					Glib::ustring(uri),
				}));

			if (resultId != 0) {
				loop->run();
			}

			return !result;
		} catch (...) {
		}
	}
	webkit_policy_decision_ignore(decision);
	return true;
}

int Instance::exec(const std::string &parentDBusName) {
	_parentDBusName = parentDBusName;

	_introspectionData = Gio::DBus::NodeInfo::create_for_xml(
		std::string(kIntrospectionXML));

	_registerId = _dbusConnection->register_object(
		std::string(kObjectPath),
		_introspectionData->lookup_interface(),
		_interfaceVTable);

	const auto app = Gio::Application::create(_serviceName);
	app->hold();
	_parentServiceWatcherId = base::Platform::DBus::RegisterServiceWatcher(
		_dbusConnection,
		parentDBusName,
		[=](
			const Glib::ustring &service,
			const Glib::ustring &oldOwner,
			const Glib::ustring &newOwner) {
			if (!newOwner.empty()) {
				return;
			}
			app->quit();
		});
	return app->run(0, nullptr);
}

bool Instance::resolve() {
	if (Remoting) {
		if (!_dbusConnection) {
			return false;
		}

		try {
			auto reply = _dbusConnection->call_sync(
				std::string(kObjectPath),
				std::string(kInterface),
				"Resolve",
				{},
				_serviceName);

			return true;
		} catch (...) {
		}
	}

	return false;
}

bool Instance::finishEmbedding() {
	if (Remoting) {
		if (!_dbusConnection) {
			return false;
		}

		try {
			auto reply = _dbusConnection->call_sync(
				std::string(kObjectPath),
				std::string(kInterface),
				"FinishEmbedding",
				{},
				_serviceName);

			return true;
		} catch (...) {
		}

		return false;
	}

	gtk_container_add(GTK_CONTAINER(_window), GTK_WIDGET(_webview));

	// WebKitSettings *settings = webkit_web_view_get_settings(
	// 	WEBKIT_WEB_VIEW(_webview));
	//webkit_settings_set_javascript_can_access_clipboard(settings, true);

	gtk_widget_hide(_window);
	gtk_widget_show_all(_window);
	gtk_widget_grab_focus(GTK_WIDGET(_webview));

	return true;
}

void Instance::navigate(std::string url) {
	if (Remoting) {
		if (!_dbusConnection) {
			return;
		}

		try {
			auto reply = _dbusConnection->call_sync(
				std::string(kObjectPath),
				std::string(kInterface),
				"Navigate",
				base::Platform::MakeGlibVariant(std::tuple{
					Glib::ustring(url),
				}),
				_serviceName);
		} catch (...) {
		}

		return;
	}

	webkit_web_view_load_uri(WEBKIT_WEB_VIEW(_webview), url.c_str());
}

void Instance::init(std::string js) {
	if (Remoting) {
		if (!_dbusConnection) {
			return;
		}

		try {
			auto reply = _dbusConnection->call_sync(
				std::string(kObjectPath),
				std::string(kInterface),
				"Init",
				base::Platform::MakeGlibVariant(std::tuple{
					js,
				}),
				_serviceName);
		} catch (...) {
		}

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
	if (Remoting) {
		if (!_dbusConnection) {
			return;
		}

		try {
			auto reply = _dbusConnection->call_sync(
				std::string(kObjectPath),
				std::string(kInterface),
				"Eval",
				base::Platform::MakeGlibVariant(std::tuple{
					js,
				}),
				_serviceName);
		} catch (...) {
		}

		return;
	}

	webkit_web_view_run_javascript(
		WEBKIT_WEB_VIEW(_webview),
		js.c_str(),
		nullptr,
		nullptr,
		nullptr);
}

void *Instance::winId() {
	if (Remoting) {
		if (!_dbusConnection) {
			return nullptr;
		}

		try {
			auto reply = _dbusConnection->call_sync(
				std::string(kObjectPath),
				std::string(kPropertiesInterface),
				"Get",
				base::Platform::MakeGlibVariant(std::tuple{
					Glib::ustring(std::string(kInterface)),
					Glib::ustring("WinId"),
				}),
				_serviceName);

			return reinterpret_cast<void*>(
				base::Platform::GlibVariantCast<guint64>(
					base::Platform::GlibVariantCast<Glib::VariantBase>(
						reply.get_child(0))));
		} catch (...) {
		}

		return nullptr;
	}

	const auto window = gtk_widget_get_window(_window);
	const auto result = window
		? reinterpret_cast<void*>(gdk_x11_window_get_xid(window))
		: nullptr;
	return result;
}

void Instance::resizeToWindow() {
	if (Remoting) {
		if (!_dbusConnection) {
			return;
		}

		try {
			auto reply = _dbusConnection->call_sync(
				std::string(kObjectPath),
				std::string(kInterface),
				"ResizeToWindow",
				{},
				_serviceName);
		} catch (...) {
		}

		return;
	}
}

void Instance::connectToRemoteSignals() {
	if (!_dbusConnection) {
		return;
	}

	_serviceWatcherId = base::Platform::DBus::RegisterServiceWatcher(
		_dbusConnection,
		_serviceName,
		[=](
			const Glib::ustring &service,
			const Glib::ustring &oldOwner,
			const Glib::ustring &newOwner) {
			if (!newOwner.empty()) {
				return;
			}
			runProcess();
		});

	_messageHandlerId = _dbusConnection->signal_subscribe(
		[=](
			const Glib::RefPtr<Gio::DBus::Connection> &connection,
			const Glib::ustring &sender_name,
			const Glib::ustring &object_path,
			const Glib::ustring &interface_name,
			const Glib::ustring &signal_name,
			Glib::VariantContainerBase parameters) {
			try {
				auto parametersCopy = parameters;

				const auto message = base::Platform::GlibVariantCast<
					std::string>(parametersCopy.get_child(0));

				_messageHandler(message);
			} catch (...) {
			}
		},
		_serviceName,
		std::string(kInterface),
		"MessageReceived",
		std::string(kObjectPath));

	if (_navigationStartHandler) {
		_navigationStartHandlerId = _dbusConnection->signal_subscribe(
			[=](
				const Glib::RefPtr<Gio::DBus::Connection> &connection,
				const Glib::ustring &sender_name,
				const Glib::ustring &object_path,
				const Glib::ustring &interface_name,
				const Glib::ustring &signal_name,
				Glib::VariantContainerBase parameters) {
				try {
					auto parametersCopy = parameters;

					const auto uri = base::Platform::GlibVariantCast<
						Glib::ustring>(parametersCopy.get_child(0));

					_dbusConnection->emit_signal(
						std::string(kObjectPath),
						std::string(kInterface),
						"NavigationStartedResult",
						_serviceName,
						base::Platform::MakeGlibVariant(std::tuple{
							_navigationStartHandler(uri),
						}));
				} catch (...) {
				}
			},
			_serviceName,
			std::string(kInterface),
			"NavigationStarted",
			std::string(kObjectPath));
	}

	if (_navigationDoneHandler) {
		_navigationDoneHandlerId = _dbusConnection->signal_subscribe(
			[=](
				const Glib::RefPtr<Gio::DBus::Connection> &connection,
				const Glib::ustring &sender_name,
				const Glib::ustring &object_path,
				const Glib::ustring &interface_name,
				const Glib::ustring &signal_name,
				Glib::VariantContainerBase parameters) {
				try {
					auto parametersCopy = parameters;

					const auto success = base::Platform::GlibVariantCast<
						bool>(parametersCopy.get_child(0));

					_navigationDoneHandler(success);
				} catch (...) {
				}
			},
			_serviceName,
			std::string(kInterface),
			"NavigationDone",
			std::string(kObjectPath));
	}
}

void Instance::runProcess() {
	if (!_dbusConnection) {
		return;
	}

	const auto context = Glib::MainContext::create();
	const auto loop = Glib::MainLoop::create(context);
	g_main_context_push_thread_default(context->gobj());
	const auto contextGuard = gsl::finally([&] {
		g_main_context_pop_thread_default(context->gobj());
	});

	const auto serviceWatcherId = base::Platform::DBus::RegisterServiceWatcher(
		_dbusConnection,
		_serviceName,
		[&](
			const Glib::ustring &service,
			const Glib::ustring &oldOwner,
			const Glib::ustring &newOwner) {
			if (newOwner.empty()) {
				return;
			}
			loop->quit();
		});

	const auto serviceWatcherGuard = gsl::finally([&] {
		if (serviceWatcherId != 0) {
			_dbusConnection->signal_unsubscribe(serviceWatcherId);
		}
	});

	if (serviceWatcherId == 0) {
		return;
	}

	// timeout in case something goes wrong
	const auto timeout = Glib::TimeoutSource::create(5000);
	timeout->connect([=] {
		if (loop->is_running()) {
			loop->quit();
		}
		return false;
	});
	timeout->attach(context);

	Glib::spawn_async(
		"",
		std::vector<std::string>{
			base::Integration::Instance().executablePath().toStdString(),
			"-webviewhelper",
			_dbusConnection->get_unique_name(),
			_serviceName,
		},
		Glib::SPAWN_DEFAULT,
		sigc::slot<void>(),
		&_servicePid);

	loop->run();
}

void Instance::handleMethodCall(
		const Glib::RefPtr<Gio::DBus::Connection> &connection,
		const Glib::ustring &sender,
		const Glib::ustring &object_path,
		const Glib::ustring &interface_name,
		const Glib::ustring &method_name,
		const Glib::VariantContainerBase &parameters,
		const Glib::RefPtr<Gio::DBus::MethodInvocation> &invocation) {
	if (sender != _parentDBusName) {
		Gio::DBus::Error error(
			Gio::DBus::Error::ACCESS_DENIED,
			"Access denied.");

		invocation->return_error(error);
		return;
	}

	try {
		auto parametersCopy = parameters;

		if (method_name == "Resolve") {
			if (Resolve()) {
				invocation->return_value({});
				return;
			}
		} else if (method_name == "FinishEmbedding") {
			if (finishEmbedding()) {
				invocation->return_value({});
				return;
			}
		} else if (method_name == "Navigate") {
			const auto url = base::Platform::GlibVariantCast<
				Glib::ustring>(parametersCopy.get_child(0));

			navigate(url);
			invocation->return_value({});
			return;
		} else if (method_name == "ResizeToWindow") {
			resizeToWindow();
			invocation->return_value({});
			return;
		} else if (method_name == "Init") {
			const auto js = base::Platform::GlibVariantCast<
				std::string>(parametersCopy.get_child(0));

			init(js);
			invocation->return_value({});
			return;
		} else if (method_name == "Eval") {
			const auto js = base::Platform::GlibVariantCast<
				std::string>(parametersCopy.get_child(0));

			eval(js);
			invocation->return_value({});
			return;
		}
	} catch (...) {
	}

	Gio::DBus::Error error(
		Gio::DBus::Error::UNKNOWN_METHOD,
		"Method does not exist.");

	invocation->return_error(error);
}

void Instance::handleGetProperty(
		Glib::VariantBase &property,
		const Glib::RefPtr<Gio::DBus::Connection> &connection,
		const Glib::ustring &sender,
		const Glib::ustring &object_path,
		const Glib::ustring &interface_name,
		const Glib::ustring &property_name) {
	if (sender != _parentDBusName) {
		throw Gio::DBus::Error(
			Gio::DBus::Error::ACCESS_DENIED,
			"Access denied.");
	}

	if (property_name == "WinId") {
		property = Glib::Variant<guint64>::create(
			reinterpret_cast<guint64>(winId()));
		return;
	}

	throw Gio::DBus::Error(
		Gio::DBus::Error::NO_REPLY,
		"No reply.");
}

bool Resolve() {
	if (Remoting) {
		static const auto result = Instance({}).resolve();
		return result;
	} else {
		return WebkitGtk::Resolve();
	}
}

} // namespace

Available Availability() {
	if (Platform::IsWayland()) {
		return Available{
			.error = Available::Error::Wayland,
			.details = "There is no way to embed WebView window "
			"on Wayland. Please switch to X11."
		};
	} else if (const auto platform = Platform::GetWindowManager().toLower()
		; platform.contains("mutter") || platform.contains("gnome")) {
		return Available{
			.error = Available::Error::MutterWM,
			.details = "Qt's window embedding doesn't work well "
			"with Mutter window manager. Please switch to another "
			"window manager or desktop environment."
		};
	} else if (!Resolve()) {
		return Available{
			.error = Available::Error::NoGtkOrWebkit2Gtk,
			.details = "Please install WebKitGTK 4 (webkit2gtk-4.0) "
			"from your package manager.",
		};
	}
	return Available{};
}

std::unique_ptr<Interface> CreateInstance(Config config) {
	if (!Supported()) {
		return nullptr;
	}
	return std::make_unique<Instance>(std::move(config));
}

int Exec(const std::string &parentDBusName) {
	Remoting = false;
	return Instance({}).exec(parentDBusName);
}

void SetServiceName(const std::string &serviceName) {
	ServiceName = serviceName;
}

} // namespace Webview::WebKit2Gtk
