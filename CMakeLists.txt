cmake_minimum_required(VERSION 3.21)

project(mmts_edit_gui VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(mmts-edit-gui WIN32
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/MmtsMap.cpp
    src/MmtsMap.h
    src/TimelineWidget.cpp
    src/TimelineWidget.h
)

target_link_libraries(mmts-edit-gui PRIVATE Qt6::Widgets)
