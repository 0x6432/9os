import QtQuick
import QtQuick.Window
Window {
    id: root
    width: 96; height: 64; visible: true
    color: "black"
    property int frames: 0
    property int base: 7
    property int answer: base * 6
    property bool grabbed: false
    Rectangle { id: tile; anchors.fill: parent; color: "#cc3311" }
    onFrameSwapped: frames++
    Component.onCompleted: {
        if (answer !== 42 || JSON.parse('{"n":9}').n !== 9) Qt.exit(1)
        base = 8
        console.log("qtquick: loaded Qt Quick")
    }
    Timer {
        interval: 300; running: true; repeat: true
        onTriggered: {
            if (root.answer !== 48) { console.log("qtquick: FAIL binding"); Qt.exit(2) }
            if (root.frames > 0 && !root.grabbed) {
                root.grabbed = true
                if (!tile.grabToImage(function(result) {
                    if (result.url.toString() === "") { console.log("qtquick: FAIL grab"); Qt.exit(3) }
                    if (!result.saveToFile("capture.png")) { console.log("qtquick: FAIL save"); Qt.exit(6) }
                    console.log("qtquick: PASSED rendered frames", root.frames, "grab", result.url)
                    Qt.quit()
                })) { console.log("qtquick: FAIL grab request"); Qt.exit(4) }
            }
        }
    }
    Timer { interval: 10000; running: true; onTriggered: { console.log("qtquick: FAIL timeout"); Qt.exit(5) } }
}
