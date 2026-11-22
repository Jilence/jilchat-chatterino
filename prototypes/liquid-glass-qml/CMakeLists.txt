cmake_minimum_required(VERSION 3.21)
project(LiquidGlassQml LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Network Qml Quick)
qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(liquidglass
    main.cpp
    TwitchChat.cpp TwitchChat.hpp
    FrameStats.cpp FrameStats.hpp
)

# Shaders are precompiled to .qsb (see compile-shaders.ps1), so the build
# does not need Qt Shader Tools.
qt_add_qml_module(liquidglass
    URI LiquidGlass
    VERSION 1.0
    QML_FILES Main.qml Glass.qml
    RESOURCES shaders/glass.vert.qsb shaders/glass.frag.qsb
)

# GuiPrivate only for rhi/qrhi.h (GPU frame timestamps in FrameStats).
target_link_libraries(liquidglass PRIVATE Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::Network Qt6::Qml Qt6::Quick)
