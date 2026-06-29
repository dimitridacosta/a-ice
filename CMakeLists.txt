cmake_minimum_required(VERSION 3.16)
project(a-ice VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Plasma SDK (KF5)
find_package(ECM REQUIRED NO_MODULE PATHS /usr/share/ECM/cmake)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
find_package(KF5 REQUIRED COMPONENTS Plasma CoreAddons)
find_package(Qt5 REQUIRED COMPONENTS Widgets Network)

# Main executable (standalone + applet)
add_executable(a-ice
    src/main.cpp
    src/AiceApplet.h
    src/AiceApplet.cpp
    src/ChatMessage.h
    src/ChatWidget.h
    src/ChatWidget.cpp
    src/llama_client.h
    src/llama_client.cpp
    src/Config.h
    src/Config.cpp
)

target_link_libraries(a-ice
    KF5::Plasma
    Qt5::Widgets
    Qt5::Network
)

target_include_directories(a-ice PRIVATE
    ${KF5Plasma_INCLUDES}
    ${Qt5Widgets_INCLUDE_DIRS}
    ${Qt5Network_INCLUDE_DIRS}
)

# Installation (appelable après build: sudo cmake --install .)
install(TARGETS a-ice DESTINATION bin)
install(FILES metadata.xml aice.desktop DESTINATION share/plasma/quickapplets/a-ice)
install(FILES metadata.xml aice.desktop DESTINATION share/plasma/services)
install(FILES config.example.json DESTINATION share/a-ice)
