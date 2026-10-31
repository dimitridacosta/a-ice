import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// POC a-ice 2.0 — overlay Qt Quick, glassmorphism KWin, ListView virtualisé + fade shader.
Window {
    id: root

    readonly property color glass: Qt.rgba(0.10, 0.10, 0.14, 0.55)
    readonly property color glassLight: Qt.rgba(0.42, 0.44, 0.55, 0.45)
    readonly property color textColor: "#e7e9f0"
    readonly property color accent: "#7d9cff"

    flags: Qt.FramelessWindowHint | Qt.Window
    color: "transparent"
    // visible après helper.attach() : le layer shell doit être configuré avant le show
    width: helper.windowWidth
    height: helper.windowHeight > 0 ? helper.windowHeight : Screen.height
    minimumHeight: 320

    Component.onCompleted: {
        helper.attach(root)
        root.visible = true
    }

    // Fond vitré plein écran (blur derrière assuré côté KWin).
    Rectangle {
        id: backdrop
        anchors.fill: parent
        radius: 18
        color: root.glass
        border.color: Qt.rgba(1, 1, 1, 0.08)
        border.width: 1

        ColumnLayout {
            id: layout
            anchors.fill: parent
            anchors.margins: 16
            spacing: 14

            // ---- Zone de conversation : ListView virtualisé, 2000 bulles, fade shader ----
            Item {
                id: chatZone
                Layout.fillWidth: true
                Layout.fillHeight: true

                ListView {
                    id: chatList
                    anchors.fill: parent
                    model: chatModel
                    spacing: 10
                    clip: true
                    reuseItems: true
                    cacheBuffer: 600 // px de prérendu au-delà du viewport
                    flickDeceleration: 6000
                    maximumFlickVelocity: 8000

                    ScrollBar.vertical: ScrollBar {
                        width: 8
                        policy: ScrollBar.AsNeeded
                    }

                    // Stress test continu (AICE_STRESS=1) pour mesurer le frame time.
                    // FrameAnimation : reçoit un tick à chaque frame (vsync) — mesure vraie.
                    FrameAnimation {
                        running: helper.stress && chatList.contentHeight > chatList.height
                        onTriggered: {
                            let end = chatList.contentHeight - chatList.height
                            chatList.contentY = (chatList.contentY + 16) % Math.max(end, 1)
                        }
                    }

                    // Génération des 2000 bulles de test.
                    ListModel {
                        id: chatModel
                        Component.onCompleted: {
                            const phrases = [
                                "Voici le POC Qt Quick de la refonte 2.0.",
                                "Chaque bulle est recyclée : mémoire plate, 60 → 120 fps visés.",
                                "Le fade haut/bas est un ShaderEffect GPU sur la couche de la liste.",
                                "Le blur derrière la fenêtre vient de KWin (KWindowEffects).",
                                "Barre déplaçable n'importe où via le geste système Wayland.",
                                "Streaming token par token rendu trivial par le modèle de bulles.",
                                "Frame time mesuré sur frameSwapped — log stderr toutes les 600 frames.",
                                "Le vitrage ne freine plus la conversation : 2000 bulles sans freeze."
                            ]
                            for (let i = 0; i < 2000; ++i) {
                                chatModel.append({
                                    text: phrases[i % phrases.length] + " (#" + (i + 1) + ")",
                                    mine: (i % 3) === 1
                                })
                            }
                        }
                    }

                    delegate: Rectangle {
                        required property string text
                        required property bool mine
                        required property int index

                        width: Math.min(chatZone.width * 0.72,
                                        content.implicitWidth + 30)
                        height: content.implicitHeight + 22
                        radius: 14
                        color: mine ? root.glassLight : root.glass
                        border.color: Qt.rgba(1, 1, 1, mine ? 0.16 : 0.08)
                        border.width: 1
                        anchors.right: mine ? parent.right : undefined
                        anchors.left: mine ? undefined : parent.left

                        Text {
                            id: content
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.margins: 15
                            text: parent.text
                            color: parent.mine ? root.accent : root.textColor
                            font.pointSize: 11
                            wrapMode: Text.WordWrap
                            textFormat: Text.PlainText
                        }
                    }
                }

                // Fade-out haut/bas : la liste est rendue en couche, le shader abaisse
                // l'alpha (prémultiplié) près des bords. 100 % GPU.
                ShaderEffect {
                    id: fadeEffect
                    anchors.fill: parent
                    property ShaderEffectSource source: ShaderEffectSource {
                        sourceItem: chatList
                        live: true
                        recursive: false
                        hideSource: true
                    }
                    property real fadeTop: 60 // px
                    property real fadeBottom: 60
                    property real pxPerUnit: Math.max(1, chatZone.height)

                    fragmentShader: "qrc:/poc/shaders/fade.frag.qsb"
                }
            }

            // ---- Barre de prompt flottante, déplacement système ----
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 64

                Rectangle {
                    id: promptBar
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(parent.width * 0.8, 620)
                    height: 56
                    radius: 28
                    color: Qt.rgba(0.16, 0.17, 0.23, 0.75)
                    border.color: focusRing.activeFocus ? Qt.rgba(0.49, 0.61, 1.0, 0.8)
                                                        : Qt.rgba(1, 1, 1, 0.14)
                    border.width: focusRing.activeFocus ? 1.5 : 1

                    // Poignée de déplacement : toute la barre répond sauf la zone de texte.
                    MouseArea {
                        anchors.fill: parent
                        onPressed: helper.startSystemMove(root) // geste système natif
                        cursorShape: Qt.SizeAllCursor
                        z: -1
                    }

                    // Grip explicite au-dessus de tout.
                    MouseArea {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 24
                        onPressed: helper.startSystemMove(root)
                        cursorShape: Qt.SizeAllCursor
                        Text { anchors.centerIn: parent; text: "⠿"; color: Qt.rgba(1, 1, 1, 0.4) }
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 22
                        anchors.rightMargin: 10
                        spacing: 8

                        TextEdit {
                            id: focusRing
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            text: ""
                            color: root.textColor
                            font.pointSize: 11.5
                            activeFocusOnPress: true
                            wrapMode: TextEdit.NoWrap
                            Text {
                                anchors.fill: parent
                                visible: focusRing.text === "" && !focusRing.activeFocus
                                text: "Pose ta question…  (barre déplaçable en saisissant la barre)"
                                color: Qt.rgba(1, 1, 1, 0.35)
                                font: focusRing.font
                            }
                            Keys.onReturnPressed: (event) => {
                                if (event.modifiers === Qt.NoModifier && text.length > 0) {
                                    chatModel.append({ text: text, mine: true })
                                    chatModel.append({
                                        text: "(POC — le hub ACP branchera le vrai moteur)",
                                        mine: false
                                    })
                                    text = ""
                                    chatList.positionViewAtEnd()
                                }
                            }
                        }

                        Rectangle {
                            Layout.alignment: Qt.AlignVCenter
                            width: 36
                            height: 36
                            radius: 18
                            color: root.accent
                            Text {
                                anchors.centerIn: parent
                                text: "→"
                                color: "white"
                                font.pointSize: 13
                            }
                        }
                    }
                }
            }

            // Bandeau infos (POC) : fps, hint mode layer.
            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 18
                Text {
                    text: helper.frameMs > 0
                          ? Math.round(1000 / helper.frameMs) + " fps · "
                            + helper.frameMs.toFixed(2) + " ms/frame"
                          : "frame time — mesure en cours…"
                    color: Qt.rgba(1, 1, 1, 0.4)
                    font.pointSize: 9
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: "2000 bulles · AICE_LAYER=1 → overlay layer-shell"
                    color: Qt.rgba(1, 1, 1, 0.25)
                    font.pointSize: 9
                }
            }
        }
    }
}