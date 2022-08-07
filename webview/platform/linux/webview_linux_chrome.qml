// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
import QtQuick
import QtWayland.Compositor

WaylandMouseTracker {
    id: mouseTracker
    anchors.fill: parent
    property WaylandOutput output: typeof mainOutput !== "undefined" ? mainOutput : null

    Loader {
        active: output.shellSurface !== null
        sourceComponent: ShellSurfaceItem {
            shellSurface: mouseTracker.output.shellSurface
            output: mouseTracker.output
            autoCreatePopupItems: false
            x: -shellSurface.windowGeometry.x
            y: -shellSurface.windowGeometry.y
            moveItem: Item { enabled: false }
            onSurfaceDestroyed: mouseTracker.output.shellSurface = null
            property bool completed: false

            function ensureSize() {
                const geometry = shellSurface.windowGeometry;
                if (!mouseTracker.output.windowFollowsSize || (geometry.width > 0 && geometry.height > 0)) {
                    if (shellSurface.toplevel !== null) {
                        shellSurface.toplevel.sendFullscreen(Qt.size(mouseTracker.output.window.width, mouseTracker.output.window.height));
                    } else if (shellSurface.popup !== null) {
                        shellSurface.popup.sendConfigure(Qt.rect(0, 0, mouseTracker.output.window.width, mouseTracker.output.window.height));
                    }
                }
            }

            Connections {
                target: shellSurface

                function onWindowGeometryChanged() {
                    if (mouseTracker.output.windowFollowsSize) {
                        const geometry = shellSurface.windowGeometry;
                        mouseTracker.output.window.width = geometry.width;
                        mouseTracker.output.window.height = geometry.height;
                    }

                    if (!completed && mouseTracker.output.window.width > 0 && mouseTracker.output.window.height > 0) {
                        completed = true;
                        shellSurface.output = mouseTracker.output;
                        mouseTracker.output.surfaceCompleted();
                    }
                }
            }

            Connections {
                target: shellSurface.toplevel

                function onTitleChanged() {
                    mouseTracker.output.window.title = shellSurface.toplevel.title;
                }

                function onFullscreenChanged() {
                    if (!shellSurface.toplevel.fullscreen) {
                        shellSurface.toplevel.sendFullscreen(Qt.size(mouseTracker.output.window.width, mouseTracker.output.window.height));
                    }
                }
            }

            Connections {
                target: mouseTracker.output.window

                function onClosing() {
                    if (shellSurface.toplevel !== null) {
                        shellSurface.toplevel.sendClose();
                    } else if (shellSurface.popup !== null) {
                        shellSurface.popup.sendPopupDone();
                    }
                    mouseTracker.output.window.close.accepted = false;
                }

                function onWidthChanged() {
                    ensureSize();
                }

                function onHeightChanged() {
                    ensureSize();
                }
            }

            MouseArea {
                id: mouseArea
                anchors.fill: parent
                cursorShape: clientCursor.visible ? Qt.BlankCursor : Qt.ArrowCursor
                hoverEnabled: true

                onPressed: (mouse) => {
                    mouse.accepted = false
                }

                WaylandCursorItem {
                    id: clientCursor
                    inputEventsEnabled: false
                    x: mouseTracker.mouseX
                    y: mouseTracker.mouseY
                    visible: surface !== null && mouseArea.containsMouse
                    seat: mouseTracker.output.compositor.defaultSeat
                }
            }
        }
    }
}
