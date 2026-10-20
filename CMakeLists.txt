cmake_minimum_required(VERSION 3.20)
project(DriftTable LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "DriftTable is a Windows application (build with MSVC or MinGW-w64).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(DriftTable WIN32
    src/main.cpp
    src/App.cpp
    src/AppBoard.cpp
    src/AppDraw.cpp
    src/AppInput.cpp
    src/Board.cpp
    src/DesktopHost.cpp
    src/ImageLoader.cpp
    src/Renderer.cpp
    src/ShellOps.cpp
    res/DriftTable.rc
)

target_compile_definitions(DriftTable PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)

if(MSVC)
    # Sources contain Russian UI strings in UTF-8.
    target_compile_options(DriftTable PRIVATE /utf-8 /W4 /permissive-)
    # The manifest comes from the .rc file.
    target_link_options(DriftTable PRIVATE /MANIFEST:NO)
else()
    target_compile_options(DriftTable PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_options(DriftTable PRIVATE -municode -static)
endif()

target_link_libraries(DriftTable PRIVATE
    d2d1 dwrite windowscodecs dwmapi shell32 shlwapi ole32 oleaut32 uuid comctl32 gdi32 user32 advapi32)
