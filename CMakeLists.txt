cmake_minimum_required(VERSION 3.21)

project(pist
    VERSION 0.7.0
    DESCRIPTION "PiST - an IDE for Atari ST assembly development"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# An installed binary must be able to find its shared libraries when installed to
# a non-system prefix, which is the normal case for a local `cmake --install`.
# Without this the staged binary links but cannot start (observed on macOS as
# "Library not loaded: @rpath/QtWidgets.framework/..."). This uses the link-time
# library directories, so it works as long as the Qt installation stays put;
# self-contained deployment (macdeployqt, windeployqt) is separate work.
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network)

# X11 is needed only to map the embedded emulator's SDL window (see
# src/ui/EmbedX11.cpp). It is optional: without it the build still succeeds and
# the emulator simply runs as a separate window.
if(UNIX AND NOT APPLE)
    find_package(X11)
endif()

qt_standard_project_setup()

# Compiled once and linked into the app and the GUI tests. tst_gui and
# tst_remotecontrol used to list these files again, so macOS CI compiled
# MainWindow.cpp (and the sprite editor) three times and sat at ~55% on
# whichever huge translation unit make had drawn next.
qt_add_library(pist_core OBJECT
    src/editor/AsmHighlighter.cpp
    src/editor/AsmHighlighter.h
    src/editor/CodeEditor.cpp
    src/ui/Appearance.cpp
    src/ui/Appearance.h
    src/ui/Icons.cpp
    src/editor/CodeEditor.h
    src/editor/InstrRef.cpp
    src/editor/InstrRef.h
    src/editor/IncludeNav.cpp
    src/editor/IncludeNav.h
    src/editor/OsCallRef.cpp
    src/editor/OsCallRef.h
    src/editor/OsCallBinding.cpp
    src/editor/OsCallBinding.h
    src/editor/OsCallScan.cpp
    src/editor/OsCallScan.h

    src/build/BuildService.cpp
    src/build/BuildService.h
    src/build/Diagnostic.h
    src/emu/ProfileData.cpp
    src/emu/ProfileData.h
    src/build/SymbolTable.cpp
    src/build/SymbolTable.h
    src/build/FloppyImage.cpp
    src/build/FloppyImage.h

    src/control/RemoteControl.cpp
    src/control/RemoteControl.h

    src/debug/Breakpoint.cpp
    src/debug/Breakpoint.h
    src/debug/Watchpoint.h
    src/build/LineMap.cpp
    src/build/LineMap.h
    src/build/LinkMap.cpp
    src/build/LinkMap.h
    src/build/ProgramLineMap.cpp
    src/build/ProgramLineMap.h

    src/emu/EmulatorHost.cpp
    src/emu/EmulatorHost.h
    src/emu/DebugBackend.h
    src/emu/EmbedSocket.cpp
    src/emu/EmbedSocket.h
    src/emu/HrdbBackend.cpp
    src/emu/HrdbBackend.h
    src/emu/HatariTextParse.cpp
    src/emu/HatariTextParse.h
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
    src/toolchain/Toolchain.cpp
    src/toolchain/Toolchain.h
    src/toolchain/ToolFetch.cpp
    src/toolchain/ToolFetch.h
    src/emu/Machine.cpp
    src/emu/Machine.h
    src/emu/TosRom.cpp
    src/emu/TosRom.h

    src/image/Palette.cpp
    src/image/Palette.h
    src/image/ImageDocument.cpp
    src/image/ImageDocument.h
    src/image/Tools.cpp
    src/image/Tools.h
    src/image/StFormats.cpp
    src/image/StFormats.h
    src/image/Transform.cpp
    src/image/Transform.h

    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/RegistersView.cpp
    src/ui/RegistersView.h
    src/ui/BreakpointPanel.cpp
    src/ui/BreakpointPanel.h
    src/ui/BitplaneExportDialog.cpp
    src/ui/BitplaneExportDialog.h
    src/ui/DisassemblyView.cpp
    src/ui/DisassemblyView.h
    src/ui/EmulatorDisplayWidget.cpp
    src/ui/EmulatorDisplayWidget.h
    src/ui/EmbedX11.cpp
    src/ui/EmbedX11.h
    src/ui/FileBrowser.cpp
    src/ui/FileBrowser.h
    src/ui/HardwareView.cpp
    src/ui/HardwareView.h
    src/ui/MemoryView.cpp
    src/ui/MemoryView.h
    src/ui/ConsoleInput.cpp
    src/ui/ConsoleInput.h
    src/ui/PcHistoryView.cpp
    src/ui/PcHistoryView.h
    src/ui/InstructionRefView.cpp
    src/ui/InstructionRefView.h
    src/ui/SymbolsView.cpp
    src/ui/SymbolsView.h
    src/ui/ProfilerView.cpp
    src/ui/ProfilerView.h
    src/ui/StackView.cpp
    src/ui/StackView.h
    src/ui/SettingsDialog.cpp
    src/ui/SettingsDialog.h
    src/ui/SetupDialog.cpp
    src/ui/SetupDialog.h
    src/ui/ImageCanvas.cpp
    src/ui/SheetCanvas.cpp
    src/ui/ImageCanvas.h
    src/ui/SheetCanvas.h
    src/ui/ImageEditor.cpp
    src/ui/ImageEditor.h
    src/ui/PalettePickerDialog.cpp
    src/ui/PalettePickerDialog.h
    src/ui/NewImageDialog.cpp
    src/ui/NewImageDialog.h
    src/ui/AboutDialog.cpp
    src/ui/AboutDialog.h
)
target_include_directories(pist_core PUBLIC src)
target_link_libraries(pist_core PUBLIC
    Qt6::Widgets
    Qt6::Network
)
# Quoted so the preprocessor sees a string, not a floating-point token.
target_compile_definitions(pist_core PUBLIC "PIST_VERSION=\"${PROJECT_VERSION}\"")

