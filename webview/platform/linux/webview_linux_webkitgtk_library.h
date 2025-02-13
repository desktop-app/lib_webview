// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
#pragma once

#include <gio/gio.h>

#define GTK_TYPE_CONTAINER (gtk_container_get_type ())
#define GTK_CONTAINER(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), GTK_TYPE_CONTAINER, GtkContainer))

#define GTK_TYPE_WIDGET (gtk_widget_get_type ())
#define GTK_WIDGET(widget) (G_TYPE_CHECK_INSTANCE_CAST ((widget), GTK_TYPE_WIDGET, GtkWidget))

#define GTK_TYPE_WINDOW (gtk_window_get_type ())
#define GTK_WINDOW(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), GTK_TYPE_WINDOW, GtkWindow))

#define GTK_TYPE_PLUG (gtk_plug_get_type ())
#define GTK_PLUG(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), GTK_TYPE_PLUG, GtkPlug))

#define GTK_TYPE_STYLE_PROVIDER (gtk_style_provider_get_type ())
#define GTK_STYLE_PROVIDER(o) (G_TYPE_CHECK_INSTANCE_CAST ((o), GTK_TYPE_STYLE_PROVIDER, GtkStyleProvider))
#define GTK_STYLE_PROVIDER_PRIORITY_APPLICATION 600

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
typedef struct _GdkScreen GdkScreen;
typedef struct _GdkRGBA GdkRGBA;
typedef struct _GtkContainer GtkContainer;
typedef struct _GtkWidget GtkWidget;
typedef struct _GtkWindow GtkWindow;
typedef struct _GtkPlug GtkPlug;
typedef struct _GtkAdjustment GtkAdjustment;
typedef struct _GtkStyleContext GtkStyleContext;
typedef struct _GtkStyleProvider GtkStyleProvider;
typedef struct _GtkCssProvider GtkCssProvider;

typedef struct _SoupMessageHeaders SoupMessageHeaders;
typedef struct _SoupSession SoupSession;
typedef struct _SoupMessage SoupMessage;

typedef struct _JSCValue JSCValue;

typedef struct _WebKitJavascriptResult WebKitJavascriptResult;
typedef struct _WebKitNavigationAction WebKitNavigationAction;
typedef struct _WebKitNavigationPolicyDecision WebKitNavigationPolicyDecision;
typedef struct _WebKitPolicyDecision WebKitPolicyDecision;
typedef struct _WebKitURIRequest WebKitURIRequest;
typedef struct _WebKitURISchemeRequest WebKitURISchemeRequest;
typedef struct _WebKitURISchemeResponse WebKitURISchemeResponse;
typedef struct _WebKitUserContentManager WebKitUserContentManager;
typedef struct _WebKitUserScript WebKitUserScript;
typedef struct _WebKitWebView WebKitWebView;
typedef struct _WebKitSettings WebKitSettings;
typedef struct _WebKitScriptDialog WebKitScriptDialog;
typedef struct _WebKitWebsiteDataManager WebKitWebsiteDataManager;
typedef struct _WebKitWebContext WebKitWebContext;
typedef struct _WebKitNetworkSession WebKitNetworkSession;

typedef enum {
	GTK_WINDOW_TOPLEVEL,
	GTK_WINDOW_POPUP,
} GtkWindowType;

typedef enum {
	WEBKIT_WEB_PROCESS_CRASHED,
	WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT,
	WEBKIT_WEB_PROCESS_TERMINATED_BY_API,
} WebKitWebProcessTerminationReason;

typedef enum {
	SOUP_MESSAGE_HEADERS_REQUEST,
	SOUP_MESSAGE_HEADERS_RESPONSE,
	SOUP_MESSAGE_HEADERS_MULTIPART,
} SoupMessageHeadersType;

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

typedef void (*WebKitURISchemeRequestCallback)(
	WebKitURISchemeRequest *request,
	gpointer user_data);

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
inline GtkWidget *(*gtk_scrolled_window_new)(
	GtkAdjustment *hadjustment,
	GtkAdjustment *vadjustment);
