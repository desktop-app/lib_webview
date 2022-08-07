// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
import QtQuick
import QtWayland.Compositor
import QtWayland.Compositor.XdgShell

WaylandCompositor {
    id: waylandCompositor

    property ListModel toplevels: ListModel {}
    property ListModel popups: ListModel {}

    CompositorScreen {
        id: screen
        compositor: waylandCompositor
        windowFollowsSize: false
        window.objectName: "window"
    }

    Instantiator {
        model: toplevels
        CompositorScreen {
            compositor: waylandCompositor

            Component.onCompleted: shellSurfaces.append({shellSurface: modelData})
            onSurfaceCompleted: window.visible = true

            Connections {
                target: modelData.surface
                function onSurfaceDestroyed() {
                    toplevels.remove(index);
                }
            }
        }
    }

    Instantiator {
        model: popups
        CompositorScreen {
            compositor: waylandCompositor

            Component.onCompleted: shellSurfaces.append({shellSurface: modelData})

            onSurfaceCompleted: {
                // Doesn't really work, see onPopupCreated comment
                // window.transientParent = screen.window.transientParent;
                // window.x = window.transientParent.x + modelData.popup.unconstrainedPosition.x;
                // window.y = window.transientParent.y + modelData.popup.unconstrainedPosition.y;
                window.flags = Qt.Popup;
                window.visible = true;
            }

            Connections {
                target: modelData.surface
                function onSurfaceDestroyed() {
                    popups.remove(index);
                }
            }
        }
    }

    XdgShell {
        onToplevelCreated: (toplevel, xdgSurface) => {
            if (screen.shellSurfaces.count > 0) {
                toplevels.append({shellSurface: xdgSurface});
            } else {
                screen.shellSurfaces.append({shellSurface: xdgSurface});
            }
        }

        // Can't find a way to find output (and host window therefore) by surface
        // onPopupCreated: (toplevel, xdgSurface) => {
        //     popups.append({shellSurface: xdgSurface});
        // }
    }
}