if(X11_FOUND)
    target_compile_definitions(pist_core PUBLIC PIST_HAVE_X11)
    target_link_libraries(pist_core PUBLIC X11::X11)
    # X Shape (libXext) makes the embedded video input-transparent mid-drag.
    if(TARGET X11::Xext)
        target_compile_definitions(pist_core PUBLIC PIST_HAVE_XEXT)
        target_link_libraries(pist_core PUBLIC X11::Xext)
    endif()
endif()

qt_add_executable(pist
    src/main.cpp
)
target_link_libraries(pist PRIVATE pist_core)

# The MCP stdio shim: lets Claude/Cursor-class agents discover a running PiST
# as a native tool, bridging MCP JSON-RPC (stdio) to the RemoteControl TCP
# protocol. Console application everywhere — WIN32_EXECUTABLE would detach
# stdout and silently break the transport.
qt_add_executable(pist-mcp
    src/control/mcp/main.cpp
    src/control/mcp/McpServer.cpp
    src/control/mcp/McpServer.h
    src/control/mcp/ControlClient.cpp
    src/control/mcp/ControlClient.h
)
target_include_directories(pist-mcp PRIVATE src)
target_link_libraries(pist-mcp PRIVATE Qt6::Core Qt6::Network)
target_compile_definitions(pist-mcp PRIVATE "PIST_VERSION=\"${PROJECT_VERSION}\"")
set_target_properties(pist-mcp PROPERTIES WIN32_EXECUTABLE FALSE)

# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

# Deploy the Qt runtime alongside the binary: platform plugins, TLS backends and
# the other dlopened pieces Qt needs at run time. Qt finds these itself, so
# without them an installed or bundled binary starts and then fails to create a
# window — the failure mode that produced "Library not loaded" on macOS and blank
# platform plugin errors elsewhere. Qt 6.5+ provides this; older Qt needs
# windeployqt/macdeployqt/manual copies instead.
if(QT_VERSION VERSION_GREATER_EQUAL 6.5)
    qt_generate_deploy_app_script(
        TARGET pist
        OUTPUT_SCRIPT qt_deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR)
    install(SCRIPT ${qt_deploy_script})
endif()

if(APPLE)
    # A GUI application on macOS belongs in a bundle: it is what macdeployqt
    # operates on, carries the icon and Info.plist, and is what a user drags to
    # Applications. Without this the release packaging step failed with
    # "Could not find app bundle".
    set_target_properties(pist PROPERTIES MACOSX_BUNDLE TRUE)
endif()

install(TARGETS pist
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS pist-mcp
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Licence and third-party notices. The LGPL (Qt) and BSD obligations require
# these to travel with a binary distribution, and the NOTICE is also how a user
# learns which external programs they still need to supply.
install(FILES LICENSE NOTICE README.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR})

