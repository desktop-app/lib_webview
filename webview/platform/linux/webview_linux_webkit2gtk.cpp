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
		WebKitUserContentManager *,
		WebKitJavascriptResult *r,
		gpointer arg);
	void scriptMessageReceived(WebKitJavascriptResult *result);

	GtkWidget *_window = nullptr;
	GtkWidget *_webview = nullptr;
	std::function<void(std::string)> _messageHandler;
	std::function<bool(std::string)> _navigationHandler;

};

Instance::Instance(Config config)
: _window(gtk_window_new(GTK_WINDOW_TOPLEVEL))
, _messageHandler(std::move(config.messageHandler))
, _navigationHandler(std::move(config.navigationHandler)) {
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
		WebKitUserContentManager *,
		WebKitJavascriptResult *result,
		gpointer arg) {
	static_cast<Instance*>(arg)->scriptMessageReceived(result);
}

void Instance::scriptMessageReceived(WebKitJavascriptResult *result) {
	const auto major = webkit_get_major_version();
	const auto minor = webkit_get_minor_version();

	auto message = std::string();
	if (major > 2 || (major == 2 && minor >= 22)) {
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
