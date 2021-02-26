# This file is part of Desktop App Toolkit,
# a set of libraries for developing nice desktop applications.
#
# For license and copyright information please follow this link:
# https://github.com/desktop-app/legal/blob/master/LEGAL

add_library(lib_webview OBJECT)
add_library(desktop-app::lib_webview ALIAS lib_webview)
init_target(lib_webview)

get_filename_component(src_loc . REALPATH)

add_library(lib_webview_wrap OBJECT)
add_library(desktop-app::lib_webview_wrap ALIAS lib_webview_wrap)
init_target(lib_webview_wrap)

if (WIN32)
    target_compile_options(lib_webview_wrap PRIVATE /await)
endif()

nice_target_sources(lib_webview_wrap ${src_loc}
PRIVATE
    webview/details/webview_wrap.cpp
    webview/details/webview_wrap.h
)

target_include_directories(lib_webview_wrap
PRIVATE
    ${src_loc}
)

target_link_libraries(lib_webview_wrap
PRIVATE
    desktop-app::external_webview
)

nice_target_sources(lib_webview ${src_loc}
PRIVATE
    webview/webview_embed.cpp
    webview/webview_embed.h
)

target_include_directories(lib_webview
PUBLIC
    ${src_loc}
)

target_link_libraries(lib_webview
PUBLIC
    desktop-app::lib_base
PRIVATE
    desktop-app::lib_webview_wrap
)
