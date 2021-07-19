// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/linux/webview_linux_webkit_gtk.h"

#ifdef DESKTOP_APP_DISABLE_GTK_INTEGRATION
#error "WebKitGTK support depends on GTK integration."
#endif // DESKTOP_APP_DISABLE_GTK_INTEGRATION

#include "base/platform/linux/base_linux_gtk_integration.h"
#include "base/platform/linux/base_linux_gtk_integration_p.h"

extern "C" {
#include <gdk/gdkx.h>
} // extern "C"

namespace Webview::WebkitGtk {
namespace {

using BaseGtkIntegration = base::Platform::GtkIntegration;

} // namespace

bool Resolve() {
	if (!BaseGtkIntegration::Instance()) {
		return false;
	}

	if (!BaseGtkIntegration::Instance()->loaded()) {
		BaseGtkIntegration::Instance()->load("x11", true);
	}

	if (!BaseGtkIntegration::Instance()->loaded()
		|| !BaseGtkIntegration::Instance()->checkVersion(3, 0, 0)) {
		return false;
	}

	auto &gtk = BaseGtkIntegration::Instance()->library();

	auto webkit2gtk = QLibrary();
	const auto result = LOAD_GTK_SYMBOL(gtk, gtk_widget_get_type)
		&& LOAD_GTK_SYMBOL(gtk, gtk_widget_grab_focus)
		&& LOAD_GTK_SYMBOL(gtk, gtk_container_get_type)
		&& LOAD_GTK_SYMBOL(gtk, gtk_container_add)
		&& LOAD_GTK_SYMBOL(gtk, gtk_widget_get_window)
		&& LOAD_GTK_SYMBOL(gtk, gtk_window_new)
		&& LOAD_GTK_SYMBOL(gtk, gtk_widget_destroy)
		&& LOAD_GTK_SYMBOL(gtk, gtk_widget_hide)
		&& LOAD_GTK_SYMBOL(gtk, gtk_widget_show_all)
		&& LOAD_GTK_SYMBOL(gtk, gtk_window_get_type)
		&& LOAD_GTK_SYMBOL(gtk, gtk_window_set_decorated)
		&& LOAD_GTK_SYMBOL(gtk, gdk_x11_window_get_xid)
		&& base::Platform::Gtk::LoadGtkLibrary(webkit2gtk, "libwebkit2gtk-4.0.so.37")
		&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_web_view_new)
		&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_web_view_get_type)
		&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_web_view_get_user_content_manager)
		&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_user_content_manager_register_script_message_handler)
		&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_web_view_get_settings)
		&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_settings_set_javascript_can_access_clipboard)
		&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_web_view_load_uri)
		&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_user_script_new)
		&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_user_content_manager_add_script)
		&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_web_view_run_javascript)
		&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_uri_request_get_uri)
		&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_policy_decision_ignore)
		&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_navigation_policy_decision_get_type);
	if (!result) {
		return false;
	}
	{
		const auto available1 = LOAD_GTK_SYMBOL(webkit2gtk, jsc_value_to_string)
			&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_javascript_result_get_js_value);

		const auto available2 = LOAD_GTK_SYMBOL(webkit2gtk, webkit_javascript_result_get_global_context)
			&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_javascript_result_get_value)
			&& LOAD_GTK_SYMBOL(webkit2gtk, JSValueToStringCopy)
			&& LOAD_GTK_SYMBOL(webkit2gtk, JSStringGetMaximumUTF8CStringSize)
			&& LOAD_GTK_SYMBOL(webkit2gtk, JSStringGetUTF8CString)
			&& LOAD_GTK_SYMBOL(webkit2gtk, JSStringRelease);
		if (!available1 && !available2) {
			return false;
		}
	}
	{
		const auto available1 = LOAD_GTK_SYMBOL(webkit2gtk, webkit_navigation_policy_decision_get_navigation_action)
			&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_navigation_action_get_request);

		const auto available2 = LOAD_GTK_SYMBOL(webkit2gtk, webkit_navigation_policy_decision_get_request);

		if (!available1 && !available2) {
			return false;
		}
	}
	return true;
}

} // namespace Webview::WebkitGtk
