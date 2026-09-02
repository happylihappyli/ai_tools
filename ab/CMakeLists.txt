cmake_minimum_required(VERSION 3.16)
project(ab VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Qt 5 (兼容 6)
find_package(Qt5 COMPONENTS Core Gui Widgets Network REQUIRED)
if(NOT Qt5_FOUND)
    find_package(Qt6 COMPONENTS Core Gui Widgets Network REQUIRED)
    set(QT_VERSION_MAJOR 6)
else()
    set(QT_VERSION_MAJOR 5)
endif()

# nlohmann/json (header-only, 找系统或 git submodule)
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    # 内联简化版 json 解析 — 避免依赖下载
    # 见 src/AbJson.h (自己实现)
    add_compile_definitions(AB_USE_BUILTIN_JSON=1)
endif()

set(AB_SOURCES
    src/main.cpp
    src/AbMainWindow.cpp
    src/AbConfig.cpp
    src/AbTheme.cpp
    src/AbTaskRunner.cpp
    src/AbLogDock.cpp
    src/AbJson.cpp
)

set(AB_HEADERS
    src/AbMainWindow.h
    src/AbConfig.h
    src/AbTheme.h
    src/AbTaskRunner.h
    src/AbLogDock.h
    src/AbJson.h
)

add_executable(ab ${AB_SOURCES} ${AB_HEADERS})

target_include_directories(ab PRIVATE src)

target_link_libraries(ab
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Gui
        Qt${QT_VERSION_MAJOR}::Widgets
)

if(nlohmann_json_FOUND)
    target_link_libraries(ab PRIVATE nlohmann_json::nlohmann_json)
endif()

# 安装
install(TARGETS ab RUNTIME DESTINATION bin)

# 复制 QSS 到 build 目录 (2026-09-02: 用 EXISTS 守卫, QSS 文件可选)
# 实际 QSS 内嵌在 AbTheme.cpp, 这里只是给想覆盖默认配色的用户用的扩展点
if(EXISTS ${CMAKE_SOURCE_DIR}/ui/ab_dark.qss)
    configure_file(${CMAKE_SOURCE_DIR}/ui/ab_dark.qss
                   ${CMAKE_BINARY_DIR}/ab_dark.qss COPYONLY)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/ui/ab_light.qss)
    configure_file(${CMAKE_SOURCE_DIR}/ui/ab_light.qss
                   ${CMAKE_BINARY_DIR}/ab_light.qss COPYONLY)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/ui/ab_solarized.qss)
    configure_file(${CMAKE_SOURCE_DIR}/ui/ab_solarized.qss
                   ${CMAKE_BINARY_DIR}/ab_solarized.qss COPYONLY)
endif()
if(EXISTS ${CMAKE_SOURCE_DIR}/ui/ab_nord.qss)
    configure_file(${CMAKE_SOURCE_DIR}/ui/ab_nord.qss
                   ${CMAKE_BINARY_DIR}/ab_nord.qss COPYONLY)
endif()
