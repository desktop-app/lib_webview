// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include <iostream>

#include <QtCore/QLibrary>

extern "C" {
#undef signals
#include <JavaScriptCore/JavaScript.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#define signals public
} // extern "C"

#if defined DESKTOP_APP_USE_PACKAGED && !defined DESKTOP_APP_USE_PACKAGED_LAZY
#define LINK_TO_GTK
#endif // DESKTOP_APP_USE_PACKAGED && !DESKTOP_APP_USE_PACKAGED_LAZY

#ifdef LINK_TO_GTK
#define LOAD_GTK_SYMBOL(lib, func) ((func = ::func), true)
#else // LINK_TO_GTK
#define LOAD_GTK_SYMBOL(lib, func) (Webview::WebkitGtk::LoadSymbol(lib, #func, func))
#endif // !LINK_TO_GTK

namespace Webview::WebkitGtk {

template <typename Function>
bool LoadSymbol(QLibrary &lib, const char *name, Function &func) {
	func = nullptr;
	if (!lib.isLoaded()) {
		std::cout << "Could not load '" << name << "'!" << std::endl;
		return false;
	}

	func = reinterpret_cast<Function>(lib.resolve(name));
	if (func) {
		return true;
	}
	std::cout << "Could not load '" << name << "'!" << std::endl;
	return false;
}

inline gboolean (*gtk_init_check)(int *argc, char ***argv) = nullptr;
inline GType (*gtk_widget_get_type)(void) = nullptr;
inline void (*gtk_widget_grab_focus)(GtkWidget *widget) = nullptr;
inline GType (*gtk_container_get_type)(void) = nullptr;
inline void (*gtk_container_add)(
	GtkContainer *container,
	GtkWidget *widget) = nullptr;

inline char *(*jsc_value_to_string)(JSCValue *value) = nullptr;

inline JSCValue *(*webkit_javascript_result_get_js_value)(
	WebKitJavascriptResult *js_result) = nullptr;
inline GtkWidget *(*webkit_web_view_new)(void) = nullptr;
inline GType (*webkit_web_view_get_type)(void) = nullptr;
inline WebKitUserContentManager *(*webkit_web_view_get_user_content_manager)(
	WebKitWebView *web_view) = nullptr;
inline gboolean (*webkit_user_content_manager_register_script_message_handler)(
	WebKitUserContentManager *manager,
	const gchar *name) = nullptr;
inline WebKitSettings *(*webkit_web_view_get_settings)(
	WebKitWebView *web_view) = nullptr;
inline void (*webkit_settings_set_javascript_can_access_clipboard)(
	WebKitSettings *settings,
	gboolean enabled) = nullptr;
inline void (*webkit_web_view_load_uri)(
	WebKitWebView *web_view,
	const gchar *uri) = nullptr;
inline WebKitUserScript *(*webkit_user_script_new)(
	const gchar *source,
	WebKitUserContentInjectedFrames injected_frames,
	WebKitUserScriptInjectionTime injection_time,
	const gchar* const *whitelist,
	const gchar* const *blacklist) = nullptr;
inline void (*webkit_user_content_manager_add_script)(
	WebKitUserContentManager *manager,
	WebKitUserScript *script) = nullptr;
inline void (*webkit_web_view_run_javascript)(
	WebKitWebView *web_view,
	const gchar *script,
	GCancellable *cancellable,
	GAsyncReadyCallback callback,
	gpointer user_data) = nullptr;

[[nodiscard]] bool Resolve();

} // namespace Webview::WebkitGtk
