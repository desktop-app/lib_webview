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
inline GtkWidget *(*gtk_scrolled_window_new)(
	GtkAdjustment *hadjustment,
	GtkAdjustment *vadjustment);
inline void (*gtk_window_destroy)(GtkWindow *widget);
inline void (*gtk_widget_destroy)(GtkWidget *widget);
inline void (*gtk_widget_set_size_request)(
	GtkWidget *window,
	gint width,
	gint height);
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

[[nodiscard]] ResolveResult Resolve(const Platform &platform);

} // namespace Webview::WebKitGTK::Library
