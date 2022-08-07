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

    property ShellSurface shellSurface
    property bool windowFollowsSize: true
    signal surfaceCompleted()

    window: Window {
        Chrome {
            output: screen
        }
    }
}
