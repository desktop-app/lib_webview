// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include "webview/webview_common.h"

#include <gio/gio.h>

#define GTK_TYPE_CONTAINER (gtk_container_get_type ())
#define GTK_CONTAINER(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), GTK_TYPE_CONTAINER, GtkContainer))

#define GTK_TYPE_WIDGET (gtk_widget_get_type ())
#define GTK_WIDGET(widget) (G_TYPE_CHECK_INSTANCE_CAST ((widget), GTK_TYPE_WIDGET, GtkWidget))

#define GTK_TYPE_WINDOW (gtk_window_get_type ())
#define GTK_WINDOW(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), GTK_TYPE_WINDOW, GtkWindow))

#define GTK_TYPE_NATIVE (gtk_native_get_type ())
#define GTK_NATIVE(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), GTK_TYPE_NATIVE, GtkNative))
#define GTK_IS_NATIVE(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GTK_TYPE_NATIVE))

#define GTK_TYPE_EVENT_CONTROLLER (gtk_event_controller_get_type ())
#define GTK_EVENT_CONTROLLER(o) (G_TYPE_CHECK_INSTANCE_CAST ((o), GTK_TYPE_EVENT_CONTROLLER, GtkEventController))

#define GTK_TYPE_PLUG (gtk_plug_get_type ())
#define GTK_PLUG(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), GTK_TYPE_PLUG, GtkPlug))

#define GTK_TYPE_STYLE_PROVIDER (gtk_style_provider_get_type ())
#define GTK_STYLE_PROVIDER(o) (G_TYPE_CHECK_INSTANCE_CAST ((o), GTK_TYPE_STYLE_PROVIDER, GtkStyleProvider))
#define GTK_STYLE_PROVIDER_PRIORITY_APPLICATION 600

#define GDK_TYPE_TOPLEVEL (gdk_toplevel_get_type ())
#define GDK_TOPLEVEL(object) (G_TYPE_CHECK_INSTANCE_CAST ((object), GDK_TYPE_TOPLEVEL, GdkToplevel))
#define GDK_IS_TOPLEVEL(object) (G_TYPE_CHECK_INSTANCE_TYPE ((object), GDK_TYPE_TOPLEVEL))

#define GDK_TYPE_X11_DISPLAY (gdk_x11_display_get_type ())
#define GDK_IS_X11_DISPLAY(object) (G_TYPE_CHECK_INSTANCE_TYPE ((object), GDK_TYPE_X11_DISPLAY))

#define GDK_TYPE_X11_SCREEN (gdk_x11_screen_get_type ())
#define GDK_IS_X11_SCREEN(object) (G_TYPE_CHECK_INSTANCE_TYPE ((object), GDK_TYPE_X11_SCREEN))

#define GDK_TYPE_X11_SURFACE (gdk_x11_surface_get_type ())
#define GDK_IS_X11_SURFACE(object) (G_TYPE_CHECK_INSTANCE_TYPE ((object), GDK_TYPE_X11_SURFACE))

#define GDK_TYPE_X11_WINDOW (gdk_x11_window_get_type ())
#define GDK_IS_X11_WINDOW(object) (G_TYPE_CHECK_INSTANCE_TYPE ((object), GDK_TYPE_X11_WINDOW))

#define GDK_TYPE_WAYLAND_TOPLEVEL (gdk_wayland_toplevel_get_type ())
#define GDK_WAYLAND_TOPLEVEL(object) (G_TYPE_CHECK_INSTANCE_CAST ((object), GDK_TYPE_WAYLAND_TOPLEVEL, GdkWaylandToplevel))
#define GDK_IS_WAYLAND_TOPLEVEL(object) (G_TYPE_CHECK_INSTANCE_TYPE ((object), GDK_TYPE_WAYLAND_TOPLEVEL))

#define GDK_TYPE_WAYLAND_WINDOW (gdk_wayland_window_get_type ())
#define GDK_IS_WAYLAND_WINDOW(object) (G_TYPE_CHECK_INSTANCE_TYPE ((object), GDK_TYPE_WAYLAND_WINDOW))

#define WEBKIT_TYPE_NAVIGATION_POLICY_DECISION (webkit_navigation_policy_decision_get_type())
#define WEBKIT_NAVIGATION_POLICY_DECISION(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), WEBKIT_TYPE_NAVIGATION_POLICY_DECISION, WebKitNavigationPolicyDecision))