if(UNIX AND NOT APPLE)
    # Desktop entry and icon, so the IDE appears in application menus rather than
    # only being launchable from a terminal.
    install(FILES pist.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
    install(FILES packaging/pist.svg
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
        RENAME pist.svg)
endif()

# Relocatable package layout: a staged tree keeps its bundled Qt beside the
# binary, so an installed package resolves its libraries wherever it lands —
# /usr, /opt, or a user's home — rather than at the build host's Qt path.
# (qt_generate_deploy_app_script above places the Qt runtime in lib/.)
if(UNIX AND NOT APPLE)
    set_target_properties(pist PROPERTIES
        INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()

# ---------------------------------------------------------------------------
# Packaging (CPack): installers built from the same staged trees the release
# archives ship, so the installer and the tarball carry identical content —
# binary, bundled Qt runtime, assembler, emulator, ROM, licences and notices.
# The generator is chosen at package time (DEB/RPM on Linux, DragNDrop on
# macOS, WIX on Windows); CPACK_INSTALLED_DIRECTORIES overrides the tree and
# must be an absolute path (CMake 4 RelativePath).
# ---------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "pist")
set(CPACK_PACKAGE_VENDOR "PiST")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "An IDE for Atari ST assembly development")
set(CPACK_PACKAGE_CONTACT "https://github.com/idontwantyourspamthanks/PiST")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/idontwantyourspamthanks/PiST")

# Debian metadata. No computed shared-library dependencies: the package is
# self-contained by design (bundled Qt, emulator, assembler, ROM), and a
# distro Qt may be older than the 6.5 floor — a libqt6 dependency would be
# wrong here.
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "PiST contributors")
set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
    "IDE for Atari ST 68000 assembly development: editor, vasm integration,\n"
    " and debugging under the bundled Hatari emulator (breakpoints, stepping,\n"
    " registers, memory, disassembly, watchpoints).\n"
    " Self-contained: Qt runtime, Hatari (hrdb-main fork), vasm and EmuTOS\n"
    " are included.")

# RPM metadata. No auto-computed requires: same self-contained rationale.
set(CPACK_RPM_PACKAGE_LICENSE "GPL-2.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Development/Tools")
set(CPACK_RPM_PACKAGE_AUTOREQPROV OFF)

# A stable upgrade GUID is what lets one PiST MSI replace the previous one
# rather than installing side by side. Do not change it between releases.
set(CPACK_WIX_UPGRADE_GUID "7d7f9c1e-8a2b-4f3a-9e5d-1c6b2a0d4e8f")

set(CPACK_DMG_VOLUME_NAME "PiST")

include(CPack)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()

find_package(Qt6 REQUIRED COMPONENTS Test)

# Tests that read committed data from the source tree (the fetch pins in
# tst_toolfetch, the foreign floppy images in tst_parsers) need the path as a C
# string literal. Forward slashes are what QFile accepts on every platform; the
# native Windows path would embed escapes like \a and \P in the literal.
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}" PIST_SOURCE_DIR_CMAKE)

qt_add_executable(tst_parsers
    tests/tst_parsers.cpp
    src/build/LineMap.cpp
    src/build/LineMap.h
    src/build/LinkMap.cpp
    src/build/LinkMap.h
    src/build/ProgramLineMap.cpp
    src/build/ProgramLineMap.h
    src/build/BuildService.cpp
    src/build/FloppyImage.cpp
    src/build/FloppyImage.h
    src/build/BuildService.h
    src/build/Diagnostic.h

    src/debug/Breakpoint.cpp
    src/debug/Breakpoint.h
    src/debug/Watchpoint.h
)
target_include_directories(tst_parsers PRIVATE src)
target_link_libraries(tst_parsers PRIVATE Qt6::Test)
target_compile_definitions(tst_parsers PRIVATE PIST_SOURCE_DIR="${PIST_SOURCE_DIR_CMAKE}")
add_test(NAME parsers COMMAND tst_parsers)

# Sprite document, ST palettes, paint tools and PI1/NEO/MBK/PNG codecs.
# Always runs; no emulator or display required.
qt_add_executable(tst_image
    tests/tst_image.cpp
    src/image/Palette.cpp
    src/image/Palette.h
    src/image/ImageDocument.cpp
    src/image/ImageDocument.h
    src/image/Tools.cpp
    src/image/Tools.h
    src/image/StFormats.cpp
    src/image/StFormats.h
    src/image/Transform.cpp
    src/image/Transform.h
)
target_include_directories(tst_image PRIVATE src)
target_link_libraries(tst_image PRIVATE Qt6::Test Qt6::Gui)
add_test(NAME image COMMAND tst_image)

