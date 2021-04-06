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
    # Thanks https://github.com/clarkezone/flutter_win_webview/blob/master/webview_popupauth/windows/CMakeLists.txt
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
    # target_link_libraries(lib_webview
    # PRIVATE
    #     ${webview2_loc}/Microsoft.Web.WebView2.targets
    #     ${src_loc}/ForceStaticLink.targets
    # )
    #
    # This works, but just to be sure, manually link to static library.
    #
    target_include_directories(lib_webview
    PRIVATE
        ${webview2_loc}/include
    )
    if (build_win64)
        set(webview2_lib_folder x64)
    else()
        set(webview2_lib_folder x86)
    endif()
    target_link_libraries(lib_webview
    PRIVATE
        ${webview2_loc}/${webview2_lib_folder}/WebView2LoaderStatic.lib
    )

    # Right now we need two libraries, one that deals with "winrt::",
    # and the other that can use lib_base / range-v3 / etc.

    # See https://developercommunity2.visualstudio.com/t/windowsfoundationh-header-compiler-error-on-clates/1350829

    add_library(lib_webview_winrt STATIC)
    add_library(desktop-app::lib_webview_winrt ALIAS lib_webview_winrt)
    init_target(lib_webview_winrt)

    target_compile_options(lib_webview_winrt
    PRIVATE
        /await
        /Zc:twoPhase-
    )

    nice_target_sources(lib_webview_winrt ${src_loc}
    PRIVATE
        webview/platform/win/webview_windows_edge_html.cpp
        webview/platform/win/webview_windows_edge_html.h
        webview/platform/win/webview_windows_winrt.cpp
        webview/platform/win/webview_windows_winrt.h
    )

    target_include_directories(lib_webview_winrt
    PRIVATE
        ${src_loc}
    )

    target_link_libraries(lib_webview
    PRIVATE
        desktop-app::lib_webview_winrt
    )
elseif (LINUX)
    if (NOT DESKTOP_APP_DISABLE_GTK_INTEGRATION)
        find_package(PkgConfig REQUIRED)

        pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
        target_include_directories(lib_webview PUBLIC ${GTK3_INCLUDE_DIRS})

        pkg_check_modules(WEBKIT REQUIRED IMPORTED_TARGET webkit2gtk-4.0)
        target_include_directories(lib_webview PUBLIC ${WEBKIT_INCLUDE_DIRS})
    else()
        remove_target_sources(lib_webview ${src_loc}
            webview/platform/linux/webview_linux_webkit_gtk.cpp
            webview/platform/linux/webview_linux_webkit_gtk.h
            webview/platform/linux/webview_linux_webkit2gtk.cpp
            webview/platform/linux/webview_linux_webkit2gtk.h
        )
    endif()
endif()
