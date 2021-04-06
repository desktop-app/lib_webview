// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include <QtCore/QMetaType>

extern "C" {
#undef signals
#include <JavaScriptCore/JavaScript.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <X11/Xlib.h>
#define signals public
} // extern "C"

namespace Webview::WebkitGtk {

inline GType (*gtk_widget_get_type)(void);
inline void (*gtk_widget_grab_focus)(GtkWidget *widget);
inline GType (*gtk_container_get_type)(void);
inline void (*gtk_container_add)(
	GtkContainer *container,
	GtkWidget *widget);
inline GdkWindow *(*gtk_widget_get_window)(GtkWidget *widget);
inline GtkWidget *(*gtk_window_new)(GtkWindowType type);
inline void (*gtk_widget_hide)(GtkWidget *widget);
inline void (*gtk_widget_show_all)(GtkWidget *widget);
inline GType (*gtk_window_get_type)(void);
inline void (*gtk_window_set_decorated)(GtkWindow *window, gboolean setting);
inline Window (*gdk_x11_window_get_xid)(GdkWindow *window);

inline char *(*jsc_value_to_string)(JSCValue *value);
inline JSStringRef (*JSValueToStringCopy)(
	JSContextRef ctx,
	JSValueRef value,
	JSValueRef* exception);
inline size_t (*JSStringGetMaximumUTF8CStringSize)(JSStringRef string);
inline size_t (*JSStringGetUTF8CString)(
	JSStringRef string,
	char* buffer,
	size_t bufferSize);
inline void (*JSStringRelease)(JSStringRef string);

inline guint (*webkit_get_major_version)();
inline guint (*webkit_get_minor_version)();
inline guint (*webkit_get_micro_version)();
inline JSCValue *(*webkit_javascript_result_get_js_value)(
	WebKitJavascriptResult *js_result);
inline JSGlobalContextRef (*webkit_javascript_result_get_global_context)(
	WebKitJavascriptResult *js_result);
inline JSValueRef (*webkit_javascript_result_get_value)(
	WebKitJavascriptResult *js_result);

inline GtkWidget *(*webkit_web_view_new)(void);
inline GType (*webkit_web_view_get_type)(void);
inline WebKitUserContentManager *(*webkit_web_view_get_user_content_manager)(
	WebKitWebView *web_view);
inline gboolean (*webkit_user_content_manager_register_script_message_handler)(
	WebKitUserContentManager *manager,
	const gchar *name);
inline WebKitSettings *(*webkit_web_view_get_settings)(
	WebKitWebView *web_view);
inline void (*webkit_settings_set_javascript_can_access_clipboard)(
	WebKitSettings *settings,
	gboolean enabled);
inline void (*webkit_web_view_load_uri)(
	WebKitWebView *web_view,
	const gchar *uri);
inline WebKitUserScript *(*webkit_user_script_new)(
	const gchar *source,
	WebKitUserContentInjectedFrames injected_frames,
	WebKitUserScriptInjectionTime injection_time,
	const gchar* const *whitelist,
	const gchar* const *blacklist);
inline void (*webkit_user_content_manager_add_script)(
	WebKitUserContentManager *manager,
	WebKitUserScript *script);
inline void (*webkit_web_view_run_javascript)(
	WebKitWebView *web_view,
	const gchar *script,
	GCancellable *cancellable,
	GAsyncReadyCallback callback,
	gpointer user_data);

[[nodiscard]] bool Resolve();

} // namespace Webview::WebkitGtk
