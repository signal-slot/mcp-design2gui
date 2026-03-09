# Copyright (C) 2025 Signal Slot Inc.
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.16)
project(mcp-design2gui LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Network McpServer PsdCore PsdGui PsdExporter PsdImporter)

qt_standard_project_setup()

if(DEPS_INCLUDE_DIR)
    include_directories(BEFORE SYSTEM ${DEPS_INCLUDE_DIR})
endif()

qt_add_executable(mcp-design2gui
    main.cpp
)

set_target_properties(mcp-design2gui PROPERTIES
    WIN32_EXECUTABLE FALSE
    MACOSX_BUNDLE FALSE
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

target_link_libraries(mcp-design2gui PRIVATE
    Qt::Core
    Qt::Gui
    Qt::Network
    Qt::McpCommon
    Qt::McpServer
    Qt::PsdCore
    Qt::PsdGui
    Qt::PsdExporter
    Qt::PsdImporter
)

if(TARGET Qt6::QMcpServerStdioPlugin)
    qt_import_plugins(mcp-design2gui INCLUDE Qt6::QMcpServerStdioPlugin)
endif()

install(TARGETS mcp-design2gui
    RUNTIME DESTINATION bin
    BUNDLE DESTINATION bin
)
