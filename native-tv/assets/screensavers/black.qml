/*
 * Black screen saver.
 *
 * Mounted over com.webos.app.screensaver like the others, so it starts and is
 * dismissed the way the platform's own is. Nothing is drawn: on an OLED the
 * pixels are off.
 */
import QtQuick 2.4
import Eos.Window 0.1

WebOSWindow {
    // Sized for the compositor's 1920x1080 layout rather than from Screen,
    // which can report smaller.
    width: 1920
    height: 1080

    windowType: "_WEBOS_WINDOW_TYPE_SCREENSAVER"
    appId: "com.webos.app.screensaver"
    title: "Screen Saver"
    visible: true
    color: "black"
}
