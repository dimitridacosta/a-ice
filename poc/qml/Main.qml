import QtQuick
import QtQuick.Window
import QtQuick.Effects

/*
 * a-ice 2.0 — M1 : le corps en Qt Quick, version testable.
 *
 * On reproduit fidelement l'identite visuelle d'a-ice telle qu'on la connait :
 *   - aucune fenetre visible : seul le verre existe (carte par carte),
 *   - des bulles flottees separees (carte agent pleine largeur, carte user
 *     plus petite alignee a droite), chacune avec sa zone de flou propre,
 *   - une barre de prompt en pilule en bas, ATTRAPABLE : on la saisit et c'est
 *     toute la fenetre qui bouge.
 *
 * Tout ce qui est mesure ici (flou, deplacement, liste virtualisee, fade) est
 * ce qu'on validait avant d'engager la refonte : voir docs/v2-rewrite/PLAN.md.
 * Depuis M2, les reponses ne sont plus simulees : elles viennent du hub
 * zcode-acp (voir HubClient.cpp), et le corps reste fidele a l'identite v1.
 */
Window {
    id: win

    // --- reglages du verre (teintes et rayons repris de la v1) ---
    readonly property int cardRadius: 16
    readonly property int barRadius: 22
    readonly property int hudRadius: 12
    readonly property int inset: 8          // marge autour des cartes dans la zone
    readonly property int halo: 12          // place laissee a l'ombre autour de la carte
    readonly property int fadeBand: 90      // hauteur du fade en haut/bas de la zone
    readonly property color userCard: "#5A3478DA"   // rgba(52,120,218,90)
    readonly property color agentCard: "#64222228"  // rgba(34,34,40,100)
    readonly property color cardBorder: "#24FFFFFF" // rgba(255,255,255,36)
    readonly property color barFill: "#6E222228"    // rgba(34,34,40,110)
    readonly property color barBorder: "#28FFFFFF"  // rgba(255,255,255,40)
    readonly property color ink: "#e9eaee"

    // Reglages modifies a la volee depuis le badge (mesure du cout GPU).
    property bool shadowOn: glass.shadow   // AICE_SHADOW=0 pour mesurer sans ombre
    property bool fadeOn: true

    width: pocSize
    height: pocSize
    visible: false
    color: "transparent"
    flags: Qt.Window | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool

    /*
     * Ombre de carte : l'effet d'ombre fourni par Qt, pose sur la matiere de la
     * carte (le fond, pas son contenu). Qt la construit a partir de ce que contient
     * le layer — donc de la forme et de ses coins arrondis — et laisse automatiquement
     * la place au degrade autour (autoPaddingEnabled).
     *
     * Le layer ne contient QUE la matiere : si le texte y entrait, chaque glyphe
     * heriterait de sa propre ombre (c'est ce qui se passait au debut).
     *
     * Le layer n'est active que si l'ombre est active : le lien « ombre » du badge
     * mesure ainsi le cout reel (un rendu en deux passes par carte).
     */
    component CardShadow: MultiEffect {
        property real intensity: 0.6      // 0 = pas d'ombre : moins opaque, l'ombre est de l'air
        property real softness: 0.85      // epaisseur du degrade (fraction du flou max)
        property int maxBlur: 50          // 42 px de halo : l'ombre monte plus haut, plus diffuse
        property int drop: 14             // devers vers le bas : la carte flotte
        /*
         * Ombre NOIRE, pas bleue. L'ombre est dessinee DERRIERE la carte, qui est
         * translucide : une ombre bleue se voit donc a travers la carte, et toute
         * la carte parait bleutee — c'est ce qu'on voyait, bien plus que l'ombre
         * elle-meme. Le noir laisse le verre garder sa teinte a lui.
         */
        property color tint: Qt.rgba(0, 0, 0, 1.0)

        shadowEnabled: intensity > 0
        shadowColor: tint
        shadowOpacity: intensity
        shadowBlur: softness
        blurMax: maxBlur
        shadowVerticalOffset: drop
        shadowHorizontalOffset: 0
        shadowScale: 1.0
        autoPaddingEnabled: true
    }

    // --- les bulles de conversation ---
    ListModel { id: bubbleModel }

    // La bulle agent en cours de remplissage (index dans le modele, -1 sinon).
    property int streamRow: -1

    /* ---------------------------------------------------------------- bulles */

    ListView {
        id: bubblesView
        anchors.top: parent.top
        anchors.bottom: pill.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: win.inset
        anchors.rightMargin: win.inset
        anchors.bottomMargin: 6

        model: bubbleModel
        spacing: 6
        cacheBuffer: 600
        boundsBehavior: Flick.StopAtBounds
        focus: false

        // Le modele est range du plus recent au plus vieux : index 0 = la bulle la
        // plus recente, calee en bas de la zone juste au-dessus de la barre, les
        // anciennes s'empilant au-dessus. C'est la seule facon d'ancrer les bulles
        // en bas sans tricher : un espaceur vide place au-dessus de la liste a
        // bloque la realisation des bulles par la vue, donc leur presence dans
        // le verre.
        verticalLayoutDirection: ListView.BottomToTop

        // Cote « plus recent » d'une vue inverseee = contentYMax (verifie : la vue
        // donne contentY = -hauteur de la zone quand la plus recente est visible).
        readonly property bool atNewest: contentY >= contentYMax - 4
        function followNewest() {
            if (atNewest)
                contentY = contentYMax;
        }

        onContentHeightChanged: glass.scheduleRefresh()
        onHeightChanged: glass.scheduleRefresh()
        onContentYChanged: glass.scheduleRefresh()
        onWidthChanged: glass.scheduleRefresh()

        delegate: Item {
            id: cell

            required property int index
            required property string role
            required property string text

            readonly property bool agent: role === "agent"

            width: bubblesView.width
            height: card.height

            /*
             * Fade par carte (pas de shader, pas de texture hors ecran) : une bulle
             * qui sort de la zone de conversation s'eteint.
             *
             * La position d'une bulle se lit dans les chiffres de la vue elle-meme :
             * y est une coordonnee de contenu (elle est negative dans une vue
             * inverseee) et le defilement est contentY (negative aussi). Mesure a
             * l'appui, la position a l'ecran est y - contentY — y compris a 2000
             * bulles, ou le contenu fait 173 992 pixels.
             *
             * Le fade suit ce qui sort de la zone : une bulle entierement dans la
             * zone reste a 1, et elle s'eteint sur les 90 premiers pixels sortis.
             * Ce n'est pas une bande fixe posee sur la liste : chaque carte est
             * evaluee pour elle-meme.
             */
            readonly property real zoneTopIn: cell.y - bubblesView.contentY
            readonly property real outside: Math.max(0.0, bubblesView.y - zoneTopIn)
                                            + Math.max(0.0, zoneTopIn + cell.height
                                                       - bubblesView.y - bubblesView.height)

            opacity: !win.fadeOn ? 1.0
                : Math.max(0.0, 1.0 - outside / Math.min(Math.max(cell.height, 1), win.fadeBand))

            onYChanged: glass.scheduleRefresh()
            onHeightChanged: glass.scheduleRefresh()
            onVisibleChanged: glass.scheduleRefresh()

            Item {
                id: card

                // C'est cette carte (et pas la fenetre) qui definit le verre :
                // flou KWin + region de clic.
                property bool glassCard: true
                property int glassRadius: win.cardRadius

                // Carte agent : pleine largeur. Carte user : épouse son texte.
                width: cell.agent ? (cell.width - 2 * win.halo)
                                  : Math.min(cell.width - 2 * win.halo, natural.width + 34)
                height: body.implicitHeight + 26
                x: cell.agent ? win.halo : (cell.width - win.halo - width)

                // La matiere de la carte, unique source de l'ombre. L'effet de Qt
                // fabrique l'ombre a partir de ce que le layer contient : en laissant
                // le texte hors du layer, chaque glyphe ne herite pas d'une ombre a lui.
                Rectangle {
                    id: cardFill

                    anchors.fill: parent
                    radius: win.cardRadius
                    color: cell.agent ? win.agentCard : win.userCard
                    border.color: win.cardBorder
                    border.width: 1

                    layer.enabled: win.shadowOn
                    layer.samples: 4
                    layer.effect: CardShadow {}
                }

                Text {
                    id: body
                    x: 16
                    y: 13
                    width: parent.width - 32
                    text: cell.text
                    color: win.ink
                    font.pixelSize: 13
                    textFormat: Text.PlainText
                    wrapMode: Text.WordWrap
                    lineHeight: 1.25
                }

                // Deuxieme passe de mesure : la largeur naturelle du texte, pour
                // que la carte user colle a son contenu (comme la bulle v1).
                Text {
                    id: natural
                    visible: false
                    text: cell.text
                    color: win.ink
                    font.pixelSize: 13
                    textFormat: Text.PlainText
                }
            }
        }
    }

    /* ------------------------------------------------ barre de prompt (grab) */

    Item {
        id: pill

        property bool glassCard: true
        property int glassRadius: win.barRadius

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 20
        anchors.rightMargin: 20
        anchors.bottomMargin: 18

        height: Math.max(52, promptArea.implicitHeight + 24)

        onYChanged: glass.scheduleRefresh()
        onHeightChanged: glass.scheduleRefresh()

        // La matiere de la barre, seule source de l'ombre : le texte saisi et le
        // bouton ne doivent pas heriter d'une ombre chacun.
        Rectangle {
            id: pillFill

            anchors.fill: parent
            radius: win.barRadius
            color: win.barFill
            border.color: win.barBorder
            border.width: 1

            layer.enabled: win.shadowOn
            layer.samples: 4
            layer.effect: CardShadow { drop: 18 }
        }

        // Saisie de la fenetre : le geste natif du composeur si il le sait faire,
        // sinon deplacement par deltas successifs (fallback composeurs prudents).
        MouseArea {
            id: pillDrag
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            cursorShape: Qt.SizeAllCursor

            property bool nativeMove: false
            property real lastX: 0
            property real lastY: 0

            onPressed: {
                lastX = mouse.x
                lastY = mouse.y
                nativeMove = glass.startMove()
            }
            onPositionChanged: {
                if (nativeMove)
                    return;
                win.setX(win.x + (mouse.x - lastX))
                win.setY(win.y + (mouse.y - lastY))
                lastX = mouse.x
                lastY = mouse.y
            }
            onReleased: nativeMove = false
        }

        Item {
            id: pillRow
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 10
            anchors.topMargin: 8
            anchors.bottomMargin: 8

            // Poignee visible : ce qui se voit se saisit.
            Item {
                id: grip
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 18
                Text {
                    anchors.centerIn: parent
                    text: "\u2806"
                    color: "#70e9eaee"
                    font.pixelSize: 15
                }
            }

            Rectangle {
                id: sendBtn
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 34
                height: 34
                radius: 17
                color: hub.ready ? (hub.generating ? "#D2DA4453" : "#C83478DA") : "#60555F6E"
                Text {
                    anchors.centerIn: parent
                    text: hub.generating ? "\u25a0" : "\u2191"
                    color: "#ffffff"
                    font.pixelSize: 16
                    font.bold: true
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    // Le meme bouton sert de ciseau pendant un tour.
                    onClicked: hub.generating ? hub.cancelTurn() : win.send()
                }
            }

            TextEdit {
                id: promptArea
                anchors.left: grip.right
                anchors.right: sendBtn.left
                anchors.verticalCenter: parent.verticalCenter
                text: ""
                readonly property string hint: "Ask A-Ice\u2026"
                color: "#f0f1f3"
                font.pixelSize: 14
                wrapMode: TextEdit.Wrap
                // TextEdit (Qt Quick de base) n'a pas de signal accepted :
                // l'entrée est traitee a la main, et shift+entrée garde son
                // comportement (saut de ligne dans le prompt).
                Keys.onReturnPressed: (event) => {
                    if (event.modifiers & Qt.ShiftModifier)
                        return;
                    win.send();
                    event.accepted = true;
                }
                Keys.onEscapePressed: Qt.quit()
                Keys.onPressed: (event) => {
                    if (event.key === Qt.Key_Q && (event.modifiers & Qt.ControlModifier)) {
                        Qt.quit();
                        event.accepted = true;
                    }
                }
            }

            // Le TextEdit de Qt Quick n'a pas de placeholder : on le dessine
            // nous-memes pour garder le ton du verre (comme en v1).
            Text {
                anchors.fill: promptArea
                visible: !promptArea.text.length
                text: promptArea.hint
                color: "#70e9eaee"
                font.pixelSize: 14
                wrapMode: Text.WordWrap
            }
        }
    }

    /* --------------------------------------------- carte d'autorisation (M2) */

    /*
     * Le pont retransmet `session/request_permission` : la demande du moteur
     * devient une carte de verre au-dessus de la barre, une option = un bouton.
     * C'est le seul endroit de l'UI ou le corps decide de laisser un outil agir.
     */
    Item {
        id: permCard

        property bool glassCard: true
        property int glassRadius: win.cardRadius

        visible: hub.permissionPending

        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: pill.top
        anchors.bottomMargin: 16

        width: Math.min(win.width - 2 * win.inset, 470)
        height: permCol.height + 26

        onYChanged: glass.scheduleRefresh()
        onHeightChanged: glass.scheduleRefresh()
        onVisibleChanged: glass.scheduleRefresh()

        Rectangle {
            id: permFill

            anchors.fill: parent
            radius: win.cardRadius
            color: "#8A2A1E22"
            border.color: "#40FFB37A"
            border.width: 1

            layer.enabled: win.shadowOn
            layer.samples: 4
            layer.effect: CardShadow { intensity: 0.5; drop: 12 }
        }

        Column {
            id: permCol
            x: 16
            y: 13
            spacing: 7
            width: permCard.width - 32

            Text {
                width: permCol.width
                text: "le moteur demande : " + hub.permissionTool
                color: win.ink
                font.pixelSize: 13
                font.bold: true
            }
            Text {
                width: permCol.width
                text: hub.permissionDetail
                color: "#A8e9eaee"
                font.pixelSize: 11
                elide: Text.ElideRight
            }

            Row {
                spacing: 8
                Repeater {
                    model: hub.permissionOptions
                    delegate: Rectangle {
                        required property var modelData

                        width: optionLabel.width + 22
                        height: 28
                        radius: 14
                        color: modelData.kind === "reject" ? "#4AD24453" : "#5A3478DA"
                        border.color: "#30FFFFFF"
                        border.width: 1

                        Text {
                            id: optionLabel
                            anchors.centerIn: parent
                            text: (modelData.name && modelData.name.length)
                                      ? modelData.name : modelData.optionId
                            color: win.ink
                            font.pixelSize: 12
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: hub.respondPermission(modelData.optionId)
                        }
                    }
                }
            }
        }
    }

    /* -------------------------------------------------- carte du hub (M2) */

    Item {
        id: hubCard

        property bool glassCard: true
        property int glassRadius: win.hudRadius

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.topMargin: 20
        anchors.leftMargin: 20

        width: hubCol.implicitWidth + 20
        height: hubCol.implicitHeight + 14

        onXChanged: glass.scheduleRefresh()
        onYChanged: glass.scheduleRefresh()
        onHeightChanged: glass.scheduleRefresh()
        onWidthChanged: glass.scheduleRefresh()

        Rectangle {
            id: hubFill

            anchors.fill: parent
            radius: win.hudRadius
            color: "#3C222228"
            border.color: "#1AFFFFFF"
            border.width: 1

            layer.enabled: win.shadowOn
            layer.samples: 4
            layer.effect: CardShadow { intensity: 0.5; drop: 12 }
        }

        Column {
            id: hubCol
            x: 10
            y: 7
            spacing: 3

            Text {
                text: "hub " + hub.status
                color: hub.ready ? win.ink : "#B8E8A2A8"
                font.pixelSize: 11
            }
            Text {
                visible: hub.toolLine.length > 0
                text: "outil " + hub.toolLine
                color: "#90e9eaee"
                font.pixelSize: 11
            }
            Row {
                spacing: 10
                Text {
                    text: hub.ready ? "couper" : "rejoindre"
                    color: "#80e9eaee"
                    font.pixelSize: 11
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: hub.ready ? hub.disconnectHub() : hub.connectHub()
                    }
                }
            }
        }
    }

    /* ------------------------------------------------- badge de mesure (HUD) */

    Item {
        id: hud

        property bool glassCard: true
        property int glassRadius: win.hudRadius

        visible: glass.hud
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.rightMargin: 20
        anchors.topMargin: 20

        width: hudCol.implicitWidth + 20
        height: hudCol.implicitHeight + 14

        Rectangle {
            id: hudFill

            anchors.fill: parent
            radius: win.hudRadius
            color: "#3C222228"
            border.color: "#1AFFFFFF"
            border.width: 1

            layer.enabled: win.shadowOn
            layer.samples: 4
            layer.effect: CardShadow { intensity: 0.5; drop: 12 }
        }

        Column {
            id: hudCol
            x: 10
            y: 7
            spacing: 3

            Text {
                text: (glass.frames > 1 ? "frame " + glass.frameMs.toFixed(2) + " ms · pic "
                                        + glass.frameMax.toFixed(2) + " ms" : "frame ms : ...")
                      + " · " + bubbleModel.count + " bulles · " + glass.cards + " cartes"
                color: win.ink
                font.pixelSize: 11
            }

            Row {
                spacing: 10

                Text {
                    text: "ombre " + (win.shadowOn ? "o" : "non")
                    color: win.shadowOn ? "#CC" + win.ink.toString().slice(1) : "#59e9eaee"
                    font.pixelSize: 11
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: win.shadowOn = !win.shadowOn
                    }
                }
                Text {
                    text: "fade " + (win.fadeOn ? "o" : "non")
                    color: win.fadeOn ? "#CC" + win.ink.toString().slice(1) : "#59e9eaee"
                    font.pixelSize: 11
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: win.fadeOn = !win.fadeOn
                    }
                }
                Text {
                    text: "fenetre " + (glass.moveNative ? "natif" : "par deltas")
                    color: "#80e9eaee"
                    font.pixelSize: 11
                }
            }
        }
    }

    /* ------------------------------------------------------------- demarrage */

    /*
     * Bouchon de mesure : AICE_STRESS=1 pousse 2000 bulles factices UNIQUEMENT
     * pour mesurer le frame time et la virtualisation (voir le brief M1). Ce
     * n'est pas une conversation : la conversation vient du hub.
     */
    function stressSeed(count) {
        const lines = [
            "Le flou est pose carte par carte, la fenetre reste translucide.",
            "Attrape la barre en bas : c'est toute la fenetre qui bouge.",
            "Une bulle sortie de la zone ne floute plus et ne capte plus les clics.",
            "La liste est virtualisee : seules les bulles affichees existent.",
            "Le fade se fait par carte, en haut et en bas de la zone."
        ];
        for (let i = 0; i < count; ++i) {
            let text = lines[i % lines.length];
            // Quelques bulles volontairement longues : wrap, hauteur variable, fade.
            const repeat = (i % 7) + 1;
            for (let k = 1; k < repeat; ++k)
                text += " " + lines[(i + k) % lines.length];
            // insert(0) : le modele est du plus recent au plus vieux.
            bubbleModel.insert(0, { role: (i % 2) === 0 ? "agent" : "user", text: text });
        }
    }

    /*
     * Un tour avec le hub : la bulle user part tout de suite, la carte agent
     * n'apparait qu'au premier chunk et grandit avec lui (le verre suit).
     */
    function send() {
        const text = promptArea.text.trim();
        if (!text.length)
            return;
        promptArea.text = "";

        // Du plus recent au plus vieux : la bulle user d'abord, la reponse
        // agent par-dessus, donc la reponse se cale tout contre la barre.
        bubbleModel.insert(0, { role: "user", text: text });
        bubblesView.followNewest();

        hub.send(text);
    }

    function appendChunk(text) {
        if (streamRow < 0) {
            bubbleModel.insert(0, { role: "agent", text: "" });
            streamRow = 0;
        }
        const current = bubbleModel.get(streamRow).text;
        bubbleModel.setProperty(streamRow, "text", current.length ? current + text : text);
        bubblesView.followNewest();
    }

    function endTurn(stopReason) {
        if (streamRow >= 0) {
            // Un tour sans texte (outil seul, tour coupe) ne laisse pas de bulle vide.
            if (!bubbleModel.get(streamRow).text.length)
                bubbleModel.remove(streamRow);
            streamRow = -1;
        }
        settleTimer.start();
    }

    Connections {
        target: hub
        // Le texte du tour arrive chunk par chunk — c'est le chemin du vrai
        // streaming, celui qui fait grandir la carte et bouger le verre.
        function onChunkReceived(text) { win.appendChunk(text); }
        function onTurnEnded(stopReason) { win.endTurn(stopReason); }
    }

    // Bourrage d'affichage (AICE_PUMP=1) : redessine en continu, pour que le
    // frame time soit mesurable sans avoir a scroller a la main.
    Timer {
        id: pumpTimer
        running: glass.pump
        interval: 0
        repeat: true
        triggeredOnStart: true
        onTriggered: win.requestUpdate()
    }

    // Reglage une fois la liste installee : les bulles viennent d'etre realisees,
    // on se cale sur la plus recente et on repose le verre.
    Timer {
        id: settleTimer
        interval: 120
        onTriggered: {
            bubblesView.followNewest()
            glass.scheduleRefresh()
        }
    }

    Component.onCompleted: {
        // Overlay, flou, mesure des frames : le verre est C++ (KWin + QRHI).
        glass.attach(win);
        // Les bulles poussees hors de la zone de conversation sont exclues du verre.
        glass.setClipItem(bubblesView)
        if (glass.stress)
            stressSeed(2000);
        promptArea.forceActiveFocus();
        win.visible = true;
        settleTimer.start();
        hub.connectHub();
    }
}
