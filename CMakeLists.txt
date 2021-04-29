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
    webview/webview_embed.cpp
    webview/webview_embed.h
    webview/webview_interface.h

    webview/platform/mac/webview_mac.h
    webview/platform/mac/webview_mac.mm

    webview/platform/linux/webview_linux.h
    webview/platform/linux/webview_linux.cpp
    webview/platform/linux/webview_linux_webkit_gtk.cpp
    webview/platform/linux/webview_linux_webkit_gtk.h
    webview/platform/linux/webview_linux_webkit2gtk.cpp
    webview/platform/linux/webview_linux_webkit2gtk.h

    webview/platform/win/webview_win.cpp
    webview/platform/win/webview_win.h
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
    desktop-app::lib_base
)

if (WIN32)
    nuget_add_webview(lib_webview)
    nuget_add_winrt(lib_webview)
elseif (LINUX)
    if (NOT DESKTOP_APP_DISABLE_WEBKITGTK)
        find_package(PkgConfig REQUIRED)

        pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
        target_include_directories(lib_webview SYSTEM PUBLIC ${GTK3_INCLUDE_DIRS})

        pkg_check_modules(WEBKIT REQUIRED webkit2gtk-4.0)
        target_include_directories(lib_webview SYSTEM PUBLIC ${WEBKIT_INCLUDE_DIRS})
    else()
        remove_target_sources(lib_webview ${src_loc}
            webview/platform/linux/webview_linux_webkit_gtk.cpp
            webview/platform/linux/webview_linux_webkit_gtk.h
            webview/platform/linux/webview_linux_webkit2gtk.cpp
            webview/platform/linux/webview_linux_webkit2gtk.h
        )
    endif()
endif()
