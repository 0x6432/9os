import QtQuick
Rectangle {
    width: 320
    height: 200
    color: "black"
    property int basis: 7
    readonly property int derived: basis * 6
}
