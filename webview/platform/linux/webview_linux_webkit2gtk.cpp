// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/linux/webview_linux_webkit2gtk.h"

#include "webview/platform/linux/webview_linux_webkit_gtk.h"
#include "base/platform/base_platform_info.h"

namespace Webview::WebKit2Gtk {
namespace {

using namespace WebkitGtk;

class Instance final : public Interface {
public:
	Instance(Config config);

	bool finishEmbedding() override;

	void navigate(std::string url) override;

	void resizeToWindow() override;

	void init(std::string js) override;
	void eval(std::string js) override;

	void *winId() override;

private:
	static void ScriptMessageReceived(
		WebKitUserContentManager *manager,
		WebKitJavascriptResult *result,
		gpointer arg);
	void scriptMessageReceived(
		WebKitUserContentManager *manager,
		WebKitJavascriptResult *result);

	static gboolean LoadFailed(
		WebKitWebView *webView,
		WebKitLoadEvent loadEvent,
		char *failingUri,
		GError *error,
		gpointer arg);
	gboolean loadFailed(
		WebKitWebView *webView,
		WebKitLoadEvent loadEvent,
		char *failingUri,
		GError *error);

	static void LoadChanged(
		WebKitWebView *webView,
		WebKitLoadEvent loadEvent,
		gpointer arg);
	void loadChanged(
		WebKitWebView *webView,
		WebKitLoadEvent loadEvent);

	static gboolean DecidePolicy(
		WebKitWebView *webView,
		WebKitPolicyDecision *decision,
		WebKitPolicyDecisionType decisionType,
		gpointer arg);
	gboolean decidePolicy(
		WebKitWebView *webView,
		WebKitPolicyDecision *decision,
		WebKitPolicyDecisionType decisionType);

	GtkWidget *_window = nullptr;
	GtkWidget *_webview = nullptr;
	std::function<void(std::string)> _messageHandler;
	std::function<bool(std::string)> _navigationStartHandler;
	std::function<void(bool)> _navigationDoneHandler;
	bool _loadFailed = false;

};

Instance::Instance(Config config)
: _window(gtk_window_new(GTK_WINDOW_TOPLEVEL))
, _messageHandler(std::move(config.messageHandler))
, _navigationStartHandler(std::move(config.navigationStartHandler))
, _navigationDoneHandler(std::move(config.navigationDoneHandler)) {
	gtk_window_set_decorated(GTK_WINDOW(_window), false);
	gtk_widget_show_all(_window);
	_webview = webkit_web_view_new();
	WebKitUserContentManager *manager =
		webkit_web_view_get_user_content_manager(WEBKIT_WEB_VIEW(_webview));
	g_signal_connect(
		manager,
		"script-message-received::external",
		G_CALLBACK(&Instance::ScriptMessageReceived),
		this);
	g_signal_connect(
		_webview,
		"load-failed",
		G_CALLBACK(&Instance::LoadFailed),
		this);
	g_signal_connect(
		_webview,
		"load-changed",
		G_CALLBACK(&Instance::LoadChanged),
		this);
	g_signal_connect(
		_webview,
		"decide-policy",
		G_CALLBACK(&Instance::DecidePolicy),
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

void Instance::ScriptMessageReceived(
		WebKitUserContentManager *manager,
		WebKitJavascriptResult *result,
		gpointer arg) {
	static_cast<Instance*>(arg)->scriptMessageReceived(manager, result);
}

void Instance::scriptMessageReceived(
		WebKitUserContentManager *manager,
		WebKitJavascriptResult *result) {
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
	_messageHandler(std::move(message));
}

gboolean Instance::LoadFailed(
		WebKitWebView *webView,
		WebKitLoadEvent loadEvent,
		char *failingUri,
		GError *error,
		gpointer arg) {
	return static_cast<Instance*>(arg)->loadFailed(
		webView,
		loadEvent,
		failingUri,
		error);
}

gboolean Instance::loadFailed(
		WebKitWebView *webView,
		WebKitLoadEvent loadEvent,
		char *failingUri,
		GError *error) {
	_loadFailed = true;
	return FALSE;
}

void Instance::LoadChanged(
		WebKitWebView *webView,
		WebKitLoadEvent loadEvent,
		gpointer arg) {
	static_cast<Instance*>(arg)->loadChanged(webView, loadEvent);
}

void Instance::loadChanged(
		WebKitWebView *webView,
		WebKitLoadEvent loadEvent) {
	if (loadEvent == WEBKIT_LOAD_FINISHED) {
		const auto success = !_loadFailed;
		_loadFailed = false;
		if (_navigationDoneHandler) {
			_navigationDoneHandler(success);
		}
	}
}

gboolean Instance::DecidePolicy(
		WebKitWebView *webView,
		WebKitPolicyDecision *decision,
		WebKitPolicyDecisionType decisionType,
		gpointer arg) {
	return static_cast<Instance*>(arg)->decidePolicy(
		webView,
		decision,
		decisionType);
}

gboolean Instance::decidePolicy(
		WebKitWebView *webView,
		WebKitPolicyDecision *decision,
		WebKitPolicyDecisionType decisionType) {
	if (decisionType != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION
		|| !_navigationStartHandler) {
		return FALSE;
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
	if (_navigationStartHandler(std::string(uri))) {
		return FALSE;
	}
	webkit_policy_decision_ignore(decision);
	return TRUE;
}

bool Instance::finishEmbedding() {
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
	webkit_web_view_load_uri(WEBKIT_WEB_VIEW(_webview), url.c_str());
}

void Instance::init(std::string js) {
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
	webkit_web_view_run_javascript(
		WEBKIT_WEB_VIEW(_webview),
		js.c_str(),
		nullptr,
		nullptr,
		nullptr);
}

void *Instance::winId() {
	const auto window = gtk_widget_get_window(_window);
	const auto result = window
		? reinterpret_cast<void*>(gdk_x11_window_get_xid(window))
		: nullptr;
	return result;
}

void Instance::resizeToWindow() {
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

} // namespace Webview::WebKit2Gtk
