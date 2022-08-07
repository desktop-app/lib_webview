// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
import QtQuick
import QtQuick.Window
import QtWayland.Compositor

WaylandOutput {
    id: screen
    scaleFactor: Screen.devicePixelRatio
    sizeFollowsWindow: true

    property ListModel shellSurfaces: ListModel {}
    property bool windowFollowsSize: true
    signal surfaceCompleted()

    window: Window {
        WaylandMouseTracker {
            id: mouseTracker
            anchors.fill: parent
            windowSystemCursorEnabled: !clientCursor.visible

            Repeater {
                model: shellSurfaces
                ShellSurfaceItem {
                    shellSurface: modelData
                    output: screen
                    // Uncomment to avoid double menus with the output-for-every-popup prototype
                    // autoCreatePopupItems: false
                    x: -shellSurface.windowGeometry.x
                    y: -shellSurface.windowGeometry.y
                    onSurfaceDestroyed: screen.shellSurfaces.remove(index)

                    property bool completed: false

                    moveItem: Item {
                        enabled: false
                    }

                    function ensureSize() {
                        const geometry = shellSurface.windowGeometry;
                        if (!windowFollowsSize || (geometry.width > 0 && geometry.height > 0)) {
                            if (shellSurface.toplevel !== null) {
                                shellSurface.toplevel.sendFullscreen(Qt.size(window.width, window.height));
                            } else if (shellSurface.popup !== null) {
                                shellSurface.popup.sendConfigure(Qt.rect(0, 0, window.width, window.height));
                            }
                        }
                    }

                    Connections {
                        target: shellSurface

                        function onWindowGeometryChanged() {
                            if (windowFollowsSize) {
                                const geometry = shellSurface.windowGeometry;
                                window.width = geometry.width;
                                window.height = geometry.height;
                            }

                            if (!completed && window.width > 0 && window.height > 0) {
                                completed = true;
                                surfaceCompleted();
                            }
                        }
                    }

                    Connections {
                        target: shellSurface.toplevel

                        function onTitleChanged() {
                            window.title = shellSurface.toplevel.title;
                        }

                        function onFullscreenChanged() {
                            if (!shellSurface.toplevel.fullscreen) {
                                shellSurface.toplevel.sendFullscreen(Qt.size(window.width, window.height));
                            }
                        }
                    }

                    Connections {
                        target: window

                        function onClosing() {
                            if (shellSurface.toplevel !== null) {
                                shellSurface.toplevel.sendClose();
                            } else if (shellSurface.popup !== null) {
                                shellSurface.popup.sendPopupDone();
                            }
                            window.close.accepted = false;
                        }

                        function onWidthChanged() {
                            ensureSize();
                        }

                        function onHeightChanged() {
                            ensureSize();
                        }
                    }
                }
            }

            WaylandCursorItem {
                id: clientCursor
                inputEventsEnabled: false
                x: mouseTracker.mouseX
                y: mouseTracker.mouseY
                visible: surface !== null && mouseTracker.containsMouse
                seat: screen.compositor.defaultSeat
            }
        }
    }
}
