# a-ice 2.0 — M1 : POC du corps (Qt Quick), hors moteur agentique.
# Le v1 (Qt Widgets) n'est pas touche : ce binaire sert uniquement a valider
# le flou KWin par carte, la barre attrapable et la liste de bulles virtualisee.

find_package(Qt6 REQUIRED COMPONENTS Quick)

add_executable(a-ice-m1
    main.cpp
    GlassHelper.h
    GlassHelper.cpp
)

qt_add_resources(a-ice-m1 "aice_m1"
    PREFIX "/poc"
    FILES
        qml/Main.qml
)

target_link_libraries(a-ice-m1 PRIVATE
    Qt6::Quick
    KF6::WindowSystem
    LayerShellQt::Interface
)
