cmake_minimum_required(VERSION 3.21)

project(pist
    VERSION 0.1.0
    DESCRIPTION "An IDE for Atari ST assembly development"
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
    src/build/LineMap.cpp
    src/build/LineMap.h

    src/emu/EmulatorHost.cpp
    src/emu/EmulatorHost.h
    src/emu/HatariProbe.cpp
    src/emu/HatariProbe.h
    src/emu/MachineState.h
    src/emu/Paths.cpp
    src/emu/Paths.h
    src/emu/SessionConfig.cpp
    src/emu/SessionConfig.h
    src/emu/TosRom.cpp
    src/emu/TosRom.h

    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/RegistersView.cpp
    src/ui/RegistersView.h
    src/ui/DisassemblyView.cpp
    src/ui/DisassemblyView.h
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
)
target_include_directories(tst_parsers PRIVATE src)
target_link_libraries(tst_parsers PRIVATE Qt6::Test)
add_test(NAME parsers COMMAND tst_parsers)

# Integration test: drives a real Hatari. Skips when its prerequisites are absent.
qt_add_executable(tst_emulatorhost
    tests/tst_emulatorhost.cpp
    src/emu/EmulatorHost.cpp
    src/emu/EmulatorHost.h
    src/emu/HatariProbe.cpp
    src/emu/HatariProbe.h
    src/emu/Paths.cpp
    src/emu/Paths.h
    src/emu/SessionConfig.cpp
    src/emu/SessionConfig.h
    src/emu/TosRom.cpp
    src/emu/TosRom.h
    src/emu/MachineState.h
)
target_include_directories(tst_emulatorhost PRIVATE src)
target_link_libraries(tst_emulatorhost PRIVATE Qt6::Test Qt6::Network)
add_test(NAME emulatorhost COMMAND tst_emulatorhost)
set_tests_properties(emulatorhost PROPERTIES TIMEOUT 300)
