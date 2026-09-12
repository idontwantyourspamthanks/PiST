cmake_minimum_required(VERSION 3.21)

project(pist
    VERSION 0.1.0
    DESCRIPTION "PiST - an IDE for Atari ST assembly development"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network)

qt_standard_project_setup()

qt_add_executable(pist
    src/main.cpp

    src/editor/AsmHighlighter.cpp
    src/editor/AsmHighlighter.h
    src/editor/CodeEditor.cpp
    src/editor/CodeEditor.h

    src/build/BuildService.cpp
    src/build/BuildService.h
    src/build/Diagnostic.h

    src/debug/Breakpoint.cpp
    src/debug/Breakpoint.h
    src/build/LineMap.cpp
    src/build/LineMap.h

    src/emu/EmulatorHost.cpp
    src/emu/EmulatorHost.h
    src/emu/HatariProbe.cpp
    src/emu/HatariProbe.h
    src/emu/MachineState.h
    src/emu/MemoryDump.cpp
    src/emu/MemoryDump.h
    src/emu/Paths.cpp
    src/emu/Paths.h
    src/emu/SessionConfig.cpp
    src/emu/SessionConfig.h
    src/project/ProjectSettings.cpp
    src/project/ProjectSettings.h
    src/emu/Machine.cpp
    src/emu/Machine.h
    src/emu/TosRom.cpp
    src/emu/TosRom.h

    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/RegistersView.cpp
    src/ui/RegistersView.h
    src/ui/BreakpointPanel.cpp
    src/ui/BreakpointPanel.h
    src/ui/DisassemblyView.cpp
    src/ui/DisassemblyView.h
    src/ui/MemoryView.cpp
    src/ui/MemoryView.h
    src/ui/SettingsDialog.cpp
    src/ui/SettingsDialog.h
)

target_include_directories(pist PRIVATE src)

target_link_libraries(pist PRIVATE
    Qt6::Widgets
    Qt6::Network
)

