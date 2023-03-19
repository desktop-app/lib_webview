// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/linux/webview_linux_webkitgtk_library.h"

#include <dlfcn.h>
#include <memory>

#define LOAD_SYMBOL(handle, func) LoadSymbol(handle, #func, func)

namespace Webview::WebKitGTK::Library {
namespace {

struct HandleDeleter {
	void operator()(void *handle) {
		dlclose(handle);
	}
};

using Handle = std::unique_ptr<void, HandleDeleter>;

bool LoadLibrary(Handle &handle, const char *name) {
	handle = Handle(dlopen(name, RTLD_LAZY | RTLD_NODELETE));
	if (handle) {
		return true;
	}
	return false;
}

template <typename Function>
inline bool LoadSymbol(const Handle &handle, const char *name, Function &func) {
	func = handle
		? reinterpret_cast<Function>(dlsym(handle.get(), name))
		: nullptr;
	return (func != nullptr);
}

} // namespace

bool Resolve(bool wayland) {
	auto lib = Handle();
	const auto result = (LoadLibrary(lib, "libwebkitgtk-6.0.so.4")
			|| LoadLibrary(lib, "libwebkit2gtk-4.1.so.0")
			|| LoadLibrary(lib, "libwebkit2gtk-4.0.so.37"))
		&& LOAD_SYMBOL(lib, gtk_init_check)
		&& LOAD_SYMBOL(lib, gtk_widget_get_type)
		&& LOAD_SYMBOL(lib, gtk_widget_grab_focus)
		&& (LOAD_SYMBOL(lib, gtk_window_set_child)
			|| (LOAD_SYMBOL(lib, gtk_container_get_type)
				&& LOAD_SYMBOL(lib, gtk_container_add)))
		&& (wayland
			|| (LOAD_SYMBOL(lib, gtk_widget_get_native)
				&& LOAD_SYMBOL(lib, gtk_native_get_surface))
			|| LOAD_SYMBOL(lib, gtk_widget_get_window))
		&& LOAD_SYMBOL(lib, gtk_window_new)
		&& (LOAD_SYMBOL(lib, gtk_window_destroy)
			|| LOAD_SYMBOL(lib, gtk_widget_destroy))
		&& LOAD_SYMBOL(lib, gtk_widget_set_visible)
		&& LOAD_SYMBOL(lib, gtk_window_get_type)
		&& LOAD_SYMBOL(lib, gtk_window_set_decorated)
		&& (wayland
			|| LOAD_SYMBOL(lib, gdk_x11_surface_get_xid)
			|| LOAD_SYMBOL(lib, gdk_x11_window_get_xid))
		&& LOAD_SYMBOL(lib, webkit_web_view_new)
		&& LOAD_SYMBOL(lib, webkit_web_view_get_type)
		&& LOAD_SYMBOL(lib, webkit_web_view_get_user_content_manager)
		&& LOAD_SYMBOL(lib, webkit_user_content_manager_register_script_message_handler)
		&& LOAD_SYMBOL(lib, webkit_web_view_get_settings)
		&& LOAD_SYMBOL(lib, webkit_settings_set_javascript_can_access_clipboard)
		&& LOAD_SYMBOL(lib, webkit_settings_set_enable_developer_extras)
		&& LOAD_SYMBOL(lib, webkit_web_view_load_uri)
		&& LOAD_SYMBOL(lib, webkit_web_view_reload_bypass_cache)
		&& LOAD_SYMBOL(lib, webkit_user_script_new)
		&& LOAD_SYMBOL(lib, webkit_user_content_manager_add_script)
		&& (LOAD_SYMBOL(lib, webkit_web_view_evaluate_javascript)
			|| LOAD_SYMBOL(lib, webkit_web_view_run_javascript))
		&& LOAD_SYMBOL(lib, webkit_uri_request_get_uri)
		&& LOAD_SYMBOL(lib, webkit_policy_decision_ignore)
		&& LOAD_SYMBOL(lib, webkit_navigation_policy_decision_get_type)
		&& LOAD_SYMBOL(lib, webkit_script_dialog_get_dialog_type)
		&& LOAD_SYMBOL(lib, webkit_script_dialog_get_message)
		&& LOAD_SYMBOL(lib, webkit_script_dialog_confirm_set_confirmed)
		&& LOAD_SYMBOL(lib, webkit_script_dialog_prompt_get_default_text)
		&& LOAD_SYMBOL(lib, webkit_script_dialog_prompt_set_text);
	if (!result) {
		return false;
	}
	LOAD_SYMBOL(lib, gtk_widget_show_all);
	LOAD_SYMBOL(lib, webkit_javascript_result_get_js_value);
	{
		const auto available1 = LOAD_SYMBOL(lib, jsc_value_to_string);

		const auto available2 = LOAD_SYMBOL(lib, webkit_javascript_result_get_global_context)
			&& LOAD_SYMBOL(lib, webkit_javascript_result_get_value)
			&& LOAD_SYMBOL(lib, JSValueToStringCopy)
			&& LOAD_SYMBOL(lib, JSStringGetMaximumUTF8CStringSize)
			&& LOAD_SYMBOL(lib, JSStringGetUTF8CString)
			&& LOAD_SYMBOL(lib, JSStringRelease);
		if (!available1 && !available2) {
			return false;
		}
	}
	{
		const auto available1 = LOAD_SYMBOL(lib, webkit_navigation_policy_decision_get_navigation_action)
			&& LOAD_SYMBOL(lib, webkit_navigation_action_get_request);

		const auto available2 = LOAD_SYMBOL(lib, webkit_navigation_policy_decision_get_request);

		if (!available1 && !available2) {
			return false;
		}
	}
	if (LOAD_SYMBOL(lib, gdk_set_allowed_backends)) {
		gdk_set_allowed_backends(wayland ? "wayland" : "x11");
	}
	return gtk_init_check(0, 0);
}

} // namespace Webview::WebKitGTK::Library