#define WEBKIT_TYPE_WEB_VIEW (webkit_web_view_get_type())
#define WEBKIT_WEB_VIEW(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), WEBKIT_TYPE_WEB_VIEW, WebKitWebView))

struct _GdkRGBA {
	float red;
	float green;
	float blue;
	float alpha;
};

typedef struct _GdkDisplay GdkDisplay;
typedef struct _GdkDevice GdkDevice;
typedef struct _GdkScreen GdkScreen;
typedef struct _GdkRGBA GdkRGBA;
typedef struct _GdkSurface GdkSurface;
typedef struct _GdkVisual GdkVisual;
typedef struct _GdkWindow GdkWindow;
typedef struct _GtkContainer GtkContainer;
typedef struct _GtkNative GtkNative;
typedef struct _GtkWidget GtkWidget;
typedef struct _GtkWindow GtkWindow;
typedef struct _GtkPlug GtkPlug;
typedef struct _GtkAdjustment GtkAdjustment;
typedef struct _GtkStyleContext GtkStyleContext;
typedef struct _GtkStyleProvider GtkStyleProvider;
typedef struct _GtkCssProvider GtkCssProvider;
typedef struct _GdkEvent GdkEvent;
typedef struct _GdkEventButton GdkEventButton;
typedef struct _GdkEventKey GdkEventKey;
typedef struct _GdkToplevel GdkToplevel;
typedef GdkToplevel GdkWaylandToplevel;
typedef struct _GdkToplevelSize GdkToplevelSize;
typedef struct _GtkEventController GtkEventController;
typedef struct _GtkGesture GtkGesture;
typedef unsigned int GdkModifierType;
typedef void (*GdkWaylandToplevelExported)(
	GdkToplevel *toplevel,
	const char *handle,
	gpointer user_data);
typedef void (*GdkWaylandWindowExported)(
	GdkWindow *window,
	const char *handle,
	gpointer user_data);

typedef struct _JSCValue JSCValue;

typedef struct _WebKitJavascriptResult WebKitJavascriptResult;
typedef struct _WebKitNavigationAction WebKitNavigationAction;
typedef struct _WebKitNavigationPolicyDecision WebKitNavigationPolicyDecision;
typedef struct _WebKitPolicyDecision WebKitPolicyDecision;
typedef struct _WebKitURIRequest WebKitURIRequest;
typedef struct _WebKitUserContentManager WebKitUserContentManager;
typedef struct _WebKitUserScript WebKitUserScript;
typedef struct _WebKitWebView WebKitWebView;
typedef struct _WebKitSettings WebKitSettings;
typedef struct _WebKitScriptDialog WebKitScriptDialog;
typedef struct _WebKitWebsiteDataManager WebKitWebsiteDataManager;
typedef struct _WebKitWebContext WebKitWebContext;
typedef struct _WebKitNetworkSession WebKitNetworkSession;
typedef struct _WebKitAuthenticationRequest WebKitAuthenticationRequest;
typedef struct _WebKitCredential WebKitCredential;

typedef enum {
	GTK_WINDOW_TOPLEVEL,
	GTK_WINDOW_POPUP,
} GtkWindowType;

typedef enum {
	GDK_WINDOW_EDGE_NORTH_WEST,
	GDK_WINDOW_EDGE_NORTH,
	GDK_WINDOW_EDGE_NORTH_EAST,
	GDK_WINDOW_EDGE_WEST,
	GDK_WINDOW_EDGE_EAST,
	GDK_WINDOW_EDGE_SOUTH_WEST,
	GDK_WINDOW_EDGE_SOUTH,
	GDK_WINDOW_EDGE_SOUTH_EAST,
} GdkWindowEdge;

typedef enum {
	GDK_SURFACE_EDGE_NORTH_WEST,
	GDK_SURFACE_EDGE_NORTH,
	GDK_SURFACE_EDGE_NORTH_EAST,
	GDK_SURFACE_EDGE_WEST,
	GDK_SURFACE_EDGE_EAST,
	GDK_SURFACE_EDGE_SOUTH_WEST,
	GDK_SURFACE_EDGE_SOUTH,
	GDK_SURFACE_EDGE_SOUTH_EAST,
} GdkSurfaceEdge;

typedef enum {
	GTK_SHADOW_NONE,
	GTK_SHADOW_IN,
	GTK_SHADOW_OUT,
	GTK_SHADOW_ETCHED_IN,
	GTK_SHADOW_ETCHED_OUT,
} GtkShadowType;

