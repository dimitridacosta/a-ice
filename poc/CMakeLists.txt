# a-ice 2.0 — POC UI Qt Quick (refonte, hors Qt Widgets)
find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2)

set(CMAKE_AUTORCC ON)

qt_add_resources(a-ice-poc "poc"
    PREFIX "/poc"
    FILES qml/Main.qml
)

add_executable(a-ice-poc
    main.cpp
)

target_link_libraries(a-ice-poc
    Qt6::Quick
    Qt6::QuickControls2
    KF6::WindowSystem
    LayerShellQt::Interface
)