inline void (*gtk_window_destroy)(GtkWindow *widget);
inline void (*gtk_widget_destroy)(GtkWidget *widget);
inline void (*gtk_window_fullscreen)(GtkWindow *window);
inline void (*gtk_widget_set_visible)(GtkWidget *widget, gboolean visible);
inline void (*gtk_widget_show_all)(GtkWidget *widget);
inline GType (*gtk_window_get_type)(void);
inline GdkDisplay *(*gtk_widget_get_display)(GtkWidget *widget);
inline GdkScreen *(*gtk_widget_get_screen)(GtkWidget *widget);
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

// returns Window that is a typedef to unsigned long,
// but we avoid to include Xlib.h here
inline GtkWidget *(*gtk_plug_new)(unsigned long socket_id);
inline unsigned long (*gtk_plug_get_id)(GtkPlug *plug);
inline GType (*gtk_plug_get_type)(void);


inline SoupSession *(*soup_session_new)(void);
inline GInputStream *(*soup_session_send_finish)(
	SoupSession *session,
	GAsyncResult *result,
	GError **error);
inline void (*soup_session_send_async)(
	SoupSession *session,
	SoupMessage *msg,
	int priority,
	GCancellable *cancellable,
	GAsyncReadyCallback callback,
	gpointer user_data);
inline SoupMessage *(*soup_message_new)(const char *method, const char *uri);
inline SoupMessageHeaders *(*soup_message_headers_new)(
	SoupMessageHeadersType type);
inline void (*soup_message_headers_append)(
	SoupMessageHeaders *hdrs,
	const char *name,
	const char *value);
inline const char *(*soup_message_headers_get_one)(
	SoupMessageHeaders *hdrs,
	const char *name);
inline void (*soup_message_headers_unref)(SoupMessageHeaders *hdrs);
inline void (*soup_message_headers_free)(SoupMessageHeaders *hdrs);

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
inline WebKitWebContext *(*webkit_web_context_new)(void);
inline WebKitWebContext *(*webkit_web_context_new_with_website_data_manager)(
	WebKitWebsiteDataManager* manager);
inline void (*webkit_web_context_register_uri_scheme)(
	WebKitWebContext *context,
	const gchar *scheme,
	WebKitURISchemeRequestCallback callback,
	gpointer user_data,
	GDestroyNotify user_data_destroy_func);
inline WebKitNetworkSession *(*webkit_network_session_new)(
	const char* data_directory,
	const char* cache_directory);
inline const gchar *(*webkit_uri_scheme_request_get_path)(
	WebKitURISchemeRequest *request);
inline void (*webkit_uri_scheme_request_finish_error)(
	WebKitURISchemeRequest *request,
	GError *error);
inline void (*webkit_uri_scheme_request_finish_with_response)(
	WebKitURISchemeRequest *request,
	WebKitURISchemeResponse *response);
inline const gchar *(*webkit_uri_scheme_request_get_uri)(
	WebKitURISchemeRequest *request);
inline SoupMessageHeaders *(*webkit_uri_scheme_request_get_http_headers)(
	WebKitURISchemeRequest *request);
inline WebKitURISchemeResponse *(*webkit_uri_scheme_response_new)(
	GInputStream *stream,
	gint64 stream_length);
inline void (*webkit_uri_scheme_response_set_content_type)(
	WebKitURISchemeResponse *response,
	const gchar *content_type);
inline void (*webkit_uri_scheme_response_set_http_headers)(
	WebKitURISchemeResponse *response,
	SoupMessageHeaders *headers);
inline void (*webkit_uri_scheme_response_set_status)(
	WebKitURISchemeResponse *response,
	guint status_code,
	const gchar *reason_phrase);

enum class ResolveResult {
	Success,
	NoLibrary,
	CantInit,
	IPCFailure,
};

[[nodiscard]] ResolveResult Resolve(bool wayland);

} // namespace Webview::WebKitGTK::Library
