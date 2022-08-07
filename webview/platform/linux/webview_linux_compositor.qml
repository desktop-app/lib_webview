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

    Loader {
        id: screenLoader
        active: typeof widgetWindow !== "undefined" && widgetWindow !== null
        sourceComponent: CompositorScreen {
            objectName: "mainOutput"
            compositor: waylandCompositor
            window: widgetWindow
            windowFollowsSize: false
        }
    }

    Instantiator {
        model: toplevels
        CompositorScreen {
            compositor: waylandCompositor
            shellSurface: modelData
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
            shellSurface: modelData

            onSurfaceCompleted: {
                const parent = modelData.popup.parentXdgSurface.output.window;
                window.x = modelData.popup.unconstrainedPosition.x;
                window.y = modelData.popup.unconstrainedPosition.y;
                if (parent === widgetWindow) {
                    const geometry = bridge.widgetGlobalGeometry(widget);
                    window.transientParent = widgetWindow.transientParent;
                    window.x += geometry.x;
                    window.y += geometry.y;
                } else {
                    window.transientParent = parent;
                    window.x += window.transientParent.x;
                    window.y += window.transientParent.y;
                }
                window.flags = Qt.Popup;
                window.color = "transparent";
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
            if (screenLoader.item.shellSurface !== null) {
                toplevels.append({shellSurface: xdgSurface});
            } else {
                screenLoader.item.shellSurface = xdgSurface;
            }
        }

        onPopupCreated: (toplevel, xdgSurface) => {
            popups.append({shellSurface: xdgSurface});
        }
    }
}