typedef enum {
	WEBKIT_WEB_PROCESS_CRASHED,
	WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT,
	WEBKIT_WEB_PROCESS_TERMINATED_BY_API,
} WebKitWebProcessTerminationReason;

typedef enum {
	WEBKIT_LOAD_STARTED,
	WEBKIT_LOAD_REDIRECTED,
	WEBKIT_LOAD_COMMITTED,
	WEBKIT_LOAD_FINISHED,
} WebKitLoadEvent;

typedef enum {
	WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION,
	WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION,
	WEBKIT_POLICY_DECISION_TYPE_RESPONSE,
} WebKitPolicyDecisionType;

typedef enum {
	WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
	WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
} WebKitUserContentInjectedFrames;

typedef enum {
	WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
	WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END,
} WebKitUserScriptInjectionTime;

typedef enum {
    WEBKIT_SCRIPT_DIALOG_ALERT,
    WEBKIT_SCRIPT_DIALOG_CONFIRM,
    WEBKIT_SCRIPT_DIALOG_PROMPT,
    WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM,
} WebKitScriptDialogType;

typedef enum {
	WEBKIT_CREDENTIAL_PERSISTENCE_NONE,
	WEBKIT_CREDENTIAL_PERSISTENCE_FOR_SESSION,
	WEBKIT_CREDENTIAL_PERSISTENCE_PERMANENT,
} WebKitCredentialPersistence;

