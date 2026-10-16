import QtQuick
import QtQuick.Window
// Construct the scene graph before Component.onCompleted starts the image
// reader. A bare Item lets qmlscene create its implicit QQuickView afterwards,
// racing the reader's backend/texture-factory initialization in Qt 6.8.3.
Window {
    visible: true
    width: 320; height: 240; color: "#000000"
    property int completed: 0
    property int target: 32
    Image {
        id: picture
        anchors.fill: parent
        asynchronous: true
        cache: false
        fillMode: Image.PreserveAspectCrop
        onStatusChanged: {
            if (status === Image.Error) {
                console.log("ASYNC_IMAGE_FAIL decode " + source)
                Qt.quit()
            }
            if (status === Image.Ready) {
                completed++
                console.log("ASYNC_IMAGE_READY " + completed + " " + sourceSize.width + "x" + sourceSize.height)
                if (completed === target) {
                    grabToImage(function(result) {
                        if (!result.saveToFile("capture.png")) {
                            console.log("ASYNC_IMAGE_FAIL capture")
                        } else {
                            console.log("ASYNC_IMAGE_PASS loads=" + completed)
                        }
                        Qt.quit()
                    })
                } else {
                    Qt.callLater(function() { picture.source = ""; Qt.callLater(reload) })
                }
            }
        }
    }
    function reload() {
        picture.source = "file:///usr/share/wallpapers/Next/contents/images/1024x768.png"
    }
    Component.onCompleted: reload()
    Timer {
        interval: 60000; running: true
        onTriggered: { console.log("ASYNC_IMAGE_FAIL deadline completed=" + completed); Qt.quit() }
    }
}