# The OS-call reference table and the trap-sequence scanner: pure QtCore
# code, so the suite needs no display and no emulator.
qt_add_executable(tst_oscall
    tests/tst_oscall.cpp
    src/editor/OsCallRef.cpp
    src/editor/OsCallRef.h
    src/editor/OsCallBinding.cpp
    src/editor/OsCallBinding.h
    src/editor/OsCallScan.cpp
    src/editor/OsCallScan.h
)
target_include_directories(tst_oscall PRIVATE src)
target_link_libraries(tst_oscall PRIVATE Qt6::Test)
add_test(NAME oscall COMMAND tst_oscall)

# TOS ROM identification: version must come from the image header, so the
# autostart guard cannot be skipped by an unusual filename.
qt_add_executable(tst_tosrom
    tests/tst_tosrom.cpp
    src/project/ProjectSettings.cpp
    src/project/ProjectSettings.h
    src/toolchain/Toolchain.cpp
    src/toolchain/Toolchain.h
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
    src/build/LinkMap.cpp
    src/build/LinkMap.h
    src/build/ProgramLineMap.cpp
    src/build/ProgramLineMap.h
    src/project/ProjectSettings.cpp
    src/project/ProjectSettings.h
    src/emu/Paths.cpp
    src/emu/Paths.h
    src/emu/Machine.cpp
    src/emu/Machine.h
    src/emu/TosRom.cpp
    src/emu/TosRom.h
    src/debug/Breakpoint.cpp
    src/debug/Breakpoint.h
    src/debug/Watchpoint.h
    src/build/LineMap.cpp
    src/build/LineMap.h
    src/build/Diagnostic.h
    src/emu/MemoryDump.cpp
    src/emu/MemoryDump.h
)
target_include_directories(tst_debug PRIVATE src)
target_link_libraries(tst_debug PRIVATE Qt6::Test)
add_test(NAME debug COMMAND tst_debug)

# Multi-module source mapping through a linker's map file. The end-to-end case
# runs a real vasm+vlink when both are available and skips otherwise, so CI
# without vlink still exercises the parsing and the address arithmetic.
qt_add_executable(tst_link
    tests/tst_link.cpp
    src/build/LinkMap.cpp
    src/build/LinkMap.h
    src/build/ProgramLineMap.cpp
    src/build/ProgramLineMap.h
    src/build/LineMap.cpp
    src/build/LineMap.h
    src/build/Diagnostic.h
)
target_include_directories(tst_link PRIVATE src)
target_link_libraries(tst_link PRIVATE Qt6::Test)
add_test(NAME link COMMAND tst_link)

