cmake_minimum_required(VERSION 3.16)
project(a-ice VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# KF6 / Qt6 (Plasma 6 / Bazzite)
find_package(ECM REQUIRED NO_MODULE PATHS /usr/share/ECM/cmake)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
find_package(KF6 REQUIRED COMPONENTS WindowSystem)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
find_package(LayerShellQt CONFIG REQUIRED)

# Activer MOC/AUTORCC pour Qt6.
set(CMAKE_AUTOMOC ON)

# Main executable (standalone + applet)
add_executable(a-ice
    src/main.cpp
    src/AiceApplet.h
    src/AiceApplet.cpp
    src/ChatMessage.h
    src/ChatMessage.cpp
    src/ChatWidget.h
    src/ChatWidget.cpp
    src/Bubble.h
    src/Bubble.cpp
    src/llama_client.h
    src/llama_client.cpp
    src/Config.h
    src/Config.cpp
    src/Tool.h
    src/ToolRegistry.h
    src/ToolRegistry.cpp
    src/tools/TerminalTool.h
    src/tools/TerminalTool.cpp
    src/tools/TerminalSafety.h
    src/tools/TerminalSafety.cpp
    src/tools/BraveSearchTool.h
    src/tools/BraveSearchTool.cpp
    src/tools/WebFetchTool.h
    src/tools/WebFetchTool.cpp
)

target_link_libraries(a-ice
    KF6::WindowSystem
    LayerShellQt::Interface
    Qt6::Widgets
    Qt6::Network
)

# Installation (appelable après build: sudo cmake --install .)
install(TARGETS a-ice DESTINATION bin)
install(FILES metadata.xml aice.desktop DESTINATION share/plasma/quickapplets/a-ice)
install(FILES metadata.xml aice.desktop DESTINATION share/plasma/services)
install(FILES config.example.json SOUL.example.md DESTINATION share/a-ice)

# --- Test unitaire hardline -------------------------------------------------
# Link uniquement TerminalSafety.cpp + Qt6::Core : le terminal (QProcess) n'est
# JAMAIS compile dans ce test -> physiquement incapable d'executer une commande.
add_executable(test_hardline
    tests/test_hardline.cpp
    src/tools/TerminalSafety.h
    src/tools/TerminalSafety.cpp
)
target_link_libraries(test_hardline PRIVATE Qt6::Core)
