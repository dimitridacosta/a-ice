# a-ice 2.0 — M1+M2 : le corps en Qt Quick, branche sur le hub zcode-acp.
# Le v1 (Qt Widgets) n'est pas touche : ce binaire sert a valider le verre
# (flou KWin par carte, barre attrapable, liste virtualisee) puis le client hub.

find_package(Qt6 REQUIRED COMPONENTS Quick Network WebSockets)

add_executable(a-ice-m1
    main.cpp
    GlassHelper.h
    GlassHelper.cpp
    HubClient.h
    HubClient.cpp
)

qt_add_resources(a-ice-m1 "aice_m1"
    PREFIX "/poc"
    FILES
        qml/Main.qml
)

target_link_libraries(a-ice-m1 PRIVATE
    Qt6::Quick
    Qt6::Network
    Qt6::WebSockets
    KF6::WindowSystem
    LayerShellQt::Interface
)
