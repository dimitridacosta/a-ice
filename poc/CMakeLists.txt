# a-ice 2.0 — M1+M2 : le corps en Qt Quick, branche sur le hub zcode-acp.
# Le v1 (Qt Widgets) n'est pas touche : ce binaire sert a valider le verre
# (flou KWin par carte, barre attrapable, liste virtualisee) puis le client hub.

find_package(Qt6 REQUIRED COMPONENTS Quick Gui Network WebSockets)

add_executable(a-ice-m1
    main.cpp
    GlassHelper.h
    GlassHelper.cpp
    HubClient.h
    HubClient.cpp
    Markdown.h
    Markdown.cpp
    VoiceControl.h
    VoiceControl.cpp
    # M3e : la voix de la v1, reprise TELLE QUELLE (src/voice/) — chunker adaptatif,
    # filtre « ce qui peut etre dit », verrou anti-avalanche, coupe immediate. Le POC
    # n'en fait qu'un pont (VoiceControl) : il n'y a pas deux voix dans a-ice.
    ../src/voice/VoiceStream.h
    ../src/voice/VoiceStream.cpp
)

qt_add_resources(a-ice-m1 "aice_m1"
    PREFIX "/poc"
    FILES
        qml/Main.qml
)

# --- Test unitaire du decoupage markdown (M3a) -------------------------------
# Le decoupage est une fonction libre : le test la teste sans hub et sans corps.
# Qt6::Gui pour QTextDocumentFragment::fromMarkdown. Run : ./test_md (et ./test_md --dump
# pour lire le HTML que Qt rend).
add_executable(test_md
    ../tests/test_md.cpp
    Markdown.h
    Markdown.cpp
)

target_link_libraries(test_md PRIVATE Qt6::Core Qt6::Gui)

target_link_libraries(a-ice-m1 PRIVATE
    Qt6::Quick
    Qt6::Network
    Qt6::WebSockets
    KF6::WindowSystem
    LayerShellQt::Interface
)