# Project settings persistence.
qt_add_executable(tst_settings
    tests/tst_settings.cpp
    src/toolchain/Toolchain.cpp
    src/toolchain/Toolchain.h
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

# First-run fetch/install: checksum verification, archive extraction, and the
# claim that an installed tool lands where discovery actually looks. Needs no
# network — fixtures are fabricated locally; the vasm build case skips without
# make/tar.
qt_add_executable(tst_toolfetch
    tests/tst_toolfetch.cpp
    src/toolchain/ToolFetch.cpp
    src/toolchain/ToolFetch.h
    src/toolchain/Toolchain.cpp
    src/toolchain/Toolchain.h
    src/emu/Paths.cpp
    src/emu/Paths.h
)
target_include_directories(tst_toolfetch PRIVATE src)
target_link_libraries(tst_toolfetch PRIVATE Qt6::Test)
# The pin guard reads the workflows' copies of the fetch pins.
target_compile_definitions(tst_toolfetch PRIVATE PIST_SOURCE_DIR="${PIST_SOURCE_DIR_CMAKE}")
add_test(NAME toolfetch COMMAND tst_toolfetch)

# GUI-level: constructs MainWindow and verifies the line map against a real
# vasm listing, covering the PC <-> source-line claim end to end.
qt_add_executable(tst_gui
    tests/tst_gui.cpp
)
target_link_libraries(tst_gui PRIVATE pist_core Qt6::Test)

# The diagnose test runs the application binary, so it must exist first — without
# this it would skip silently in a parallel build, and a skipped test reads as a
# pass in the summary.
add_dependencies(tst_gui pist)
add_test(NAME gui COMMAND tst_gui)
set_tests_properties(gui PROPERTIES TIMEOUT 900)

# Remote-control socket protocol: framing, block terminators, and the synchronous
# command responses an agent depends on. Drives MainWindow offscreen, needs no
# display or emulator.
qt_add_executable(tst_remotecontrol
    tests/tst_remotecontrol.cpp
)
target_link_libraries(tst_remotecontrol PRIVATE pist_core Qt6::Test)
add_dependencies(tst_remotecontrol pist)
add_test(NAME remotecontrol COMMAND tst_remotecontrol)
set_tests_properties(remotecontrol PROPERTIES TIMEOUT 120)

# MCP shim: the JSON-RPC surface an agent talks to. Driven in-process through
# handleLine/sendLine, with tool calls answered by a scripted fake IDE over
# loopback TCP — the framing table and the watch failure paths are where a
# regression would hang or mis-frame an agent. Needs no display or emulator.
qt_add_executable(tst_mcp
    tests/tst_mcp.cpp
    src/control/mcp/McpServer.cpp
    src/control/mcp/McpServer.h
    src/control/mcp/ControlClient.cpp
    src/control/mcp/ControlClient.h
)
target_include_directories(tst_mcp PRIVATE src)
target_link_libraries(tst_mcp PRIVATE Qt6::Network Qt6::Test)
target_compile_definitions(tst_mcp PRIVATE "PIST_VERSION=\"${PROJECT_VERSION}\"")
add_test(NAME mcp COMMAND tst_mcp)
set_tests_properties(mcp PROPERTIES TIMEOUT 120)

# Integration test: drives a real Hatari. Skips when its prerequisites are absent.
qt_add_executable(tst_emulatorhost
    tests/tst_emulatorhost.cpp
    src/build/LineMap.cpp
    src/build/LineMap.h
    src/build/LinkMap.cpp
    src/build/LinkMap.h
    src/build/ProgramLineMap.cpp
    src/build/ProgramLineMap.h
    src/build/Diagnostic.h
    src/debug/Breakpoint.cpp
    src/debug/Breakpoint.h
    src/debug/Watchpoint.h
    src/emu/EmulatorHost.cpp
    src/emu/EmulatorHost.h
    src/emu/HrdbBackend.cpp
    src/emu/HrdbBackend.h
    src/emu/DebugBackend.h
    src/emu/EmbedSocket.cpp
    src/emu/EmbedSocket.h
    src/emu/HatariTextParse.cpp
    src/emu/HatariTextParse.h
    src/emu/HatariProbe.cpp
    src/emu/HatariProbe.h
    src/emu/Paths.cpp
    src/emu/Paths.h
    src/emu/SessionConfig.cpp
    src/emu/SessionConfig.h
    src/project/ProjectSettings.cpp
    src/project/ProjectSettings.h
    src/toolchain/Toolchain.cpp
    src/toolchain/Toolchain.h
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

# HRDB backend against the tattlemuss/hatari hrdb-main fork. Gated on
# $PIST_HRDB_HATARI naming the fork's binary. CI builds the fork (it is the
# bundled emulator in the Linux AppImage); developer machines skip without it.
qt_add_executable(tst_hrdb
    tests/tst_hrdb.cpp
    src/emu/DebugBackend.h
    src/emu/EmbedSocket.cpp
    src/emu/EmbedSocket.h
    src/emu/HrdbBackend.cpp
    src/emu/HrdbBackend.h
    src/emu/EmulatorHost.cpp
    src/emu/EmulatorHost.h
    src/emu/HatariTextParse.cpp
    src/emu/HatariTextParse.h
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
target_include_directories(tst_hrdb PRIVATE src)
target_link_libraries(tst_hrdb PRIVATE Qt6::Test Qt6::Network)
add_test(NAME hrdb COMMAND tst_hrdb)
set_tests_properties(hrdb PROPERTIES TIMEOUT 300)

# Test binaries must be console applications on Windows. qt_add_executable marks
# them GUI-subsystem, which detaches stdout — the test runs, fails, and reports
# nothing at all, making a red run on Windows undiagnosable.
foreach(test_target IN ITEMS tst_parsers tst_image tst_tosrom tst_debug tst_settings tst_gui tst_emulatorhost tst_link tst_remotecontrol tst_toolfetch tst_hrdb tst_mcp)
    if(TARGET ${test_target})
        set_target_properties(${test_target} PROPERTIES WIN32_EXECUTABLE FALSE)
    endif()
endforeach()
