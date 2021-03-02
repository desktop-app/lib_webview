# This file is part of Desktop App Toolkit,
# a set of libraries for developing nice desktop applications.
#
# For license and copyright information please follow this link:
# https://github.com/desktop-app/legal/blob/master/LEGAL

add_library(lib_webview OBJECT)
add_library(desktop-app::lib_webview ALIAS lib_webview)
init_target(lib_webview)

get_filename_component(src_loc . REALPATH)

# Right now we need two libraries, one that deals with "winrt::",
# and the other that can use lib_base / range-v3 / etc.

# See https://developercommunity2.visualstudio.com/t/windowsfoundationh-header-compiler-error-on-clates/1350829

add_library(lib_webview_wrap STATIC)
add_library(desktop-app::lib_webview_wrap ALIAS lib_webview_wrap)
init_target(lib_webview_wrap)

if (WIN32)
    target_compile_options(lib_webview_wrap
    PRIVATE
        /await
        /Zc:twoPhase-
    )
endif()

nice_target_sources(lib_webview_wrap ${src_loc}
PRIVATE
    webview/webview_interface.h

    webview/platform/mac/webview_mac.h
    webview/platform/mac/webview_mac.mm

    webview/platform/win/webview_win.cpp
    webview/platform/win/webview_win.h
    webview/platform/win/webview_windows_edge_chromium.cpp
    webview/platform/win/webview_windows_edge_chromium.h
    webview/platform/win/webview_windows_edge_html.cpp
    webview/platform/win/webview_windows_edge_html.h
    webview/platform/win/webview_windows_winrt.cpp
    webview/platform/win/webview_windows_winrt.h
)

target_include_directories(lib_webview_wrap
PRIVATE
    ${src_loc}
)

# Thanks https://github.com/clarkezone/flutter_win_webview/blob/master/webview_popupauth/windows/CMakeLists.txt
if (WIN32)
    find_program(NUGET_EXE NAMES nuget)
    if (NOT NUGET_EXE)
	    message("NUGET.EXE not found.")
	    message(FATAL_ERROR "Please install this executable, and run CMake again.")
    endif()

    exec_program(
        ${NUGET_EXE}
    ARGS
        install
        "Microsoft.Web.WebView2"
        -Version 1.0.705.50
        -ExcludeVersion
        -OutputDirectory ${CMAKE_BINARY_DIR}/packages
    )

    set(webview2_loc ${CMAKE_BINARY_DIR}/packages/Microsoft.Web.WebView2/build/native)
    # target_link_libraries(lib_webview_wrap
    # PRIVATE
    #     ${webview2_loc}/Microsoft.Web.WebView2.targets
    #     ${src_loc}/ForceStaticLink.targets
    # )
    #
    # This works, but just to be sure, manually link to static library.
    #
    target_include_directories(lib_webview_wrap
    PRIVATE
        ${webview2_loc}/include
    )
    if (build_win64)
        set(webview2_lib_folder x64)
    else()
        set(webview2_lib_folder x86)
    endif()
    target_link_libraries(lib_webview_wrap
    PRIVATE
        ${webview2_loc}/${webview2_lib_folder}/WebView2LoaderStatic.lib
    )
endif()

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
