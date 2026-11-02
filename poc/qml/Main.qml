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

    /*
     * Proportions mesurees dans la v1 (src/Bubble.cpp, src/ChatWidget.cpp,
     * src/Glass.h), celles de l'identite qu'on connait :
     *   bulles : carte de rayon 16, contenu a 16 px de gauche et 12 px du haut,
     *           4 px d'air autour de la carte (kGlassInset), liste a 8 px des
     *           bords, 13 px du haut (place de l'ombre) et 4 px entre les bulles ;
     *   barre : pilule de rayon 22, a 8 px des bords et 19 px du bas, contenu a
     *           18 px de gauche, 8 px de haut, 10 px de droite et 8 px du bas,
     *           saisie 14 px de police avec 30 px de haut minimum,
     *           bouton d'envoi 34 x 34.
     */
    readonly property int cardRadius: 16
    readonly property int barRadius: 22
    readonly property int hudRadius: 12
    readonly property int inset: 8          // marge de la zone de bulles (v1 : 8)
    readonly property int halo: 4           // air autour de la carte pour l'ombre (v1 : 4)
    readonly property int fadeBand: 90      // hauteur du fade en haut/bas de la zone
    readonly property color userCard: "#5A3478DA"   // rgba(52,120,218,90)
    readonly property color agentCard: "#64222228"  // rgba(34,34,40,100)
    readonly property color cardBorder: "#24FFFFFF" // rgba(255,255,255,36)
    readonly property color barFill: "#6E222228"    // rgba(34,34,40,110)
    readonly property color barBorder: "#28FFFFFF"  // rgba(255,255,255,40)
    readonly property color ink: "#e9eaee"
    // Le corps est en Ubuntu (police installee sur la machine : Ubuntu Regular),
    // plus etroite et plus legere que la police par defaut du systeme.
    readonly property string uiFont: "Ubuntu"
    readonly property string monoFont: "Ubuntu Mono"

    // Reglages modifies a la volee depuis le badge (mesure du cout GPU).
    property bool shadowOn: glass.shadow   // AICE_SHADOW=0 pour mesurer sans ombre
    property bool fadeOn: true

    width: pocWidth
    height: pocHeight
    // Sous la, la barre de prompt (46 px) et une carte ne tiennent plus : le
    // composeur doit poser la limite quand la poignee de taille arrive dessus.
    minimumWidth: 320
    minimumHeight: 380
    visible: false
    color: "transparent"
    // Le drapeau « garde devant » ne se pose que pour le mode au-dessus : en mode
    // fenetre le corps peut etre double comme n'importe quelle fenetre, et en mode
    // fixe c'est la couche du composeur qui decide du rang.
    flags: Qt.Window | Qt.FramelessWindowHint | Qt.Tool
           | (glass.windowMode === "au-dessus" ? Qt.WindowStaysOnTopHint : 0)

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
        property int drop: 5              // 5 px sous la carte : l'ombre reste centree sur elle
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

    /*
     * Un bouton d'autorisation : le libellé est celui du moteur, la place est
     * celle du kind (autoriser a gauche, refuser a droite).
     */
    component AskOption: Rectangle {
        id: askOption

        required property var modelData

        width: askOptionLabel.width + 22
        height: 28
        radius: 14
        color: modelData.allow ? "#5A3478DA" : "#4AD24453"
        border.color: "#30FFFFFF"
        border.width: 1

        Text {
            id: askOptionLabel
            anchors.centerIn: parent
            text: (modelData.name && modelData.name.length) ? modelData.name : modelData.optionId
            color: win.ink
            font.family: win.uiFont
            font.pixelSize: 12
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: hub.respondPermission(modelData.optionId)
        }
    }

    /*
     * Un bloc de markdown rendu dans une carte.
     *
     * L'air est celui de la v1, et c'est la carte qui le porte : 5 px autour d'un
     * paragraphe ou d'une liste, 14/7 autour d'un titre, 8 autour d'un filet, 0 au
     * premier et au dernier bloc (bulle nette aux bords). Qt ne sait pas rendre
     * ces marges depuis un CSS : il les ecrase par des marges inline.
     *
     * Le fond (code et tableau) est un Rectangle, pas du CSS : le texte riche de
     * Qt Quick ne dessine pas de fond derriere un span.
     */
    component BlockChunk: Rectangle {
        id: chunk

        required property var model
        required property var modelData
        required property int index

        readonly property var blk: modelData
        readonly property string kind: blk ? blk.kind : "para"
        readonly property int level: blk && blk.level ? blk.level : 0
        readonly property bool mono: kind === "code" || kind === "table"
        readonly property bool hasHtml: !mono && blk && blk.html && blk.html.length > 0
        readonly property bool first: index === 0
        readonly property bool last: index === (model ? model.length - 1 : 0)

        readonly property real topPad: first ? 0.0
            : kind === "heading" ? (level <= 1 ? 14.0 : 12.0)
            : kind === "rule" ? 8.0 : 5.0
        readonly property real bottomPad: last ? 0.0
            : kind === "heading" ? 7.0
            : kind === "rule" ? 8.0 : 5.0
        // Le fond d'un bloc de code est dessine, pas negocie : 6 px autour du texte.
        readonly property real monoPad: mono ? 6.0 : 0.0
        readonly property real indent: kind === "quote" ? 12.0
            : (kind === "list" || kind === "task") ? 8.0 : 0.0

        // parent = la colonne des blocs, dont la largeur est celle du contenu de la carte
        width: parent.width
        height: kind === "rule" ? (topPad + 1.0 + bottomPad)
                                : (text.y + text.height + monoPad + bottomPad)

        color: mono ? (kind === "code" ? "#16FFFFFF" : "#14FFFFFF") : "transparent"
        border.width: mono ? 1 : 0
        border.color: "#1AFFFFFF"
        radius: mono ? 6 : 0

        Text {
            id: text
            visible: chunk.kind !== "rule"
            x: chunk.indent + (chunk.mono ? 8.0 : 0.0)
            y: chunk.topPad + chunk.monoPad
            width: chunk.width - chunk.x - (chunk.mono ? 8.0 : 0.0)
            text: chunk.blk ? (chunk.mono || !chunk.hasHtml ? chunk.blk.text : chunk.blk.html) : ""
            textFormat: chunk.hasHtml ? Text.RichText : Text.PlainText
            color: chunk.kind === "quote" ? "#AAe9eaee"
                 : chunk.mono ? "#DCe9eaee" : win.ink
            font.family: chunk.mono ? win.monoFont : win.uiFont
            font.pixelSize: chunk.kind === "heading"
                              ? (chunk.level <= 1 ? 17 : chunk.level === 2 ? 15
                                                   : chunk.level === 3 ? 14 : 13)
                              : 13
            font.bold: chunk.kind === "heading"
            wrapMode: Text.WordWrap
            lineHeight: 1.25
        }

        // Le filet, dessine : le texte riche de Qt Quick ne sait pas rendre <hr>.
        Rectangle {
            id: ruleLine
            visible: chunk.kind === "rule"
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: 1
            color: "#32FFFFFF"
        }

        // La citation garde sa barre de la v1 (2 px, a gauche du texte).
        Rectangle {
            id: quoteBar
            visible: chunk.kind === "quote"
            x: 3
            y: text.y
            width: 2
            height: text.height
            color: "#3CFFFFFF"
        }
    }

    /*
     * Le meme bloc, mesure en une seule ligne et sans fond : c'est lui qui dit
     * quelle largeur la carte doit prendre. Le texte mesure est celui rendu (les
     * marques du markdown sont deja retirees par le parseur).
     */
    component NaturalChunk: Text {
        required property var modelData

        readonly property var blk: modelData
        readonly property string kind: blk ? blk.kind : "para"
        readonly property bool mono: kind === "code" || kind === "table"

        text: blk ? (blk.plain && blk.plain.length ? blk.plain : blk.text) : ""
        color: win.ink
        font.family: mono ? win.monoFont : win.uiFont
        font.pixelSize: kind === "heading"
                          ? (blk.level <= 1 ? 17 : blk.level === 2 ? 15
                                 : blk.level === 3 ? 14 : 13)
                          : 13
        font.bold: kind === "heading"
    }

    // --- les bulles de conversation ---
    ListModel { id: bubbleModel }

    // La bulle agent en cours de remplissage (index dans le modele, -1 sinon).
    property int streamRow: -1
    property string streamMessageId: ""

    /* ---------------------------------------------------------------- bulles */

    ListView {
        id: bubblesView
        anchors.top: parent.top
        anchors.bottom: pill.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: win.inset
        anchors.rightMargin: win.inset
        /*
         * Air entre la fenetre et les cartes, pris de la v1 (marges de la liste
         * 8/13/8/8 + 4 px d'inset autour de chaque carte) : les faces des cartes
         * s'arretent a 12 px des cotes et 17 px du haut.
         */
        anchors.topMargin: 17
        anchors.bottomMargin: 22

        model: bubbleModel
        /*
         * Ecart entre deux bulles : 12 px, comme en v1. En v1, la carte est dessinee
         * a 4 px du bord de sa bulle (kGlassInset) et les bulles sont espacees de
         * 4 px : l'air visible entre deux cartes fait 4 + 4 + 4. Ici la carte est
         * toute la bulle, donc l'espacement porte a lui seul cet air.
         */
        spacing: 12
        cacheBuffer: 600
        boundsBehavior: Flickable.StopAtBounds
        focus: false

        // Le modele est range du plus recent au plus vieux : index 0 = la bulle la
        // plus recente, calee en bas de la zone juste au-dessus de la barre, les
        // anciennes s'empilant au-dessus. C'est la seule facon d'ancrer les bulles
        // en bas sans tricher : un espaceur vide place au-dessus de la liste a
        // bloque la realisation des bulles par la vue, donc leur presence dans
        // le verre.
        verticalLayoutDirection: ListView.BottomToTop

        /*
         * Cote « plus recent » d'une vue inverseee. La vue ne publie plus de
         * contentYMax (l'API Flickable ne la connait pas) : la borne haute se
         * calcule. Verifie a 200 bulles comme a 6, sur une zone de 761 px :
         * contentY = -761 quand la plus recente est visible, originY = -14 188
         * (200 bulles) comme -420 (6 bulles).
         */
        readonly property real newestY: originY + contentHeight - height
        readonly property bool atNewest: contentY >= newestY - 4
        function followNewest() {
            if (atNewest)
                contentY = newestY;
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
            required property string msgId
            required property string blocks
            required property bool parsed

            // Le modele de bulles ne sait pas garder un tableau : les blocs y
            // tiennent en JSON, et une seule lecture par bulle le reconvertit.
            readonly property var blockList: parsed ? JSON.parse(blocks) : []
            readonly property int blockCount: blockList ? blockList.length : 0
            readonly property bool agent: role === "agent"
            // Une bulle fermee porte ses blocs venus du parseur hors-UI ; en
            // streaming (ou pour une bulle user), elle reste en texte brut.
            readonly property bool rich: agent && parsed && blockCount > 0

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

                // Les deux roles epousent leur texte (la v1 etait une colonne de
                // 460 px : une reponse courte donnait une carte courte), et le
                // padding est le plus etroit possible : 12 px sur les cotes, 8 px en
                // haut et en bas. Une bulle d'une seule ligne est donc tres petite.
                // En riche, c'est la largeur naturelle de ses blocs qui decide.
                width: Math.min(cell.width - 2 * win.halo,
                                Math.max(cell.rich ? naturalBlocks.implicitWidth
                                                   : natural.implicitWidth,
                                          40) + 24)
                height: (cell.rich ? blockCol.implicitHeight : body.implicitHeight) + 16
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
                    visible: !cell.rich
                    x: 12
                    y: 8
                    width: parent.width - 24
                    text: cell.text
                    textFormat: Text.PlainText
                    color: win.ink
                    font.family: win.uiFont
                    font.pixelSize: 13
                    wrapMode: Text.WordWrap
                    lineHeight: 1.25
                }

                /*
                 * La bulle fermée : une colonne de blocs, un par bloc du markdown.
                 * C'est la carte qui porte l'air entre eux (voir BlockChunk), comme
                 * en v1 — pas un CSS que Qt ecrase.
                 */
                Column {
                    id: blockCol
                    visible: cell.rich
                    x: 12
                    y: 8
                    width: card.width - 24
                    spacing: 0

                    Repeater {
                        model: cell.blockList
                        delegate: BlockChunk {}
                    }
                }

                // Deuxieme passe de mesure : la largeur naturelle du texte, pour
                // que la carte colle a son contenu (comme la bulle v1). Elle porte
                // la meme mise en forme que ce qui se rend : une bulle riche se
                // mesure bloc par bloc, sinon la carte se trompe de largeur.
                Column {
                    id: naturalBlocks
                    visible: false
                    x: 0
                    y: 0
                    spacing: 0

                    Repeater {
                        model: cell.blockList
                        delegate: NaturalChunk {}
                    }
                }

                Text {
                    id: natural
                    visible: false
                    text: body.text
                    textFormat: body.textFormat
                    color: win.ink
                    font.family: win.uiFont
                    font.pixelSize: 13
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
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        anchors.bottomMargin: 19

        // v1 : 8 px de marge haute et basse autour d'une saisie de 30 px minimum.
        height: Math.max(46, promptArea.implicitHeight + 28)

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
            layer.effect: CardShadow { drop: 6 }
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
                    font.family: win.uiFont
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
                anchors.left: parent.left
                anchors.right: sendBtn.left
                anchors.verticalCenter: parent.verticalCenter
                text: ""
                readonly property string hint: "Ask A-Ice\u2026"
                color: "#f0f1f3"
                font.family: win.uiFont
                font.pixelSize: 14
                wrapMode: TextEdit.Wrap
                // M3d : l'autocompletion suit ce qui est ecrit. Une saisie
                // nouvelle efface l'annulation demandee au clavier (Echap).
                onTextChanged: {
                    win.slashDismissed = false;
                    win.computeSlash();
                }
                // TextEdit (Qt Quick de base) n'a pas de signal accepted :
                // l'entrée est traitee a la main, et shift+entrée garde son
                // comportement (saut de ligne dans le prompt).
                // Une demande du moteur passe avant le prompt : Entrée prend la
                // premiere autorisation (le pont les a rangeees), Échap prend le
                // refus du moteur — jamais un « cancelled » invente quand le
                // moteur propose son propre refus.
                Keys.onReturnPressed: (event) => {
                    if (event.modifiers & Qt.ShiftModifier)
                        return;
                    if (hub.permissionPending) {
                        const options = hub.permissionOptions;
                        hub.respondPermission(options.length ? options[0].optionId : "");
                        event.accepted = true;
                        return;
                    }
                    if (hub.elicitationPending) {
                        askCard.accepted();
                        event.accepted = true;
                        return;
                    }
                    // Une suggestion en cours d'abord : Entree complete le / a
                    // moitie ecrit, et n'envoie que ce qui est deja la commande
                    // entiere — sinon on enverrait un texte moitie au moteur.
                    if (win.slashRows.length && !win.slashComplete()) {
                        win.slashApply(win.slashRow >= 0 ? win.slashRow : 0);
                        event.accepted = true;
                        return;
                    }
                    win.send();
                    event.accepted = true;
                }
                // Tab complete la suggestion surlignee, les fleches la choisissent
                // tant qu'elle est la (elles gardent leur role dans le texte sinon).
                Keys.onTabPressed: (event) => {
                    if (!win.slashRows.length)
                        return;
                    win.slashApply(win.slashRow >= 0 ? win.slashRow : 0);
                    event.accepted = true;
                }
                Keys.onUpPressed: (event) => {
                    if (win.slashStep(-1))
                        event.accepted = true;
                }
                Keys.onDownPressed: (event) => {
                    if (win.slashStep(1))
                        event.accepted = true;
                }
                Keys.onEscapePressed: (event) => {
                    if (hub.permissionPending) {
                        hub.respondPermission(hub.rejectOption());
                        event.accepted = true;
                        return;
                    }
                    if (hub.elicitationPending) {
                        hub.respondElicitation("cancel", ({}));
                        event.accepted = true;
                        return;
                    }
                    // Premier Echap : effacer les suggestions. Second : fermer.
                    if (win.slashRows.length) {
                        win.slashCollapse();
                        event.accepted = true;
                        return;
                    }
                    Qt.quit();
                }
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
                font.family: win.uiFont
                font.pixelSize: 14
                wrapMode: Text.WordWrap
            }
        }

        /*
         * Poignee de redimensionnement : le bord droit de la barre, la ou l'on
         * attend la limite de la fenetre.
         *
         * Deux obligations :
         *   - elle est declaree APRES pillDrag et apres pillRow : dans une carte,
         *     le MouseArea monte le plus haut capte le geste en premier, sinon le
         *     drag de la barre mangerait la poignee ;
         *   - elle est DEDANS la carte de verre : la fenetre est masquee ailleurs,
         *     donc un element pose hors des cartes ne recoit aucun clic.
         *
         * Sa largeur (10 px) est exactement la marge de droite du contenu : elle ne
         * recouvre pas le bouton d'envoi.
         */
        Item {
            id: resizeEdge

            z: 1
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 10

            // Pas de repere visible : seule la zone de saisie du geste compte. Le
            // geste s'annonce par le curseur — SizeFDiagCursor ici, SizeAllCursor
            // sur le reste de la barre.
            MouseArea {
                id: edgeArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.SizeFDiagCursor
                acceptedButtons: Qt.LeftButton

                property bool nativeResize: false
                property real lastX: 0
                property real lastY: 0
                property int startW: 0
                property int startH: 0
                property real pressedAt: 0

                onPressed: {
                    lastX = mouse.x
                    lastY = mouse.y
                    startW = win.width
                    startH = win.height
                    pressedAt = Date.now()
                    nativeResize = glass.startResize(Qt.RightEdge | Qt.BottomEdge)
                }
                onPositionChanged: {
                    /*
                     * Le geste est au clic, pas au survol. Un MouseArea avec
                     * hoverEnabled recoit AUSSI les mouvements sans bouton : sans
                     * ce garde-fou, la fenetre se fait redimensionner par un simple
                     * passage de curseur sur le bord droit.
                     */
                    if (!pressed)
                        return;
                    /*
                     * Le composeur a-t-il vraiment joue ? Une fenetre sans decoration
                     * peut lui faire refuser le geste tout en le donnant pour bon.
                     * On lui laisse un delai, puis on regarde la taille : si elle
                     * n'a pas bouge, on reprend la main par deltas — et on repart
                     * de la position courante du curseur pour ne pas empiler deux
                     * redimensionnements.
                     */
                    if (nativeResize) {
                        if (Date.now() - pressedAt < 220)
                            return;
                        if (win.width !== startW || win.height !== startH)
                            return;
                        nativeResize = false
                        lastX = mouse.x
                        lastY = mouse.y
                    }
                    win.width = win.width + (mouse.x - lastX)
                    win.height = win.height + (mouse.y - lastY)
                    lastX = mouse.x
                    lastY = mouse.y
                }
                onReleased: nativeResize = false
                onCanceled: nativeResize = false
            }
        }
    }

    /* ------------------------------ carte d'autorisation (M2, reprise M3b) */

    /*
     * Le pont retransmet `session/request_permission` : la demande du moteur
     * devient une carte de verre au-dessus de la barre. Les autorisations sont a
     * gauche (la premiere est le choix par defaut, Entrée), les refus a droite
     * (Échap). Les libelles viennent du moteur, tels quels : on ne renvoie qu'un
     * optionId, et c'est le pont qui recolle la decision durable (une decision
     * invente here ferait d'un « toujours » un one-shot).
     */
    Item {
        id: permCard

        property bool glassCard: true
        property int glassRadius: win.cardRadius

        // L'entree de l'outil est coupee a trois lignes ; « deployer » la montre entiere.
        property bool detailShown: false

        visible: hub.permissionPending

        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: pill.top
        anchors.bottomMargin: 16

        width: Math.min(win.width - 2 * win.inset, 470)
        height: permCol.height + 16

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
            layer.effect: CardShadow { intensity: 0.5 }
        }

        Column {
            id: permCol
            x: 12
            y: 8
            spacing: 6
            width: permCard.width - 24

            Item {
                width: permCol.width
                height: 18

                Text {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - (hub.askCount > 1 ? 120 : 0)
                    text: "le moteur demande : " + hub.permissionTool
                          + (hub.permissionKind.length ? "  \u00b7  " + hub.permissionKind : "")
                    color: win.ink
                    font.family: win.uiFont
                    font.pixelSize: 13
                    font.bold: true
                    elide: Text.ElideRight
                }
                Text {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    visible: hub.askCount > 1
                    text: "demande " + hub.askIndex + " sur " + hub.askCount
                    color: "#80e9eaee"
                    font.family: win.uiFont
                    font.pixelSize: 11
                }
            }

            // L'entree de l'outil, en monospace : c'est du texte de commande.
            Text {
                width: permCol.width
                text: hub.permissionDetail
                color: "#A8e9eaee"
                font.family: win.monoFont
                font.pixelSize: 11
                wrapMode: Text.WordWrap
                elide: Text.ElideRight
                maximumLineCount: permCard.detailShown ? 100000 : 3
            }

            // La sortie, quand le moteur la joint deja a sa demande.
            Text {
                visible: hub.permissionOutput.length > 0
                width: permCol.width
                text: hub.permissionOutput
                color: "#80e9eaee"
                font.family: win.monoFont
                font.pixelSize: 10
                wrapMode: Text.WordWrap
                elide: Text.ElideRight
                maximumLineCount: 3
            }

            Row {
                spacing: 10
                Text {
                    text: permCard.detailShown ? "replier" : "deployer"
                    color: "#80e9eaee"
                    font.family: win.uiFont
                    font.pixelSize: 11
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: permCard.detailShown = !permCard.detailShown
                    }
                }
                Text {
                    text: "annuler la demande"
                    color: "#80e9eaee"
                    font.family: win.uiFont
                    font.pixelSize: 11
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        // Aucune option : le pont reprend la main, c'est le « cancelled » du protocole.
                        onClicked: hub.respondPermission("")
                    }
                }
            }

            Item {
                width: permCol.width
                height: 28

                Row {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8
                    Repeater {
                        model: hub.permissionOptions.filter((o) => o.allow)
                        delegate: AskOption {}
                    }
                }

                Row {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8
                    Repeater {
                        model: hub.permissionOptions.filter((o) => !o.allow)
                        delegate: AskOption {}
                    }
                }
            }
        }
    }

    /* ------------------------------- carte de questionnaire (M3c) */

    /*
     * `elicitation/create` (mode formulaire) : le moteur pose ses questions, une
     * seule carte les rend toutes. Le choix multiple rend un tableau de chaines,
     * le champ libre compagnon ecrase le choix simple et s'ajoute au choix
     * multiple — c'est le pont qui lit le contenu, on ne reinvente rien.
     */
    Item {
        id: askCard

        property bool glassCard: true
        property int glassRadius: win.cardRadius

        // Les reponses en cours, par cle du schema. Elles sont remplacees d'un
        // coup (jamais modifiees en place) : c'est ce qui previent les boutons.
        property var answers: ({})

        visible: hub.elicitationPending

        anchors.horizontalCenter: parent.horizontalCenter
        // Les deux cartes ne se marchent pas dessus : la demande d'autorisation
        // tient la place basse, le questionnaire se pose au-dessus d'elle.
        y: (permCard.visible ? permCard.y : pill.y) - height - 16

        width: Math.min(win.width - 2 * win.inset, 470)
        height: askCol.height + 16

        onXChanged: glass.scheduleRefresh()
        onYChanged: glass.scheduleRefresh()
        onHeightChanged: glass.scheduleRefresh()
        onVisibleChanged: glass.scheduleRefresh()

        // Une nouvelle question remplace la reponse en cours : la carte ne doit
        // pas trainer les boutions du questionnaire precedent.
        property string formKey: hub.elicitationMessage + "|" + hub.elicitationFields.length
        onFormKeyChanged: answers = ({})

        function setAnswer(key, value) {
            const next = Object.assign({}, askCard.answers);
            next[key] = value;
            askCard.answers = next;
        }

        function toggleAnswer(key, value) {
            const picked = Array.isArray(askCard.answers[key]) ? askCard.answers[key].slice() : [];
            const at = picked.indexOf(value);
            if (at >= 0)
                picked.splice(at, 1);
            else
                picked.push(value);
            const next = Object.assign({}, askCard.answers);
            next[key] = picked;
            askCard.answers = next;
        }

        function picked(fieldKey, fieldKind, optionValue) {
            const value = askCard.answers[fieldKey];
            if (fieldKind === "multi")
                return Array.isArray(value) && value.indexOf(optionValue) >= 0;
            if (fieldKind === "boolean")
                return value === true;
            return value === optionValue;
        }

        /*
         * Le contenu rendu conforme au schema : rien n'est obligatoire (le pont
         * laisse required vide), donc une reponse partielle est une reponse.
         */
        function accepted() {
            const content = {};
            for (const field of hub.elicitationFields) {
                const other = askCard.answers[field.otherKey || ""];
                const value = askCard.answers[field.key];
                if (field.kind === "multi") {
                    const list = Array.isArray(value) ? value.slice() : [];
                    if (other && String(other).length)
                        list.push(String(other));
                    if (list.length)
                        content[field.key] = list;
                } else if (field.kind === "boolean") {
                    if (value !== undefined)
                        content[field.key] = value === true;
                } else if (field.kind === "number") {
                    if (value !== undefined && String(value).length)
                        content[field.key] = Number(value);
                } else if (other && String(other).length) {
                    content[field.key] = String(other);
                } else if (value !== undefined && String(value).length) {
                    content[field.key] = String(value);
                }
            }
            hub.respondElicitation("accept", content);
        }

        Rectangle {
            id: askFill

            anchors.fill: parent
            radius: win.cardRadius
            color: "#8A1E2430"
            border.color: "#40A8C8F0"
            border.width: 1

            layer.enabled: win.shadowOn
            layer.samples: 4
            layer.effect: CardShadow { intensity: 0.5 }
        }

        Column {
            id: askCol
            x: 12
            y: 8
            spacing: 8
            width: askCard.width - 24

            Text {
                width: askCol.width
                text: hub.elicitationMessage
                      + (hub.askCount > 1 ? "  \u00b7  question " + hub.askIndex + " sur " + hub.askCount : "")
                color: win.ink
                font.family: win.uiFont
                font.pixelSize: 13
                font.bold: true
                wrapMode: Text.WordWrap
            }

            Repeater {
                model: hub.elicitationFields

                delegate: Column {
                    id: field

                    required property var modelData

                    readonly property var ask: modelData

                    width: askCol.width
                    spacing: 4

                    Text {
                        width: field.width
                        text: field.ask.title
                        color: win.ink
                        font.family: win.uiFont
                        font.pixelSize: 12
                        wrapMode: Text.WordWrap
                    }
                    Text {
                        visible: (field.ask.help || "").length > 0
                        width: field.width
                        text: field.ask.help
                        color: "#80e9eaee"
                        font.family: win.uiFont
                        font.pixelSize: 10
                        wrapMode: Text.WordWrap
                    }

                    // Un choix (unique ou multiple) : une rangée de pastilles.
                    Flow {
                        visible: field.ask.options.length > 0 || field.ask.kind === "boolean"
                        width: field.width
                        spacing: 8

                        Repeater {
                            model: field.ask.options
                            delegate: Rectangle {
                                id: option

                                required property var modelData

                                readonly property bool on: askCard.picked(field.ask.key, field.ask.kind, modelData.value)
                                readonly property bool skip: modelData.value === "__skip__"

                                width: optionLabel.width + 22
                                height: 26
                                radius: 13
                                color: on ? (skip ? "#46736F78" : "#743478DA") : "#24FFFFFF"
                                border.color: on ? "#60FFFFFF" : "#22FFFFFF"
                                border.width: 1

                                Text {
                                    id: optionLabel
                                    anchors.centerIn: parent
                                    // Le pont met le libelle dans le titre : le sentinel
                                    // __skip__ ne doit jamais se lire tel quel.
                                    text: option.skip ? "passer" : modelData.label
                                    color: win.ink
                                    font.family: win.uiFont
                                    font.pixelSize: 12
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        if (field.ask.kind === "multi")
                                            askCard.toggleAnswer(field.ask.key, modelData.value);
                                        else
                                            askCard.setAnswer(field.ask.key, modelData.value);
                                    }
                                }
                            }
                        }

                        Repeater {
                            model: field.ask.kind === "boolean" ? ["oui", "non"] : []
                            delegate: Rectangle {
                                required property var modelData

                                readonly property bool on: askCard.answers[field.ask.key] === (modelData === "oui")

                                width: boolLabel.width + 22
                                height: 26
                                radius: 13
                                color: on ? "#743478DA" : "#24FFFFFF"
                                border.color: "#22FFFFFF"
                                border.width: 1

                                Text {
                                    id: boolLabel
                                    anchors.centerIn: parent
                                    text: modelData
                                    color: win.ink
                                    font.family: win.uiFont
                                    font.pixelSize: 12
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: askCard.setAnswer(field.ask.key, modelData === "oui")
                                }
                            }
                        }
                    }

                    // Le champ libre compagnon (q_i_other) : il ecrase le choix
                    // simple, il s'ajoute au choix multiple — le pont le lit ainsi.
                    Item {
                        visible: (field.ask.otherKey || "").length > 0
                        width: field.width
                        height: field.ask.kind === "text" || field.ask.kind === "number" ? 0 : 24

                        TextInput {
                            id: otherInput
                            anchors.fill: parent
                            color: win.ink
                            font.family: win.uiFont
                            font.pixelSize: 12
                            text: ""
                            onTextChanged: askCard.setAnswer(field.ask.otherKey, text)
                            onAccepted: askCard.accepted()
                        }
                        Text {
                            anchors.fill: otherInput
                            visible: !otherInput.text.length
                            text: field.ask.otherTitle
                            color: "#59e9eaee"
                            font.family: win.uiFont
                            font.pixelSize: 12
                        }
                    }

                    // Et le champ lui-meme, quand la question n'a pas de liste.
                    TextInput {
                        visible: field.ask.kind === "text" || field.ask.kind === "number"
                        width: field.width
                        height: 24
                        color: win.ink
                        font.family: field.ask.kind === "number" ? win.monoFont : win.uiFont
                        font.pixelSize: 12
                        onTextChanged: askCard.setAnswer(field.ask.key, text)
                        onAccepted: askCard.accepted()
                    }
                }
            }

            Row {
                spacing: 10
                Text {
                    text: "repondre"
                    color: "#C0e9eaee"
                    font.family: win.uiFont
                    font.pixelSize: 11
                    font.bold: true
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: askCard.accepted()
                    }
                }
                Text {
                    text: "laisser tomber"
                    color: "#80e9eaee"
                    font.family: win.uiFont
                    font.pixelSize: 11
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: hub.respondElicitation("cancel", ({}))
                    }
                }
            }
        }
    }

    /* --------------------------------- carte de suggestions slash (M3d) */

    /*
     * Ce que le moteur publie, rendu tel quel : les commandes slash qu il admet
     * (available_commands_update) et les valeurs que la session admet pour ses
     * reglages (configOptions). Rien n est durci : /compact, /goal, les skills ou
     * les commandes de plugin apparaissent parce que le moteur les declare.
     *
     * La carte se pose au-dessus de la barre (elle ne marche pas dessus) et ne
     * vit que pendant la saisie d un /. Elle est declaree apres les cartes de
     * demande : si elles sont montees en meme temps, c elle qui recoit les clics.
     */
    Item {
        id: slashCard

        property bool glassCard: true
        property int glassRadius: win.hudRadius

        visible: win.slashRows.length > 0

        anchors.horizontalCenter: parent.horizontalCenter
        y: (askCard.visible ? askCard.y
              : (permCard.visible ? permCard.y : pill.y)) - height - 10

        width: Math.min(win.width - 2 * win.inset, 470)
        height: slashCol.height + 14

        onXChanged: glass.scheduleRefresh()
        onYChanged: glass.scheduleRefresh()
        onHeightChanged: glass.scheduleRefresh()
        onWidthChanged: glass.scheduleRefresh()
        onVisibleChanged: glass.scheduleRefresh()

        Rectangle {
            id: slashFill

            anchors.fill: parent
            radius: win.hudRadius
            color: "#8A1E2430"
            border.color: "#26FFFFFF"
            border.width: 1

            layer.enabled: win.shadowOn
            layer.samples: 4
            layer.effect: CardShadow { intensity: 0.5 }
        }

        Column {
            id: slashCol
            x: 8
            y: 7
            spacing: 2
            width: slashCard.width - 16

            Text {
                width: slashCol.width
                text: "commandes du moteur  \u00b7  Tab complete, Entree envoie"
                color: "#70e9eaee"
                font.family: win.uiFont
                font.pixelSize: 10
            }

            Repeater {
                model: win.slashRows

                delegate: Rectangle {
                    id: row

                    required property var modelData
                    required property int index

                    width: slashCol.width
                    height: 22
                    radius: 7
                    color: index === win.slashRow ? "#28FFFFFF" : "transparent"

                    Text {
                        id: rowCommand
                        x: 8
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(190, implicitWidth)
                        text: row.modelData.command
                        color: index === win.slashRow ? win.ink : "#C0e9eaee"
                        font.family: win.uiFont
                        font.pixelSize: 12
                        font.bold: true
                        elide: Text.ElideRight
                    }

                    Text {
                        anchors.left: rowCommand.right
                        anchors.leftMargin: 10
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        text: (row.modelData.description || "")
                              + (row.modelData.hint ? "  \u00b7  " + row.modelData.hint : "")
                              + (row.modelData.current ? "  \u00b7  actuel" : "")
                        color: "#80e9eaee"
                        font.family: win.uiFont
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: win.slashApply(index)
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
        // En fenetre etroite, la carte du hub se pose sous le badge de mesures au
        // lieu de passer dessous (elles sont ancrees toutes deux en haut, de cotes
        // opposes).
        anchors.topMargin: glass.hud ? hud.height + 28 : 20
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
            layer.effect: CardShadow { intensity: 0.5 }
        }

        Column {
            id: hubCol
            x: 10
            y: 7
            spacing: 3

            Text {
                text: "hub " + hub.status
                color: hub.ready ? win.ink : "#B8E8A2A8"
                font.family: win.uiFont
                font.pixelSize: 11
            }
            Text {
                visible: hub.toolLine.length > 0
                text: "outil " + hub.toolLine
                color: "#90e9eaee"
                font.family: win.uiFont
                font.pixelSize: 11
            }
            Text {
                visible: hub.ready
                // On voit quel cerveau du hub on est en train de faire parler.
                text: "dans " + hub.workspace
                color: "#70e9eaee"
                font.family: win.uiFont
                font.pixelSize: 10
            }
            Text {
                visible: hub.ready && hub.configLabel("model").length > 0
                // Les reglages que le moteur publie et que /model, /mode et
                // /thought changent (voir la carte de suggestions de la barre).
                text: "modele " + hub.configLabel("model")
                      + (hub.configLabel("mode").length ? "  \u00b7  mode " + hub.configLabel("mode") : "")
                      + (hub.configLabel("thought").length ? "  \u00b7  pensee " + hub.configLabel("thought") : "")
                color: "#70e9eaee"
                font.family: win.uiFont
                font.pixelSize: 10
            }
            Text {
                // La voix de la v1, reprise telle quelle (src/voice/) : le
                // diagnostic du module — etat, buffer local, avance estimee.
                visible: hub.ready
                text: "voix " + voice.status
                color: voice.on ? "#A8F0A0B0" : "#60e9eaee"
                font.family: win.uiFont
                font.pixelSize: 10
            }
            Text {
                // Le mode de fenetre vecu (fenetre / au-dessus / fixe) : ce que le
                // composeur a vraiment pose, plus ce qui reste a faire pour lui.
                text: "fenetre " + glass.windowMode
                      + (glass.windowModeRestart ? " · relancer pour appliquer" : "")
                color: "#80e9eaee"
                font.family: win.uiFont
                font.pixelSize: 10
            }
            Row {
                spacing: 10
                Text {
                    text: hub.ready ? "couper" : "rejoindre"
                    color: "#80e9eaee"
                    font.family: win.uiFont
                    font.pixelSize: 11
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: hub.ready ? hub.disconnectHub() : hub.connectHub()
                    }
                }
                Text {
                    // La session vecue est gardee par le client : sans cette sortie
                    // on resterait bloque sur une conversation usee.
                    visible: hub.ready
                    text: "conversation neuve (/new)"
                    color: "#80e9eaee"
                    font.family: win.uiFont
                    font.pixelSize: 11
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: win.newConversation()
                    }
                }
                Text {
                    // La meme commande que /voice dans la barre, a portee de clic.
                    text: voice.on ? "couper la voix" : "monter la voix"
                    color: "#80e9eaee"
                    font.family: win.uiFont
                    font.pixelSize: 11
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: win.reglerVoix("")
                    }
                }
                Text {
                    // Le clic fait tourner les trois modes de fenetre (fenetre →
                    // au-dessus → fixe). Le reglage est persiste : il vaudra aussi
                    // au prochain lancement.
                    text: "fenetre : " + glass.windowMode
                    color: "#80e9eaee"
                    font.family: win.uiFont
                    font.pixelSize: 11
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: win.reglerFenetre("")
                    }
                }
            }

            /*
             * Multi-sessions : les sessions vivantes du hub, une par ligne,
             * cliquable. Le corps ne connaît jamais que l'une d'elles a la fois —
             * ouvrir l'autre vide la conversation et fait rejouer l historique.
             */
            Repeater {
                model: hub.sessions

                delegate: Column {
                    spacing: 1

                    Text {
                        text: (modelData.title.length
                                   ? (modelData.title.length > 34 ? modelData.title.slice(0, 34) + "…" : modelData.title)
                                   : modelData.sessionId.slice(0, 8))
                              + " · " + modelData.workspace + (modelData.state === "idle" ? "" : " · " + modelData.state)
                        color: modelData.sessionId === hub.sessionId ? win.ink : "#80e9eaee"
                        font.family: win.uiFont
                        font.pixelSize: 10
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: hub.openSession(modelData.sessionId, modelData.workspacePath)
                        }
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
            layer.effect: CardShadow { intensity: 0.5 }
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
                font.family: win.uiFont
                font.pixelSize: 11
            }

            Row {
                spacing: 10

                Text {
                    text: "ombre " + (win.shadowOn ? "o" : "non")
                    color: win.shadowOn ? "#CC" + win.ink.toString().slice(1) : "#59e9eaee"
                    font.family: win.uiFont
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
                    font.family: win.uiFont
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
                    font.family: win.uiFont
                    font.pixelSize: 11
                }
                Text {
                    text: "taille " + win.width + " x " + win.height + " ("
                          + (glass.resizeNative ? "natif" : "par deltas") + ")"
                    color: "#80e9eaee"
                    font.family: win.uiFont
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
            bubbleModel.insert(0, { role: (i % 2) === 0 ? "agent" : "user", text: text, msgId: "", blocks: "[]", parsed: false });
        }
    }

    /*
     * Une bulle de markdown complet, sans hub : l'echantillon qui sert a regarder
     * le rendu des blocs (titre, liste, code, citation, tableau, filet, cases)
     * hors connexion, en AICE_STRESS.
     */
    function seedMarkdown() {
        const sample = "## Rapport du hub\n\n"
            + "Voici ce que j'ai trouve, en **deux** points.\n\n"
            + "- le premier point\n"
            + "- le second, en **gras**\n"
            + "- le troisieme avec du `code en ligne`\n\n"
            + "```python\ndef repond(requete):\n    retour = \"ok\"\n    return retour\n```\n\n"
            + "> une citation utile, avec son air a elle\n\n"
            + "| cle | valeur |\n| --- | --- |\n| hub | 534584 |\n| session | b4f893 |\n\n"
            + "---\n\n"
            + "- [ ] a faire\n"
            + "- [x] fait\n\n"
            + "La conclusion, en une phrase.";
        bubbleModel.insert(0, { role: "agent", text: sample, msgId: "seed-md", blocks: "[]", parsed: false });
        markdown.parse("seed-md", sample);
    }

    /* -------------------------------------------------- commandes slash (M3d) */

    /*
     * La barre ne connait qu'une chose : ce que le moteur publie. Les commandes
     * viennent de available_commands_update, les valeurs des reglages de
     * configOptions — aucune liste n est durcie ici.
     *
     * Trois choses sont neanmoins rendues par le corps, parce que le pont ne les
     * rend pas comme nous :
     *   - /new : commande du TUI Z Code, declaree « hors service » en mode
     *   app-server (le pont repond une erreur) — c est nous qui ouvrons la
     *   conversation vierge ;
     *   - /model, /mode, /thought : le pont les intercepte et rend un chunk de
     *   texte DANS LE FIL (un faux message dans la conversation, rejoue a la
     *   reprise). On regle par session/set_config_option, qui ne laisse pas de trace.
     *
     * Le reste part tel quel : le pont sait faire (compact, goal, auto, fork,
     * quota, mcp, init, resume, skills, commandes de plugin) — et un nom qu il ne
     * connait pas non plus part au modele comme texte ordinaire, sans casser le tour.
     */
    readonly property var slashSettings: ["model", "mode", "thought"]
    // Les valeurs admises par les commandes que le corps rend lui-meme.
    readonly property var slashPropres: ({ voice: ["on", "off"],
                                          fenetre: ["fenetre", "au-dessus", "fixe"] })
    readonly property int slashMaxRows: 7

    property var slashRows: []
    property int slashRow: -1
    // Le fait que la carte de suggestions a ete ecartee au clavier : une saisie
    // nouvelle la fait repartir.
    property bool slashDismissed: false
    // Harnais AICE_SLASH : la prochaine entree de la liste a traiter.
    property int slashFed: 0

    // L'option publiee par le pont pour ce reglage, null si elle n'est pas la.
    function slashConfig(id) {
        const published = hub.configOptions;
        for (let i = 0; i < published.length; ++i) {
            if (published[i].id === id)
                return published[i];
        }
        return null;
    }

    /*
     * Les valeurs propositionnables d'un mot : celles que le moteur publie pour un
     * reglage, plus celles que nos propres commandes admettent (elles n'ont pas de
     * choix publies, donc c'est nous qui les declarons).
     */
    function slashChoices(id) {
        const moteur = slashConfig(id);
        if (moteur)
            return moteur;
        const propres = slashPropres[id];
        if (!propres)
            return null;
        const choix = [];
        for (const valeur of propres)
            choix.push({ value: valeur, name: valeur });
        return { id: id, currentValue: "", options: choix };
    }

    // Ce que le corps rend lui-meme, pose avant les commandes du pont (les noms
    // identiques du pont sont alors ignores).
    function slashOwn() {
        const own = [
            {
                name: "new",
                description: "conversation vierge — rendue par a-ice",
                hint: "",
            },
            {
                name: "voice",
                description: "la voix de la v1, au fil de l'eau — actuel " + (voice.on ? "montee" : "coupee"),
                hint: "on|off",
            },
            {
                name: "fenetre",
                description: "normale, gardee devant, ou posee sur tous les bureaux — actuel " + glass.windowMode,
                hint: "fenetre|au-dessus|fixe",
            },
        ];
        for (let i = 0; i < win.slashSettings.length; ++i) {
            const id = win.slashSettings[i];
            const option = win.slashConfig(id);
            const current = option && option.currentValue.length ? hub.configLabel(id) : "";
            own.push({
                name: id,
                description: current.length ? "actuel : " + current : "choix non publies par le moteur",
                hint: "<choix>",
            });
        }
        return own;
    }

    /*
     * Les lignes propositionnables, selon ce qui est ecrit. Deux regimes : le nom
     * de la commande, puis — pour un reglage dont le nom est ecrit et suivi d un
     * espace — la valeur de l argument, parmi les choix publies par le moteur.
     */
    function computeSlash() {
        if (!promptArea.text.startsWith("/") || win.slashDismissed) {
            if (win.slashRows.length)
                win.slashRows = [];
            win.slashRow = -1;
            glass.scheduleRefresh();
            return;
        }

        const text = promptArea.text;
        const body = text.slice(1);
        const cut = body.search(/\s/);
        const token = cut < 0 ? body : body.slice(0, cut);
        const arg = cut < 0 ? "" : body.slice(cut + 1).replace(/^\s+/, "");
        const lower = token.toLowerCase();

        const rows = [];
        const option = win.slashChoices(token);
        // Le nom du reglage est ecrit ET suivi d'un blanc : on en est a l'argument.
        const argument = !!option && (arg.length > 0 || cut >= 0);

        if (argument) {
            const wanted = arg.toLowerCase();
            for (let i = 0; i < option.options.length && rows.length < win.slashMaxRows; ++i) {
                const pick = option.options[i];
                const label = pick.name || pick.value;
                if (label.toLowerCase().indexOf(wanted) < 0
                    && pick.value.toLowerCase().indexOf(wanted) < 0)
                    continue;
                rows.push({
                    command: "/" + token + " " + pick.value,
                    insert: "/" + token + " " + pick.value,
                    name: label,
                    description: label === pick.value ? "" : pick.value,
                    hint: pick.value === option.currentValue ? "" : "",
                    current: pick.value === option.currentValue,
                });
            }
        } else if (!arg.length) {
            // Le nom de la commande, tant qu'aucun argument n'est ecrit.
            // (Une commande qui n'est pas un reglage publie n'a pas de valeurs a
            // proposer : son argument est libre — et si on proposait quand meme, le
            // completage effacerait ce que l'utilisateur vient d'ecrire.)
            const seen = {};
            const candidates = win.slashOwn().concat(hub.commands);
            for (let i = 0; i < candidates.length && rows.length < win.slashMaxRows; ++i) {
                const entry = candidates[i];
                if (!entry.name || seen[entry.name])
                    continue;
                seen[entry.name] = true;
                if (lower.length && entry.name.toLowerCase().indexOf(lower) !== 0)
                    continue;
                rows.push({
                    command: "/" + entry.name,
                    // Une commande qui attend un argument se complete d un blanc :
                    // c est lui qui ouvre la liste des valeurs.
                    insert: "/" + entry.name + (entry.hint ? " " : ""),
                    name: entry.name,
                    description: entry.description || "",
                    hint: entry.hint || "",
                    current: false,
                });
            }
        }

        // La premiere ligne est toujours surlignee : Tab ou Entree n'ont plus qu'a
        // choisir, et les fleches déplacent ce choix.
        win.slashRows = rows;
        win.slashRow = rows.length ? 0 : -1;
        glass.scheduleRefresh();
    }

    // Ce qui est ecrit est-il deja la commande entiere ? (alors Entree envoie)
    function slashComplete() {
        const rows = win.slashRows;
        if (!rows.length)
            return false;
        const row = rows[win.slashRow >= 0 ? win.slashRow : 0];
        return promptArea.text.trim().toLowerCase() === row.command.toLowerCase();
    }

    // Deplacer le choix dans la liste. Rend false si la liste n'existe pas : les
    // fleches restent alors dans le texte de la saisie.
    function slashStep(delta) {
        const count = win.slashRows.length;
        if (!count)
            return false;
        let next = win.slashRow < 0 ? 0 : win.slashRow + delta;
        if (next < 0)
            next = count - 1;
        if (next >= count)
            next = 0;
        win.slashRow = next;
        return true;
    }

    // Ecrire la ligne choisie dans la barre.
    function slashApply(row) {
        const rows = win.slashRows;
        if (row < 0 || row >= rows.length)
            return;
        promptArea.text = rows[row].insert;
        promptArea.cursorPosition = promptArea.text.length;
        win.computeSlash();
    }

    // Effacer les suggestions sans toucher a la saisie (Echap).
    function slashCollapse() {
        win.slashRows = [];
        win.slashRow = -1;
        win.slashDismissed = true;
        glass.scheduleRefresh();
    }

    /*
     * Une saisie qui commence par / : rend true quand c'est le corps qui traite,
     * false quand le texte doit partir au pont tel quel.
     */
    function slashDispatch(text) {
        const parts = text.slice(1).split(/\s+/).filter((part) => part.length);
        const name = parts.length ? parts[0] : "";
        const argument = parts.slice(1).join(" ");

        if (name === "new") {
            win.newConversation();
            promptArea.text = "";
            win.computeSlash();
            return true;
        }
        if (name === "voice") {
            win.reglerVoix(argument);
            promptArea.text = "";
            win.computeSlash();
            return true;
        }
        if (name === "fenetre") {
            if (!argument.length) {
                // Le mode seul : on ne le devine pas. On laisse la saisie en place et
                // on ouvre les trois modes.
                promptArea.text = "/fenetre ";
                win.computeSlash();
                return true;
            }
            win.reglerFenetre(argument);
            promptArea.text = "";
            win.computeSlash();
            return true;
        }
        if (win.slashSettings.indexOf(name) < 0)
            return false;

        if (!argument.length) {
            // Le reglage sans choix : on ne regle rien soi-meme. On laisse la
            // saisie en place et on ouvre la liste des valeurs publieses.
            promptArea.text = "/" + name + " ";
            win.computeSlash();
            return true;
        }

        hub.setConfigOption(name, argument);
        promptArea.text = "";
        win.computeSlash();
        return true;
    }

    /*
     * Un tour avec le hub : la bulle user part tout de suite, la carte agent
     * n'apparait qu'au premier chunk et grandit avec lui (le verre suit).
     */
    function send() {
        const text = promptArea.text.trim();
        if (!text.length)
            return;

        // Une commande rendue par le corps ne part pas au moteur et n'ouvre pas
        // de bulle : elle se dit dans la carte du hub (le statut).
        if (text.startsWith("/") && win.slashDispatch(text))
            return;

        promptArea.text = "";

        // Du plus recent au plus vieux : la bulle user d'abord, la reponse
        // agent par-dessus, donc la reponse se cale tout contre la barre.
        bubbleModel.insert(0, { role: "user", text: text, msgId: "", blocks: "[]", parsed: false });
        bubblesView.followNewest();

        hub.send(text);
    }

    /*
     * Une bulle fermee (le message suivant est ouvert, ou le tour a termine)
     * part au parseur hors-UI : ses blocs reviendront par applyBlocks(), et elle
     * basculera en rendu par blocs a ce moment-la. Pendant le streaming, la bulle
     * reste en texte brut — on ne reparse pas a chaque chunk.
     */
    function finalizeBubble(row) {
        if (row < 0 || row >= bubbleModel.count)
            return;
        const bubble = bubbleModel.get(row);
        const msgId = bubble.msgId || "";
        if (bubble.role !== "agent" || !bubble.text.length || !msgId.length || bubble.parsed)
            return;
        markdown.parse(msgId, bubble.text);
    }

    function appendChunk(messageId, role, text) {
        // Un messageId = une bulle. Le texte d un meme message s y accole (le
        // direct du tour), et le replay d une session reprise ouvre une bulle par
        // message, du plus vieux au plus recent : insert(0) tient la route.
        const sameMessage = streamRow >= 0 && (messageId.length === 0
            ? streamMessageId.length === 0
            : streamMessageId === messageId);

        if (sameMessage) {
            const current = bubbleModel.get(streamRow).text;
            bubbleModel.setProperty(streamRow, "text", current.length ? current + text : text);
            bubblesView.followNewest();
            return;
        }

        // La bulle precedente ferme sa vie ici : elle part au parseur.
        finalizeBubble(streamRow);

        // Notre propre message, deja pose par send() : si le pont nous le renvoie,
        // on ne le duplique pas, on s y rattache juste.
        if (role === "user" && bubbleModel.count > 0 && bubblesView.atNewest
            && bubbleModel.get(0).role === "user" && bubbleModel.get(0).text === text) {
            streamRow = 0;
            streamMessageId = messageId;
            return;
        }

        bubbleModel.insert(0, { role: role, text: text, msgId: messageId, blocks: "[]", parsed: false });
        streamRow = 0;
        streamMessageId = messageId;
        bubblesView.followNewest();
    }

    function endTurn(stopReason) {
        if (streamRow >= 0) {
            if (!bubbleModel.get(streamRow).text.length)
                bubbleModel.remove(streamRow);
            else
                finalizeBubble(streamRow);
            streamRow = -1;
        }
        streamMessageId = "";
        settleTimer.start();
    }

    /* Les blocs venus du parseur (hors UI) rattrapent leur bulle par son messageId. */
    function applyBlocks(messageId, blocks) {
        for (let i = 0; i < bubbleModel.count; ++i) {
            const bubble = bubbleModel.get(i);
            if ((bubble.msgId || "") === messageId) {
                bubbleModel.setProperty(i, "blocks", JSON.stringify(blocks || []));
                bubbleModel.setProperty(i, "parsed", true);
                glass.scheduleRefresh();
                return;
            }
        }
    }

    /* Vider la conversation : changement de session ou rupture, le modele
       repart de zero (l'historique rejoue reconstitue ce qui l'a cree). */
    function resetConversation() {
        bubbleModel.clear();
        streamRow = -1;
        streamMessageId = "";
        // Le buffer local de la voix suit la conversation : ce qui etait en train
        // de preparer un chunk ne doit pas partir dans la session d'apres.
        voice.resetBuffer();
        settleTimer.start();
    }

    /* Repartir d une conversation vierge : le modele se vide avec la session. */
    function newConversation() {
        resetConversation();
        hub.startFreshConversation();
    }

    /*
     * M3e — /voice on, /voice off, /voice (bascule), comme la v1. Couper la voix
     * interrompt aussi le tour en cours : sans quoi du texte continuerait a
     * arriver, et repartirait dans la voix suivante.
     */
    function reglerVoix(argument) {
        const a = (argument || "").trim().toLowerCase();
        if (a === "on" || a === "1" || a === "true")
            voice.turnOn();
        else if (a === "off" || a === "0" || a === "false")
            voice.turnOff();
        else
            voice.toggle();

        if (!voice.on && hub.generating)
            hub.cancelTurn();
    }

    /*
     * M3f — les trois modes de fenetre : /fenetre <mode> dans la barre, ou un clic
     * qui les fait tourner. Le corps persiste le choix (window/mode) ; voir
     * GlassHelper::setWindowMode pour ce qui s'applique tout de suite et ce qui
     * attend la prochaine ouverture.
     */
    readonly property var fenetreModes: ["fenetre", "au-dessus", "fixe"]

    function reglerFenetre(argument) {
        if (argument && argument.length) {
            glass.setWindowMode(argument);
            return;
        }
        const index = win.fenetreModes.indexOf(glass.windowMode);
        glass.setWindowMode(win.fenetreModes[(index + 1) % win.fenetreModes.length]);
    }

    Connections {
        target: hub
        // Le texte du tour arrive chunk par chunk — c'est le chemin du vrai
        // streaming, celui qui fait grandir la carte et bouger le verre. Une
        // session reprise passe par le meme chemin : le pont rejoue l historique.
        function onChunkReceived(messageId, role, text) { win.appendChunk(messageId, role, text); }
        function onTurnEnded(stopReason) { win.endTurn(stopReason); }
        function onHistoryReset() { win.resetConversation(); }
    }

    Connections {
        // Le parseur vit en dehors du hub : ses blocs parses reviennent par sa
        // propre connexion et rattrapent leur bulle hors du chemin du pont.
        target: markdown
        function onParsed(messageId, blocks) { win.applyBlocks(messageId, blocks); }
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

    /*
     * Test M3d/M3e (AICE_SLASH="/mo,/model ") : ecrire dans la barre sans clavier,
     * une entree toutes les 6 s une fois la session ouverte, et regarder ce que les
     * suggestions deviennent. Une entree prefixed par "@" est dispatchee comme une
     * vraie saisie de la barre : c'est la seule facon de tester /voice en plein tour.
     */
    Timer {
        id: slashTestTimer
        running: win.slashFed < slashInputs.length
        interval: 6000
        repeat: true
        onTriggered: {
            const input = slashInputs[win.slashFed];
            ++win.slashFed;

            if (input.startsWith("@")) {
                const saisie = input.slice(1);
                console.log("dispatch " + JSON.stringify(saisie) + " -> "
                            + String(win.slashDispatch(saisie)));
                return;
            }

            win.slashDismissed = false;
            promptArea.text = input;
            win.computeSlash();
            const names = win.slashRows.map((row) => row.command);
            console.log("slash " + JSON.stringify(input) + " -> "
                        + JSON.stringify(names) + " complet " + win.slashComplete());
            promptArea.text = "";
            win.computeSlash();
        }
    }

    Component.onCompleted: {
        // Overlay, flou, mesure des frames : le verre est C++ (KWin + QRHI).
        glass.attach(win);
        // Les bulles poussees hors de la zone de conversation sont exclues du verre.
        glass.setClipItem(bubblesView)
        if (glass.stress) {
            stressSeed(2000);
            // Le markdown complet, sans hub : pour regarder le rendu des blocs.
            seedMarkdown();
        }
        promptArea.forceActiveFocus();
        win.visible = true;
        settleTimer.start();
        hub.connectHub();
    }
}
