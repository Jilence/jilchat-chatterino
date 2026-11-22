import QtQuick
import QtQuick.Window
import LiquidGlass

Window {
    id: win
    width: Launch.width
    height: Launch.height
    minimumWidth: 360
    minimumHeight: 480
    visible: false // shown from main.cpp after enabling GPU timestamps
    color: "#0e0e10"
    title: "Leafyrino – Liquid Glass (Qt Quick Prototyp)"

    property var channels: Array.from(Launch.channels)
    property int current: 0
    property bool glassOn: Launch.glass
    property bool stress: Launch.stress
    property bool lensOn: Launch.stress
    property bool hudOn: true
    property bool wallpaper: true
    property real frost: 2.0
    property real dispersion: Launch.prism ? 0.14 : 0.0

    function submit() {
        const t = field.text.trim()
        if (t.length === 0)
            return
        if (t.startsWith("/join ")) {
            const c = t.slice(6).trim().toLowerCase().replace(/^#/, "")
            if (c.length > 0) {
                const i = channels.indexOf(c)
                if (i >= 0) {
                    current = i
                } else {
                    channels = channels.concat([c])
                    current = channels.length - 1
                }
            }
        } else {
            chat.sendLocal(t)
            list.follow = true
        }
        field.text = ""
    }

    TwitchChat {
        id: chat
        channel: win.channels[win.current] ?? ""
        flood: win.stress
    }

    FrameStats {
        id: stats
        window: win
    }

    // ---------------------------------------------------------------- content
    // Everything the glass refracts lives in this layer. It is rendered once
    // per frame into a mipmapped texture; every glass surface samples it.
    Item {
        id: chatLayer
        anchors.fill: parent
        layer.enabled: true
        layer.mipmap: true
        layer.smooth: true

        Rectangle {
            anchors.fill: parent
            color: "#0e0e10"
        }

        Canvas {
            id: wall
            anchors.fill: parent
            visible: win.wallpaper
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                function blob(x, y, r, c) {
                    const g = ctx.createRadialGradient(x, y, 0, x, y, r)
                    g.addColorStop(0, c)
                    g.addColorStop(1, "rgba(0,0,0,0)")
                    ctx.fillStyle = g
                    ctx.fillRect(0, 0, width, height)
                }
                blob(width * 0.1, height * 0.15, width * 0.95, "rgba(145,71,255,0.26)")
                blob(width * 1.0, height * 0.55, width * 0.85, "rgba(0,160,255,0.16)")
                blob(width * 0.25, height * 1.0, width * 0.9, "rgba(255,70,150,0.14)")
            }
        }

        ListView {
            id: list
            anchors.fill: parent
            topMargin: 78
            bottomMargin: 86
            model: chat
            reuseItems: true
            boundsBehavior: Flickable.StopAtBounds
            cacheBuffer: 400

            property bool follow: true
            onMovementEnded: follow = atYEnd
            onCountChanged: if (follow) Qt.callLater(positionViewAtEnd)
            onHeightChanged: if (follow) Qt.callLater(positionViewAtEnd)

            delegate: Item {
                id: row
                required property string name
                required property string nameColor
                required property string html
                required property bool system

                width: ListView.view.width
                height: msg.implicitHeight + 7

                Text {
                    id: msg
                    x: 16
                    y: 3
                    width: row.width - 32
                    wrapMode: Text.Wrap
                    textFormat: Text.StyledText
                    font.pixelSize: 14
                    font.family: "Segoe UI"
                    color: row.system ? "#9a9aa8" : "#ececf1"
                    text: row.system
                          ? `<i>${row.html}</i>`
                          : `<font color="${row.nameColor}"><b>${row.name}</b></font>: ${row.html}`
                }
            }
        }
    }

    // ---------------------------------------------------------------- glass
    Item {
        id: overlay
        anchors.fill: parent

        // Tabs, docked at the top like JilChat's channel bar.
        Glass {
            id: header
            source: chatLayer
            glass: win.glassOn
            frost: win.frost
            dispersion: win.dispersion
            x: 10
            y: 10
            width: overlay.width - 20
            height: 54
            bezel: 16
            thickness: 34

            Rectangle {
                id: indicator
                property Item target: tabs.count > win.current ? tabs.itemAt(win.current) : null
                x: target ? tabRow.x + target.x : 0
                width: target ? target.width : 0
                height: 40
                radius: 20
                anchors.verticalCenter: parent.verticalCenter
                color: Qt.rgba(1, 1, 1, 0.14)
                border.color: Qt.rgba(1, 1, 1, 0.16)
                Behavior on x { SpringAnimation { spring: 3.2; damping: 0.26; epsilon: 0.25 } }
                Behavior on width { SpringAnimation { spring: 3.2; damping: 0.26; epsilon: 0.25 } }
            }

            Row {
                id: tabRow
                x: 7
                anchors.verticalCenter: parent.verticalCenter
                Repeater {
                    id: tabs
                    model: win.channels
                    delegate: Item {
                        id: tab
                        required property string modelData
                        required property int index
                        readonly property bool active: index === win.current
                        width: label.implicitWidth + 28
                        height: 40
                        Text {
                            id: label
                            anchors.centerIn: parent
                            text: tab.modelData
                            color: tab.active ? "#ffffff" : "#b8b8c4"
                            font.pixelSize: 14
                            font.family: "Segoe UI"
                            font.weight: tab.active ? Font.DemiBold : Font.Medium
                        }
                        TapHandler { onTapped: win.current = tab.index }
                    }
                }
            }

            Row {
                anchors.right: parent.right
                anchors.rightMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6
                Rectangle {
                    width: 8
                    height: 8
                    radius: 4
                    anchors.verticalCenter: parent.verticalCenter
                    color: chat.connected ? "#3ddc84" : "#ff5f57"
                }
                Text {
                    text: chat.connected ? "live" : "…"
                    color: "#c9c9d2"
                    font.pixelSize: 12
                    font.family: "Segoe UI"
                }
            }
        }

        // Frame timing.
        Glass {
            id: hud
            visible: win.hudOn
            source: chatLayer
            glass: win.glassOn
            frost: win.frost
            x: overlay.width - width - 14
            y: header.y + header.height + 12
            width: hudCol.implicitWidth + 26
            height: hudCol.implicitHeight + 16
            radius: 16
            bezel: 10
            thickness: 18
            shadow: 0.35

            Column {
                id: hudCol
                anchors.centerIn: parent
                spacing: 2
                Text {
                    text: `${stats.fps.toFixed(0)} fps  ${stats.frameMs.toFixed(1)} ms  max ${stats.worstMs.toFixed(1)}`
                    font.family: "Consolas"
                    font.pixelSize: 12
                    color: "white"
                }
                Text {
                    text: `cpu ${stats.renderMs.toFixed(2)} ms  gpu ${stats.gpuMs.toFixed(2)} ms  ${chat.rate.toFixed(0)} msg/s  ${stats.api}`
                    font.family: "Consolas"
                    font.pixelSize: 12
                    color: "#d0d0da"
                }
                Text {
                    text: "^G Glas ^F Frost ^D Prisma ^S Stress ^L Linse ^W Bg ^H"
                    font.family: "Segoe UI"
                    font.pixelSize: 10
                    color: "#9a9aa8"
                }
            }
        }

        // Shown when scrolled up.
        Glass {
            id: jump
            property bool shown: !list.follow
            source: chatLayer
            glass: win.glassOn
            frost: win.frost
            dispersion: win.dispersion
            interactive: true
            width: jumpText.implicitWidth + 34
            height: 36
            bezel: 12
            thickness: 22
            x: (overlay.width - width) / 2
            y: inputBar.y - height - 12
            opacity: shown && !picker.open ? 1 : 0
            visible: opacity > 0.01
            Behavior on opacity { NumberAnimation { duration: 180 } }
            Text {
                id: jumpText
                anchors.centerIn: parent
                text: "Neue Nachrichten  ↓"
                color: "white"
                font.pixelSize: 13
                font.family: "Segoe UI"
                font.weight: Font.Medium
            }
            onClicked: {
                list.follow = true
                list.positionViewAtEnd()
            }
        }

        // Emote picker, a large floating panel.
        Glass {
            id: picker
            property bool open: Launch.picker
            source: chatLayer
            glass: win.glassOn
            frost: win.frost + 0.8
            dispersion: win.dispersion
            x: 10
            width: overlay.width - 20
            height: Math.min(340, overlay.height * 0.45)
            y: emoteBtn.y - height - 12
            radius: 28
            bezel: 24
            thickness: 60
            shadow: 0.7
            transformOrigin: Item.BottomLeft
            scale: open ? 1 : 0.4
            opacity: open ? 1 : 0
            visible: opacity > 0.01
            Behavior on opacity { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

            Text {
                x: 22
                y: 14
                text: `7TV Emotes · ${chat.emotes.length}`
                color: "#e2e2ea"
                font.pixelSize: 13
                font.family: "Segoe UI"
                font.weight: Font.DemiBold
            }
            GridView {
                anchors.fill: parent
                anchors.topMargin: 40
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                anchors.bottomMargin: 16
                clip: true
                cellWidth: 56
                cellHeight: 50
                model: chat.emotes
                delegate: Item {
                    required property var modelData
                    width: 56
                    height: 50
                    Image {
                        anchors.centerIn: parent
                        height: 34
                        width: Math.min(52, 34 * parent.modelData.width / Math.max(1, parent.modelData.height))
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        source: parent.modelData.url
                    }
                    TapHandler {
                        onTapped: {
                            field.insert(field.cursorPosition, parent.modelData.name + " ")
                            field.forceActiveFocus()
                        }
                    }
                }
            }
        }

        Glass {
            id: emoteBtn
            source: chatLayer
            glass: win.glassOn
            frost: win.frost
            dispersion: win.dispersion
            interactive: true
            width: 50
            height: 50
            x: 10
            y: overlay.height - height - 12
            bezel: 14
            thickness: 30
            Text {
                anchors.centerIn: parent
                text: picker.open ? "✕" : "☺"
                color: "white"
                font.pixelSize: picker.open ? 18 : 26
            }
            onClicked: picker.open = !picker.open
        }

        Glass {
            id: inputBar
            source: chatLayer
            glass: win.glassOn
            frost: win.frost
            dispersion: win.dispersion
            x: emoteBtn.x + emoteBtn.width + 8
            y: emoteBtn.y
            width: overlay.width - x - 10
            height: 50
            bezel: 16
            thickness: 34

            TextInput {
                id: field
                anchors.left: parent.left
                anchors.right: send.left
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 20
                anchors.rightMargin: 10
                color: "white"
                font.pixelSize: 15
                font.family: "Segoe UI"
                clip: true
                selectByMouse: true
                selectionColor: "#7c5cff"
                focus: true
                onAccepted: win.submit()
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: field.text.length === 0
                    text: `Nachricht an #${chat.channel}`
                    color: "#8f8f9c"
                    font: field.font
                }
            }

            Rectangle {
                id: send
                width: 38
                height: 38
                radius: 19
                anchors.right: parent.right
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                color: "#9147ff"
                scale: field.text.length > 0 ? 1 : 0
                visible: scale > 0.01
                Behavior on scale { SpringAnimation { spring: 5; damping: 0.32; epsilon: 0.005 } }
                Text {
                    anchors.centerIn: parent
                    text: "↑"
                    color: "white"
                    font.pixelSize: 20
                    font.bold: true
                }
                TapHandler { onTapped: win.submit() }
            }
        }

        // Draggable lens (Ctrl+L): the clearest view of the refraction.
        Glass {
            id: lens
            visible: win.lensOn
            source: chatLayer
            glass: win.glassOn
            dispersion: win.dispersion
            width: 150
            height: 150
            bezel: 46
            thickness: 110
            frost: 0
            zoom: 1.2
            specular: 0.55
            shadow: 0.6
            brightness: 0.02
            tint: Qt.vector4d(1, 1, 1, 0.03)
            x: (overlay.width - width) / 2
            y: overlay.height * 0.4

            property real angle: 0
            NumberAnimation on angle {
                running: win.stress && win.lensOn && !drag.active
                from: 0
                to: Math.PI * 2
                duration: 7000
                loops: Animation.Infinite
            }
            onAngleChanged: {
                if (!win.stress || drag.active)
                    return
                x = (overlay.width - width) / 2 + Math.cos(angle) * overlay.width * 0.24
                y = overlay.height * 0.47 - height / 2 + Math.sin(angle) * overlay.height * 0.2
            }
            DragHandler { id: drag }
        }
    }

    Shortcut { sequence: "Ctrl+G"; onActivated: win.glassOn = !win.glassOn }
    Shortcut { sequence: "Ctrl+F"; onActivated: win.frost = win.frost >= 4 ? 0 : win.frost + 1 }
    Shortcut { sequence: "Ctrl+D"; onActivated: win.dispersion = win.dispersion > 0 ? 0 : 0.14 }
    Shortcut { sequence: "Ctrl+S"; onActivated: { win.stress = !win.stress; if (win.stress) win.lensOn = true } }
    Shortcut { sequence: "Ctrl+L"; onActivated: win.lensOn = !win.lensOn }
    Shortcut { sequence: "Ctrl+H"; onActivated: win.hudOn = !win.hudOn }
    Shortcut { sequence: "Ctrl+W"; onActivated: win.wallpaper = !win.wallpaper }
    Shortcut { sequence: "Esc"; onActivated: picker.open = false }
}
