// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/linux/webview_linux_webkit2gtk.h"

#include "webview/platform/linux/webview_linux_webkit_gtk.h"

namespace Webview::WebKit2Gtk {
namespace {

using namespace WebkitGtk;

class Instance final : public Interface {
public:
	Instance(Config config);

	void navigate(std::string url) override;

	void resizeToWindow() override;

	void init(std::string js) override;
	void eval(std::string js) override;

	void *winId() override;

private:
	GtkWidget *_window = nullptr;
	GtkWidget *_webview = nullptr;
	std::function<void(std::string)> _messageCallback;
	std::function<void(std::string)> _navigationCallback;

};

Instance::Instance(Config config)
: _window(static_cast<GtkWidget*>(config.window)) {
	std::cout << "Init" << std::endl;
	gtk_init_check(0, NULL);
	std::cout << "Create WebView" << std::endl;
	_webview = webkit_web_view_new();
	std::cout << "Manager.." << std::endl;
	WebKitUserContentManager *manager =
		webkit_web_view_get_user_content_manager(WEBKIT_WEB_VIEW(_webview));
	std::cout << "Signal.." << std::endl;
	g_signal_connect(
		manager,
		"script-message-received::external",
		G_CALLBACK(+[](
				WebKitUserContentManager *,
				WebKitJavascriptResult *r,
				gpointer arg) {
			auto *w = static_cast<Instance*>(arg);
#if WEBKIT_MAJOR_VERSION >= 2 && WEBKIT_MINOR_VERSION >= 22
			JSCValue *value = webkit_javascript_result_get_js_value(r);
			char *s = jsc_value_to_string(value);
#else
			JSGlobalContextRef ctx
				= webkit_javascript_result_get_global_context(r);
			JSValueRef value = webkit_javascript_result_get_value(r);
			JSStringRef js = JSValueToStringCopy(ctx, value, NULL);
			size_t n = JSStringGetMaximumUTF8CStringSize(js);
			char *s = g_new(char, n);
			JSStringGetUTF8CString(js, s, n);
			JSStringRelease(js);
#endif
			w->_messageCallback(s);
			g_free(s);
		}),
		this);
	std::cout << "Register.." << std::endl;
	webkit_user_content_manager_register_script_message_handler(
		manager,
		"external");
	std::cout << "Init.." << std::endl;
	init(R"(
window.external = {
	invoke: function(s) {
		window.webkit.messageHandlers.external.postMessage(s);
	}
};)");

	std::cout << "Add.." << std::endl;
	gtk_container_add(GTK_CONTAINER(_window), GTK_WIDGET(_webview));
	std::cout << "Focus.." << std::endl;
	gtk_widget_grab_focus(GTK_WIDGET(_webview));

	std::cout << "Settings.." << std::endl;
	WebKitSettings *settings = webkit_web_view_get_settings(
		WEBKIT_WEB_VIEW(_webview));
	std::cout << "Done.." << std::endl;
	//webkit_settings_set_javascript_can_access_clipboard(settings, true);

	//gtk_widget_show_all(_window);
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
	return nullptr;
}

void Instance::resizeToWindow() {
}

} // namespace

bool Supported() {
	static const auto resolved = Resolve();
	return resolved;
}

std::unique_ptr<Interface> CreateInstance(Config config) {
	if (!Supported()) {
		return nullptr;
	}
	std::cout << "Creating" << std::endl;
	return std::make_unique<Instance>(std::move(config));
}

} // namespace Webview::WebKit2Gtk