namespace Webview::WebKitGTK::Library {

inline gboolean (*gtk_init_check)(int *argc, char ***argv);
inline void (*gdk_set_allowed_backends)(const gchar *backends);
inline GType (*gtk_widget_get_type)(void);
inline GType (*gtk_container_get_type)(void);
inline void (*gtk_container_add)(
	GtkContainer *container,
	GtkWidget *widget);
inline void (*gtk_window_set_child)(
	GtkWindow *window,
	GtkWidget *child);
inline GtkWidget *(*gtk_window_new)(GtkWindowType type);
inline void (*gtk_window_set_title)(
	GtkWindow *window,
	const gchar *title);
inline void (*gtk_window_set_decorated)(
	GtkWindow *window,
	gboolean setting);
inline void (*gtk_window_set_default_size)(
	GtkWindow *window,
	gint width,
	gint height);
inline void (*gtk_window_fullscreen)(GtkWindow *window);
inline void (*gtk_window_unfullscreen)(GtkWindow *window);
inline GtkWidget *(*gtk_scrolled_window_new)(
	GtkAdjustment *hadjustment,
	GtkAdjustment *vadjustment);
inline void (*gtk_scrolled_window_set_shadow_type)(
	GtkWidget *scrolled_window,
	GtkShadowType type);
inline void (*gtk_window_destroy)(GtkWindow *widget);
inline void (*gtk_widget_destroy)(GtkWidget *widget);
inline void (*gtk_widget_set_size_request)(
	GtkWidget *window,
	gint width,
	gint height);
inline void (*gtk_widget_set_visible)(GtkWidget *widget, gboolean visible);
inline void (*gtk_widget_set_app_paintable)(
	GtkWidget *widget,
	gboolean app_paintable);
inline void (*gtk_widget_show_all)(GtkWidget *widget);
inline GType (*gtk_window_get_type)(void);
inline GdkDisplay *(*gtk_widget_get_display)(GtkWidget *widget);
inline GdkWindow *(*gtk_widget_get_window)(GtkWidget *widget);
inline GdkScreen *(*gtk_widget_get_screen)(GtkWidget *widget);
inline void (*gtk_widget_set_visual)(
	GtkWidget *widget,
	GdkVisual *visual);
inline gint (*gtk_widget_get_scale_factor)(GtkWidget *widget);
inline gboolean (*gdk_display_is_composited)(GdkDisplay *display);
inline gboolean (*gdk_screen_is_composited)(GdkScreen *screen);
inline GdkVisual *(*gdk_screen_get_rgba_visual)(GdkScreen *screen);
inline GtkStyleContext *(*gtk_widget_get_style_context)(GtkWidget *widget);
inline void (*gtk_widget_add_css_class)(
	GtkWidget *widget,
	const char *css_class);
inline void (*gtk_style_context_add_provider_for_display)(
	GdkDisplay *display,
	GtkStyleProvider *provider,
	guint priority);
inline void (*gtk_style_context_add_provider_for_screen)(
	GdkScreen *screen,
	GtkStyleProvider *provider,
	guint priority);
inline void (*gtk_style_context_add_class)(
	GtkStyleContext *context,
	const char *class_name);
inline GType (*gtk_style_provider_get_type)(void);
inline GtkCssProvider *(*gtk_css_provider_new)(void);
inline void (*gtk_css_provider_load_from_string)(
	GtkCssProvider *css_provider,
	const char *string);
inline void (*gtk_css_provider_load_from_data)(
	GtkCssProvider *css_provider,
	const gchar *data,
	gssize length,
	GError **error);
inline GtkGesture *(*gtk_gesture_click_new)(void);
inline GtkEventController *(*gtk_event_controller_key_new)(void);
inline GType (*gtk_event_controller_get_type)(void);
inline void (*gtk_widget_add_controller)(
	GtkWidget *widget,
	GtkEventController *controller);
inline void (*gtk_window_begin_move_drag)(
	GtkWindow *window,
	gint button,
	gint root_x,
	gint root_y,
	guint32 timestamp);
inline void (*gtk_window_begin_resize_drag)(
	GtkWindow *window,
	GdkWindowEdge edge,
	gint button,
	gint root_x,
	gint root_y,
	guint32 timestamp);
inline GdkSurface *(*gtk_native_get_surface)(GtkNative *self);
inline GType (*gtk_native_get_type)(void);
inline GType (*gdk_toplevel_get_type)(void);
inline GType (*gdk_x11_display_get_type)(void);
inline GType (*gdk_x11_screen_get_type)(void);
inline GType (*gdk_x11_surface_get_type)(void);
inline GType (*gdk_x11_window_get_type)(void);
inline GType (*gdk_wayland_toplevel_get_type)(void);
inline GType (*gdk_wayland_window_get_type)(void);
inline unsigned long (*gdk_x11_surface_get_xid)(GdkSurface *surface);
inline unsigned long (*gdk_x11_window_get_xid)(GdkWindow *window);
inline void (*gdk_window_set_shadow_width)(
	GdkWindow *window,
	gint left,
	gint right,
	gint top,
	gint bottom);
inline void (*gdk_toplevel_begin_move)(
	GdkToplevel *toplevel,
	GdkDevice *device,
	int button,
	double x,
	double y,
	guint32 timestamp);
inline void (*gdk_toplevel_begin_resize)(
	GdkToplevel *toplevel,
	GdkSurfaceEdge edge,
	GdkDevice *device,
	int button,
	double x,
	double y,
	guint32 timestamp);
inline void (*gdk_toplevel_size_set_shadow_width)(
	GdkToplevelSize *size,
	int left,
	int right,
	int top,
	int bottom);
inline gboolean (*gdk_wayland_toplevel_export_handle)(
	GdkToplevel *toplevel,
	GdkWaylandToplevelExported callback,
	gpointer user_data,
	GDestroyNotify destroy_func);
inline void (*gdk_wayland_toplevel_drop_exported_handle)(
	GdkToplevel *toplevel,
	const char *handle);
inline void (*gdk_wayland_toplevel_unexport_handle)(
	GdkToplevel *toplevel);
inline gboolean (*gdk_wayland_window_export_handle)(
	GdkWindow *window,
	GdkWaylandWindowExported callback,
	gpointer user_data,
	GDestroyNotify destroy_func);
inline void (*gdk_wayland_window_unexport_handle)(GdkWindow *window);
inline void (*gdk_wayland_window_announce_csd)(GdkWindow *window);
inline gint (*gdk_surface_get_width)(GdkSurface *surface);
inline gint (*gdk_surface_get_height)(GdkSurface *surface);
inline void (*gtk_window_get_size)(
	GtkWindow *window,
	gint *width,
	gint *height);

// returns Window that is a typedef to unsigned long,
// but we avoid to include Xlib.h here
inline GtkWidget *(*gtk_plug_new)(unsigned long socket_id);
inline unsigned long (*gtk_plug_get_id)(GtkPlug *plug);
inline GType (*gtk_plug_get_type)(void);

inline char *(*jsc_value_to_string)(JSCValue *value);
inline JSCValue *(*webkit_javascript_result_get_js_value)(
	WebKitJavascriptResult *js_result);

inline GType (*webkit_navigation_policy_decision_get_type)(void);
inline WebKitNavigationAction *(*webkit_navigation_policy_decision_get_navigation_action)(
	WebKitNavigationPolicyDecision *decision);
inline WebKitURIRequest *(*webkit_navigation_action_get_request)(
	WebKitNavigationAction *navigation);
inline const gchar *(*webkit_uri_request_get_uri)(WebKitURIRequest *request);
inline void (*webkit_policy_decision_ignore)(WebKitPolicyDecision *decision);

inline WebKitScriptDialogType (*webkit_script_dialog_get_dialog_type)(
	WebKitScriptDialog *dialog);
inline const gchar *(*webkit_script_dialog_get_message)(
	WebKitScriptDialog *dialog);
inline void (*webkit_script_dialog_confirm_set_confirmed)(
	WebKitScriptDialog *dialog,
	gboolean confirmed);
inline const gchar *(*webkit_script_dialog_prompt_get_default_text)(
	WebKitScriptDialog *dialog);
inline void (*webkit_script_dialog_prompt_set_text)(
	WebKitScriptDialog *dialog,
	const gchar *text);

inline GtkWidget *(*webkit_web_view_new_with_context)(WebKitWebContext *context);
inline GType (*webkit_web_view_get_type)(void);
inline gboolean (*webkit_web_view_get_is_web_process_responsive)(
	WebKitWebView *web_view);
inline WebKitUserContentManager *(*webkit_web_view_get_user_content_manager)(
	WebKitWebView *web_view);
inline const gchar *(*webkit_web_view_get_uri)(WebKitWebView *web_view);
inline const gchar *(*webkit_web_view_get_title)(WebKitWebView *web_view);
inline gboolean (*webkit_web_view_can_go_back)(WebKitWebView *web_view);
inline gboolean (*webkit_web_view_can_go_forward)(WebKitWebView *web_view);
inline gboolean (*webkit_user_content_manager_register_script_message_handler)(
	WebKitUserContentManager *manager,
	const gchar *name,
	const gchar *world_name);
inline WebKitSettings *(*webkit_web_view_get_settings)(
	WebKitWebView *web_view);
inline void (*webkit_settings_set_enable_developer_extras)(
	WebKitSettings *settings,
	gboolean enabled);
inline gboolean (*webkit_web_view_is_loading)(WebKitWebView *web_view);
inline void (*webkit_web_view_load_uri)(
	WebKitWebView *web_view,
	const gchar *uri);
inline void (*webkit_web_view_reload_bypass_cache)(WebKitWebView *web_view);
inline WebKitUserScript *(*webkit_user_script_new)(
	const gchar *source,
	WebKitUserContentInjectedFrames injected_frames,
	WebKitUserScriptInjectionTime injection_time,
	const gchar *const *whitelist,
	const gchar *const *blacklist);
inline void (*webkit_user_content_manager_add_script)(
	WebKitUserContentManager *manager,
	WebKitUserScript *script);
inline void (*webkit_web_view_evaluate_javascript)(
	WebKitWebView *web_view,
	const gchar *script,
	gssize length,
	const gchar *world_name,
	const gchar *source_uri,
	GCancellable *cancellable,
	GAsyncReadyCallback callback,
	gpointer user_data);
inline void (*webkit_web_view_run_javascript)(
	WebKitWebView *web_view,
	const gchar *script,
	GCancellable *cancellable,
	GAsyncReadyCallback callback,
	gpointer user_data);
inline void (*webkit_web_view_set_background_color)(
	WebKitWebView *web_view,
	const GdkRGBA *rgba);
inline WebKitWebsiteDataManager *(*webkit_website_data_manager_new)(
	const gchar *first_option_name,
	...);
inline WebKitWebContext *(*webkit_web_context_new_with_website_data_manager)(
	WebKitWebsiteDataManager* manager);
inline WebKitNetworkSession *(*webkit_network_session_new)(
	const char* data_directory,
	const char* cache_directory);
inline void (*webkit_authentication_request_authenticate)(
	WebKitAuthenticationRequest *request,
	WebKitCredential *credential);
inline const gchar *(*webkit_authentication_request_get_host)(
	WebKitAuthenticationRequest *request);
inline guint (*webkit_authentication_request_get_port)(
	WebKitAuthenticationRequest *request);
inline WebKitCredential *(*webkit_credential_new)(
	const gchar *username,
	const gchar *password,
	WebKitCredentialPersistence persistence);
inline void (*webkit_credential_free)(WebKitCredential *credential);

enum class ResolveResult {
	Success,
	NoLibrary,
	CantInit,
	IPCFailure,
};

enum class Platform {
	Any,
	Wayland,
	X11,
};

[[nodiscard]] ResolveResult Resolve(
	const Platform &platform,
	WindowMode mode);

} // namespace Webview::WebKitGTK::Library
