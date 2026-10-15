import QtQuick
import QtQuick.Window
Window {
    id: root
    width: 128; height: 96; visible: true
    title: "9os-alpine-wayland"
    property int frames: 0
    property int phase: 0
    property bool ready: false
    Rectangle {
        anchors.fill: parent
        color: root.phase === 0 ? "#cc3311" : root.phase === 1 ? "#2244cc" : "#11aa55"
    }
    onFrameSwapped: frames++
    Timer {
        interval: 150; running: true; repeat: true
        onTriggered: {
            if (root.phase < 2) { root.phase++; return }
            if (!root.ready && root.frames >= 3) {
                root.ready = true
                console.log("alpine-wayland: READY frames", root.frames, "size", root.width, root.height)
                exitTimer.start()
            }
        }
    }
    // Keep the final surface alive while the harness verifies Sway IPC/capture.
    Timer { id: exitTimer; interval: 8000; onTriggered: Qt.quit() }
    Timer { interval: 15000; running: true; onTriggered: { console.log("alpine-wayland: FAIL timeout"); Qt.exit(1) } }
}
