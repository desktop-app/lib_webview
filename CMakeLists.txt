# This file is part of Desktop App Toolkit,
# a set of libraries for developing nice desktop applications.
#
# For license and copyright information please follow this link:
# https://github.com/desktop-app/legal/blob/master/LEGAL

add_library(lib_webview STATIC)
add_library(desktop-app::lib_webview ALIAS lib_webview)
init_target(lib_webview)

get_filename_component(src_loc . REALPATH)

nice_target_sources(lib_webview ${src_loc}
PRIVATE
    webview/webview_common.h
    webview/webview_data_stream.h
    webview/webview_data_stream_memory.cpp
    webview/webview_data_stream_memory.h
    webview/webview_dialog.cpp
    webview/webview_dialog.h
    webview/webview_embed.cpp
    webview/webview_embed.h
    webview/webview_interface.h

    webview/platform/mac/webview_mac.h
    webview/platform/mac/webview_mac.mm

    webview/platform/linux/webview_linux.h
    webview/platform/linux/webview_linux.cpp
    webview/platform/linux/webview_linux_compositor.cpp
    webview/platform/linux/webview_linux_compositor.h
    webview/platform/linux/webview_linux_http_server.cpp
    webview/platform/linux/webview_linux_http_server.h
    webview/platform/linux/webview_linux_webkitgtk_library.cpp
    webview/platform/linux/webview_linux_webkitgtk_library.h
    webview/platform/linux/webview_linux_webkitgtk.cpp
    webview/platform/linux/webview_linux_webkitgtk.h

    webview/platform/win/webview_win.cpp
    webview/platform/win/webview_win.h
    webview/platform/win/webview_windows_data_stream.cpp
    webview/platform/win/webview_windows_data_stream.h
    webview/platform/win/webview_windows_edge_chromium.cpp
    webview/platform/win/webview_windows_edge_chromium.h
    webview/platform/win/webview_windows_edge_html.cpp
    webview/platform/win/webview_windows_edge_html.h
)

target_include_directories(lib_webview
PUBLIC
    ${src_loc}
)

target_link_libraries(lib_webview
PUBLIC
    desktop-app::lib_ui
    desktop-app::lib_base
)

if (WIN32)
    nuget_add_webview(lib_webview)
elseif (LINUX)
    include(${cmake_helpers_loc}/external/glib/generate_dbus.cmake)
    generate_dbus(lib_webview org.desktop_app.GtkIntegration.Webview. Webview ${src_loc}/webview/platform/linux/webview_linux_interface.xml)

    target_compile_definitions(lib_webview
    PRIVATE
        G_LOG_DOMAIN="WebView"
    )
endif()