install(TARGETS pist
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

find_package(Qt6 REQUIRED COMPONENTS Test)

qt_add_executable(tst_parsers
    tests/tst_parsers.cpp
    src/build/LineMap.cpp
    src/build/LineMap.h
    src/build/Diagnostic.h

    src/debug/Breakpoint.cpp
    src/debug/Breakpoint.h
)
target_include_directories(tst_parsers PRIVATE src)
target_link_libraries(tst_parsers PRIVATE Qt6::Test)
add_test(NAME parsers COMMAND tst_parsers)

# TOS ROM identification: version must come from the image header, so the
# autostart guard cannot be skipped by an unusual filename.
qt_add_executable(tst_tosrom
    tests/tst_tosrom.cpp
    src/project/ProjectSettings.cpp
    src/project/ProjectSettings.h
    src/emu/Machine.cpp
    src/emu/Machine.h
    src/emu/TosRom.cpp
    src/emu/TosRom.h
    src/emu/Paths.cpp
    src/emu/Paths.h
)
target_include_directories(tst_tosrom PRIVATE src)
target_link_libraries(tst_tosrom PRIVATE Qt6::Test)
add_test(NAME tosrom COMMAND tst_tosrom)

# Breakpoint resolution and memory-dump parsing: pure functions, no emulator.
qt_add_executable(tst_debug
    tests/tst_debug.cpp
    src/debug/Breakpoint.cpp
    src/debug/Breakpoint.h
    src/build/LineMap.cpp
    src/build/LineMap.h
    src/build/Diagnostic.h
    src/emu/MemoryDump.cpp
    src/emu/MemoryDump.h
)
target_include_directories(tst_debug PRIVATE src)
target_link_libraries(tst_debug PRIVATE Qt6::Test)
add_test(NAME debug COMMAND tst_debug)

# Project settings persistence.
qt_add_executable(tst_settings
    tests/tst_settings.cpp
    src/project/ProjectSettings.cpp
    src/project/ProjectSettings.h
    src/emu/Machine.cpp
    src/emu/Machine.h
    src/emu/TosRom.cpp
    src/emu/TosRom.h
    src/emu/Paths.cpp
    src/emu/Paths.h
)
target_include_directories(tst_settings PRIVATE src)
target_link_libraries(tst_settings PRIVATE Qt6::Test)
add_test(NAME settings COMMAND tst_settings)

# GUI-level: constructs MainWindow and verifies the line map against a real
# vasm listing, covering the PC <-> source-line claim end to end.
qt_add_executable(tst_gui
    tests/tst_gui.cpp
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/RegistersView.cpp
    src/ui/RegistersView.h
    src/ui/BreakpointPanel.cpp
    src/ui/BreakpointPanel.h
    src/ui/BreakpointPanel.cpp
    src/ui/BreakpointPanel.h
    src/ui/DisassemblyView.cpp
    src/ui/DisassemblyView.h
    src/ui/MemoryView.cpp
    src/ui/MemoryView.h
    src/ui/SettingsDialog.cpp
    src/ui/SettingsDialog.h
    src/project/ProjectSettings.cpp
    src/project/ProjectSettings.h
    src/editor/CodeEditor.cpp
    src/editor/CodeEditor.h
    src/editor/AsmHighlighter.cpp
    src/editor/AsmHighlighter.h
    src/build/BuildService.cpp
    src/build/BuildService.h
    src/build/Diagnostic.h
    src/build/LineMap.cpp
    src/build/LineMap.h
    src/debug/Breakpoint.cpp
    src/debug/Breakpoint.h
    src/emu/EmulatorHost.cpp
    src/emu/EmulatorHost.h
    src/emu/HatariProbe.cpp
    src/emu/HatariProbe.h
    src/emu/MachineState.h
    src/emu/MemoryDump.cpp
    src/emu/MemoryDump.h
    src/emu/Paths.cpp
    src/emu/Paths.h
    src/emu/SessionConfig.cpp
    src/emu/SessionConfig.h
    src/project/ProjectSettings.cpp
    src/project/ProjectSettings.h
    src/emu/Machine.cpp
    src/emu/Machine.h
    src/emu/TosRom.cpp
    src/emu/TosRom.h
)
target_include_directories(tst_gui PRIVATE src)
target_link_libraries(tst_gui PRIVATE Qt6::Test Qt6::Widgets Qt6::Network)
add_test(NAME gui COMMAND tst_gui)
set_tests_properties(gui PROPERTIES TIMEOUT 180)

# Integration test: drives a real Hatari. Skips when its prerequisites are absent.
qt_add_executable(tst_emulatorhost
    tests/tst_emulatorhost.cpp
    src/build/LineMap.cpp
    src/build/LineMap.h
    src/build/Diagnostic.h
    src/debug/Breakpoint.cpp
    src/debug/Breakpoint.h
    src/emu/EmulatorHost.cpp
    src/emu/EmulatorHost.h
    src/emu/HatariProbe.cpp
    src/emu/HatariProbe.h
    src/emu/Paths.cpp
    src/emu/Paths.h
    src/emu/SessionConfig.cpp
    src/emu/SessionConfig.h
    src/project/ProjectSettings.cpp
    src/project/ProjectSettings.h
    src/emu/Machine.cpp
    src/emu/Machine.h
    src/emu/TosRom.cpp
    src/emu/TosRom.h
    src/emu/MachineState.h
    src/emu/MemoryDump.cpp
    src/emu/MemoryDump.h
)
target_include_directories(tst_emulatorhost PRIVATE src)
target_link_libraries(tst_emulatorhost PRIVATE Qt6::Test Qt6::Network)
add_test(NAME emulatorhost COMMAND tst_emulatorhost)
set_tests_properties(emulatorhost PROPERTIES TIMEOUT 300)
