import QtQuick

// A Liquid Glass surface. `source` is the item it refracts (must have
// layer.enabled, ideally layer.mipmap). Children are drawn on top of the glass.
Item {
    id: root

    property Item source
    property bool glass: true

    property real radius: Math.min(width, height) / 2
    property real bezel: 18
    property real thickness: 40
    property real frost: 1.0
    property real dispersion: 0
    property real specular: 0.4
    property real saturation: 1.6
    property real brightness: 0.035
    // Dark mode: a little dimming keeps text on busy chat readable.
    property vector4d tint: Qt.vector4d(0.06, 0.06, 0.08, 0.24)
    property real shadow: 0.55
    property real zoom: 1.0

    // Pressable glass: springs up and brightens while held.
    property bool interactive: false
    signal clicked()

    default property alias content: inner.data
    readonly property real pad: 30

    scale: interactive && tap.pressed ? 1.07 : 1.0
    Behavior on scale { SpringAnimation { spring: 5; damping: 0.32; epsilon: 0.001 } }

    property real press: interactive && tap.pressed ? 1 : 0
    Behavior on press { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

    // Where our quad (glass + shadow padding) sits inside the source, in uv.
    // Uses the parent mapping on purpose: our own scale then magnifies the
    // content under the glass instead of following it.
    property rect _src: Qt.rect(0, 0, 1, 1)
    function sync() {
        if (!source || !parent || source.width <= 0 || source.height <= 0)
            return
        const p = parent.mapToItem(source, x - pad, y - pad)
        _src = Qt.rect(p.x / source.width, p.y / source.height,
                       (width + 2 * pad) / source.width, (height + 2 * pad) / source.height)
    }
    onXChanged: sync()
    onYChanged: sync()
    onWidthChanged: sync()
    onHeightChanged: sync()
    onSourceChanged: sync()
    Component.onCompleted: sync()
    Connections {
        target: root.source
        function onWidthChanged() { root.sync() }
        function onHeightChanged() { root.sync() }
    }

    ShaderEffect {
        id: fx
        visible: root.glass && root.source !== null
        x: -root.pad
        y: -root.pad
        width: root.width + 2 * root.pad
        height: root.height + 2 * root.pad

        property variant src: root.source
        property vector4d srcRect: Qt.vector4d(root._src.x, root._src.y, root._src.width, root._src.height)
        property vector2d size: Qt.vector2d(width, height)
        property vector4d box: Qt.vector4d(root.pad, root.pad, root.width, root.height)
        property real radius: root.radius
        property real bezel: root.bezel
        property real thickness: root.thickness
        property real frost: root.frost
        property real dispersion: root.dispersion
        property real specular: root.specular
        property real saturation: root.saturation
        property real brightness: root.brightness
        property vector4d tint: root.tint
        property real shadow: root.shadow
        property real press: root.press
        property real zoom: root.zoom
        property real flipY: 0

        vertexShader: "shaders/glass.vert.qsb"
        fragmentShader: "shaders/glass.frag.qsb"
    }

    // Flat fallback (Ctrl+G) to compare look and cost.
    Rectangle {
        visible: !root.glass
        anchors.fill: parent
        radius: root.radius
        color: "#ee1d1d22"
        border.color: "#2effffff"
    }

    Item {
        id: inner
        anchors.fill: parent
    }

    TapHandler {
        id: tap
        enabled: root.interactive
        onTapped: root.clicked()
    }
}
