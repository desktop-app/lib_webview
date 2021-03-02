// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#include "webview/platform/linux/webview_linux_webkit_gtk.h"

namespace Webview::WebkitGtk {
namespace {

bool TriedToInit = false;
bool Loaded = false;

bool LoadLibrary(QLibrary &lib, const char *name, int version) {
#ifdef LINK_TO_GTK
	return true;
#else // LINK_TO_GTK
	lib.setFileNameAndVersion(QLatin1String(name), version);
	if (lib.load()) {
		return true;
	}
	lib.setFileNameAndVersion(QLatin1String(name), QString());
	if (lib.load()) {
		return true;
	}
	std::cout << "Could not load library '" << name << "'!" << std::endl;
	return false;
#endif // !LINK_TO_GTK
}

bool SetupGtkBase(QLibrary &lib) {
	if (!LOAD_GTK_SYMBOL(lib, gtk_init_check)) {
		return false;
	} else if (!gtk_init_check(0, 0)) {
		gtk_init_check = nullptr;
		return false;
	}
	return true;
}

} // namespace

bool Resolve() {
	QLibrary gtk;
	gtk.setLoadHints(QLibrary::DeepBindHint);
	QLibrary webkit2gtk;
	webkit2gtk.setLoadHints(QLibrary::DeepBindHint);
	std::cout << "Loading";
	return LoadLibrary(gtk, "gtk-3", 0)
		&& SetupGtkBase(gtk)
		&& LOAD_GTK_SYMBOL(gtk, gtk_widget_get_type)
		&& LOAD_GTK_SYMBOL(gtk, gtk_widget_grab_focus)
		&& LOAD_GTK_SYMBOL(gtk, gtk_container_get_type)
		&& LOAD_GTK_SYMBOL(gtk, gtk_container_add)
		&& LoadLibrary(webkit2gtk, "/usr/lib/x86_64-linux-gnu/libwebkit2gtk-4.0.so.37", 0)
		&& LOAD_GTK_SYMBOL(webkit2gtk, jsc_value_to_string)
		&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_javascript_result_get_js_value)
		&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_web_view_new)
		&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_web_view_get_type)
		&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_web_view_get_user_content_manager)
		&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_user_content_manager_register_script_message_handler)
		&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_web_view_get_settings)
		&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_settings_set_javascript_can_access_clipboard)
		&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_web_view_load_uri)
		&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_user_script_new)
		&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_user_content_manager_add_script)
		&& LOAD_GTK_SYMBOL(webkit2gtk, webkit_web_view_run_javascript);
}

} // namespace Webview::WebkitGtk
