cmake_minimum_required(VERSION 3.21)

project(pist
    VERSION 0.8.7
    DESCRIPTION "PiST - an IDE for Atari ST assembly development"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The warning baseline (MIN-48): every compiler's everyday set, on the code from
# its first build rather than bolted on later. Measured clean across all targets
# before enabling (the one nodiscard hit in tst_gui is fixed), and CI inherits
# these from the configure, so the baseline lives in exactly one place.
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

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

# ---------------------------------------------------------------------------
# Module libraries
# ---------------------------------------------------------------------------
# One library per module of the map in docs/ARCHITECTURE.md, so the layering is
# a build-graph fact rather than a convention. The map's build rule — image,
# build, debug, emu, project, toolchain and git are widget-free — is enforced by
# the linker now: none of those links Qt6::Widgets, so a stray <QWidget> include
# or symbol fails the build instead of merely offending a reviewer.
#
# STATIC, not OBJECT: a consumer pulls in only the objects it needs, and the
# test targets link the module libraries instead of re-listing their sources.
# Under the old single pist_core OBJECT library the whole of MainWindow.cpp was
# linked into tst_git, and src/emu/Paths.cpp was compiled eight times.
#
# Every edge in the graph below is an include in the sources.
# ---------------------------------------------------------------------------
function(pist_module name)
    qt_add_library(${name} STATIC ${ARGN})
    # Consumers get the source root and PIST_VERSION exactly as they did from
    # the old single pist_core.
    target_include_directories(${name} PUBLIC src)
    # Quoted so the preprocessor sees a string, not a floating-point token.
    target_compile_definitions(${name} PUBLIC "PIST_VERSION=\"${PROJECT_VERSION}\"")
endfunction()

# support/ — the leaf utilities the whole graph may use. One rule lives here so
# far: how a file is written so that a failed write cannot destroy what the user
# already had. Qt6::Core, no module edges of its own, so it sits under the graph
# the way model/ does.
pist_module(pist_support
    src/support/FileWrite.cpp
    src/support/FileWrite.h
)

# model/ — the vocabulary two modules share and neither owns: the machines PiST
# offers, their Hatari names, and which TOS versions they accept. Leaf of the
# graph: Qt6::Core, no module edges of its own, so `project/` (which persists a
# machine name) does not have to reach into `emu/` for it (MIN-56).
pist_module(pist_model
    src/model/Machine.cpp
    src/model/Machine.h
)

# image/ — sprite document, ST graphics, paint tools, and the PI1/NEO/MBF/PNG codecs.
pist_module(pist_image
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

# build/ — vasm/vlink orchestration, the line maps, the symbol table, the FAT12 writer.
pist_module(pist_build
    src/build/BuildService.cpp
    src/build/BuildService.h
    src/build/Diagnostic.h
    src/build/SymbolTable.cpp
    src/build/SymbolTable.h
    src/build/FloppyImage.cpp
    src/build/FloppyImage.h
    src/build/FloppyTransfer.cpp
    src/build/FloppyTransfer.h
    src/build/LineMap.cpp
    src/build/LineMap.h
    src/build/LinkMap.cpp
    src/build/LinkMap.h
    src/build/ProgramLineMap.cpp
    src/build/ProgramLineMap.h
)

# debug/ — the file:line breakpoint model and the watchpoints.
pist_module(pist_debug
    src/debug/Breakpoint.cpp
    src/debug/Breakpoint.h
    src/debug/Watchpoint.h
)

# emu/ — the debug backends, the session argv, ROM/model/memory/paths helpers.
pist_module(pist_emu
    src/emu/ProfileData.cpp
    src/emu/ProfileData.h
    src/emu/EmulatorHost.cpp
    src/emu/EmulatorHost.h
    src/emu/DebugBackend.h
    src/emu/EmbedSocket.cpp
    src/emu/EmbedSocket.h
    src/emu/HrdbBackend.cpp
    src/emu/HrdbBackend.h
    src/emu/HostAudio.cpp
    src/emu/HostAudio.h
    src/emu/HostKey.h
    src/emu/LibretroAbi.h
    src/emu/LibretroBackend.cpp
    src/emu/LibretroBackend.h
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
    src/emu/TosRom.cpp
    src/emu/TosRom.h
)

# project/ — the per-project .pistproject file.
pist_module(pist_project
    src/project/ProjectSettings.cpp
    src/project/ProjectSettings.h
)

# toolchain/ — tool discovery and the checksum-pinned fetch.
pist_module(pist_toolchain
    src/toolchain/Toolchain.cpp
    src/toolchain/Toolchain.h
    src/toolchain/ToolFetch.cpp
    src/toolchain/ToolFetch.h
)

# git/ — the git subprocess driver and its porcelain parser.
pist_module(pist_git
    src/git/GitParse.cpp
    src/git/GitTypes.h
    src/git/GitService.cpp
    src/git/GitService.h
)

# editor/ — the text editor, the highlighter, and the reference tables.
pist_module(pist_editor
    src/editor/AsmHighlighter.cpp
    src/editor/AsmHighlighter.h
    src/editor/CodeEditor.cpp
    src/editor/CodeEditor.h
    src/editor/EditorTheme.h
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
    src/editor/AsmLex.h
)

# control/ — the remote-control TCP protocol (RemoteControl drives the IDE through ControlHost).
pist_module(pist_control
    src/control/RemoteControl.cpp
    src/control/RemoteControl.h
)

# control/mcp — the pist-mcp stdio shim. Its own library: the shim talks MCP on
# stdio and the remote-control socket over TCP, so it must stay a QtCore/QtNetwork
# console binary — no Widgets, no IDE — while pist-mcp and tst_mcp share one
# compilation of it.
pist_module(pist_mcp
    src/control/mcp/ControlClient.cpp
    src/control/mcp/ControlClient.h
    src/control/mcp/McpServer.cpp
    src/control/mcp/McpServer.h
)

# ui/ — MainWindow, every dock panel, the sprite editor, appearance and icons.
pist_module(pist_ui
    src/ui/Appearance.cpp
    src/ui/Appearance.h
    src/ui/Icons.cpp
    src/ui/UiText.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/DebugSessionController.cpp
    src/ui/DebugSessionController.h
    src/ui/SessionLauncher.cpp
    src/ui/SessionLauncher.h
    src/ui/BreakpointWatchpointModel.cpp
    src/ui/BreakpointWatchpointModel.h
    src/ui/ProfilerController.cpp
    src/ui/ProfilerController.h
    src/ui/RemoteStateAdapter.cpp
    src/ui/RemoteStateAdapter.h
    src/ui/RegistersView.cpp
    src/ui/RegistersView.h
    src/ui/BreakpointPanel.cpp
    src/ui/BreakpointPanel.h
    src/ui/BitplaneExportDialog.cpp
    src/ui/BitplaneExportDialog.h
    src/ui/BitplaneExportController.cpp
    src/ui/BitplaneExportController.h
    src/ui/DisassemblyView.cpp
    src/ui/DisassemblyView.h
    src/ui/EmulatorDisplayWidget.cpp
    src/ui/EmulatorDisplayWidget.h
    src/ui/EmbedX11.cpp
    src/ui/EmbedX11.h
    src/ui/EmbedWin32.cpp
    src/ui/EmbedWin32.h
    src/ui/FileBrowser.cpp
    src/ui/FileBrowser.h
    src/ui/GitPanel.cpp
    src/ui/GitPanel.h
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
    src/ui/SheetSlicing.cpp
    src/ui/SheetSlicing.h
    src/ui/AnimationPreviewWidget.cpp
    src/ui/AnimationPreviewWidget.h
    src/ui/ImageEditor.cpp
    src/ui/ImageEditor.h
    src/ui/PalettePickerDialog.cpp
    src/ui/PalettePickerDialog.h
    src/ui/NewImageDialog.cpp
    src/ui/NewImageDialog.h
    src/ui/AboutDialog.cpp
    src/ui/AboutDialog.h
)

# ---------------------------------------------------------------------------
# The module graph — one edge per include in the sources
# ---------------------------------------------------------------------------
# image, build, debug, emu, model, project, toolchain and git stay widget-free:
# they link Qt6::Core / Qt6::Gui / Qt6::Network and never Qt6::Widgets.
target_link_libraries(pist_model PUBLIC Qt6::Core)                 # the machine vocabulary: QtCore only
target_link_libraries(pist_support PUBLIC Qt6::Core)               # the write-once-safely rule: QtCore only
target_link_libraries(pist_image PUBLIC Qt6::Gui pist_support)     # QImage/QColor in the codecs and the document; .pim and ST exports save through files::write
target_link_libraries(pist_build PUBLIC Qt6::Core pist_support)    # subprocess + parsed text only; saveRaw writes the user's floppy image
target_link_libraries(pist_debug PUBLIC Qt6::Core pist_build)      # Breakpoint.h includes build/LineMap.h + ProgramLineMap.h
target_link_libraries(pist_emu PUBLIC Qt6::Core Qt6::Network pist_model)   # emu/TosRom.h includes model/Machine.h
if(APPLE)
    # The in-process core's sound. Linux discards the same samples; it does
    # not link a player. AudioToolbox is a system framework, not a Qt module.
    target_link_libraries(pist_emu PUBLIC "-framework AudioToolbox")
endif()
target_link_libraries(pist_project PUBLIC Qt6::Core pist_model pist_support)    # ProjectSettings.h includes model/Machine.h, and save() is files::write
target_link_libraries(pist_toolchain PUBLIC Qt6::Core pist_emu)    # ToolFetch.cpp includes emu/Paths.h
target_link_libraries(pist_git PUBLIC Qt6::Core)                   # git subprocess + porcelain only

# The graph is acyclic, and every edge is an include in the sources. ui/ is the
# only module that reaches both ways by nature — it builds the editors and owns
# the appearance — so the edges it would form with editor/ and control/ point
# one way only: ui/MainWindow.cpp includes editor/CodeEditor.h and
# control/RemoteControl.h, while editor/CodeEditor.cpp takes its colours as an
# EditorTheme value and src/control/ talks to the IDE through ControlHost.
# Neither of those edges runs back to ui/, so CMake never has to repeat a whole
# archive list to break a cycle (MIN-86); the only repeats left in a consumer's
# link line are the ordinary direct-and-transitive ordering ones.
target_link_libraries(pist_editor PUBLIC Qt6::Widgets pist_git pist_support)
target_link_libraries(pist_ui PUBLIC Qt6::Widgets Qt6::Network
    pist_image
    pist_build
    pist_debug
    pist_emu
    pist_project
    pist_toolchain
    pist_git
    pist_editor
    pist_control
    pist_support
)
target_link_libraries(pist_control PUBLIC Qt6::Network pist_emu)   # its verbs reuse emu/'s typed reads (MemoryDump, MachineState)
target_link_libraries(pist_mcp PUBLIC Qt6::Core Qt6::Network)      # MCP stdio <-> remote-control socket, no Widgets

if(X11_FOUND)
    # Only the UI touches X11 (src/ui/EmbedX11.cpp). The define and the
    # libraries ride on pist_ui, so the app and the GUI tests keep the compile
    # line they had when this was one library.
    target_compile_definitions(pist_ui PUBLIC PIST_HAVE_X11)
    target_link_libraries(pist_ui PUBLIC X11::X11)
    # X Shape (libXext) makes the embedded video input-transparent mid-drag.
    if(TARGET X11::Xext)
        target_compile_definitions(pist_ui PUBLIC PIST_HAVE_XEXT)
        target_link_libraries(pist_ui PUBLIC X11::Xext)
    endif()
endif()

if(WIN32)
    # The Windows half of the embedded display (src/ui/EmbedWin32.cpp) moves the
    # emulator's own window into the dock: EnumWindows, SetParent, MoveWindow
    # and the window styles are all user32. Qt links it transitively, but the
    # direct use is declared here so it does not depend on that.
    target_link_libraries(pist_ui PUBLIC user32)
endif()

qt_add_executable(pist
    src/main.cpp
)
# main.cpp itself uses the control listener, the emulator probe/paths/ROM
# helpers and the toolchain lookup on top of the UI.
target_link_libraries(pist PRIVATE pist_ui pist_control pist_emu pist_toolchain)

# The MCP stdio shim: lets Claude/Cursor-class agents discover a running PiST
# as a native tool, bridging MCP JSON-RPC (stdio) to the RemoteControl TCP
# protocol. Console application everywhere — WIN32_EXECUTABLE would detach
# stdout and silently break the transport.
qt_add_executable(pist-mcp
    src/control/mcp/main.cpp
)
target_link_libraries(pist-mcp PRIVATE pist_mcp)
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
    #
    # BUNDLE_NAME is CFBundleName: the short name in the menu bar. Finder's
    # label under the icon is the bundle directory's name, not this key — the
    # build tree stays pist.app (the target name) and the release step renames
    # the shipped bundle to PiST.app, which is what the disk image shows.
    #
    # GUI_IDENTIFIER is CFBundleIdentifier, and the project never set it: the
    # template writes whatever the property holds, so pist.app shipped with an
    # empty bundle identifier. It is the same reverse-DNS name the AppStream
    # component uses.
    set_target_properties(pist PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "PiST"
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.idontwantyourspamthanks.pist"
        MACOSX_BUNDLE_COPYRIGHT "Copyright (c) Koala Software. GPL-2.0-or-later, see LICENSE.")
endif()

install(TARGETS pist
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS pist-mcp
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Licence and third-party notices. The LGPL (Qt) and BSD obligations require
# these to travel with a binary distribution, and the NOTICE is also how a user
# learns which external programs they still need to supply. `copyright` is the
# DEP-5 statement Debian policy wants at /usr/share/doc/<pkg>/copyright; without
# it the deb states its licence nowhere a package manager looks.
install(FILES LICENSE NOTICE README.md packaging/copyright
    DESTINATION ${CMAKE_INSTALL_DOCDIR})

if(UNIX AND NOT APPLE)
    # Desktop entry and icon, so the IDE appears in application menus rather than
    # only being launchable from a terminal.
    install(FILES pist.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
    install(FILES packaging/pist.svg
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
        RENAME pist.svg)
    # Raster sizes beside the SVG, all named pist.png for Icon=pist. Without
    # these the hicolor tree a package carries is either SVG-only or — in the
    # AppImage, whose deployer creates the size directories itself — empty
    # directories at 16, 32, 64 and 128.
    foreach(pist_icon_size 128 256 512)
        install(FILES packaging/icons/pist-${pist_icon_size}.png
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${pist_icon_size}x${pist_icon_size}/apps
            RENAME pist.png)
    endforeach()
    # AppStream metadata: the name, developer and licence a software centre
    # reports for the installed package. Without it GNOME Software and Discover
    # fall back to the deb's file name and "Unknown Author". The file name is the
    # component id, and <launchable> inside it is what joins the component to
    # pist.desktop.
    install(FILES packaging/io.github.idontwantyourspamthanks.pist.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo)
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
# Packaging (CPack): the Linux deb/rpm and the Windows MSI are built from the
# same staged trees the release archives ship, so the installer and the
# archive carry identical content — binary, bundled Qt runtime, assembler,
# emulator, ROM, licences and notices. The generator is chosen at package
# time (DEB/RPM on Linux, WIX on Windows). The macOS disk image is not a
# CPack product; the release workflow writes it with hdiutil. 
# CPACK_INSTALLED_DIRECTORIES overrides the tree and must be an absolute path
# (CMake 4 RelativePath).
# ---------------------------------------------------------------------------
# The product is PiST and the author is Koala Software. These are the names an
# installer shows: CPACK_PACKAGE_NAME is the MSI product name and the NSIS/STGZ
# install directory, CPACK_PACKAGE_VENDOR the MSI Manufacturer (WIX.template.in)
# and the RPM Vendor. The distro package names are lowercased separately below,
# because Debian policy requires it and CPackDeb lowercases whatever it is given
# anyway (CPackRPM does not).
set(CPACK_PACKAGE_NAME "PiST")
set(CPACK_PACKAGE_VENDOR "Koala Software")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "An IDE for Atari ST assembly development")
set(CPACK_PACKAGE_CONTACT "https://github.com/idontwantyourspamthanks/PiST")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/idontwantyourspamthanks/PiST")
# The licence the installers quote. Left at CMake's default this is
# CPack.GenericLicense.txt — a placeholder that tells an MSI or self-extracting
# installer user nothing at all about GPL-2.0-or-later.
#
# It must be a path whose extension CPack understands. CPackWIX turns the file
# into RTF by extension and accepts .txt or .rtf; the repository's LICENSE has
# no extension at all, which failed the Windows leg with "unsupported WiX
# License file extension ''". The copy is generated into the build tree, so
# there is still exactly one licence text to maintain.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
    ${CMAKE_CURRENT_BINARY_DIR}/license.txt COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_BINARY_DIR}/license.txt")
# Pointing that resource at anything other than CMake's placeholder makes CPack
# enable a dmg Software License Agreement by default: CPack.cmake sets
# CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE unless CMP0133 is NEW. A dmg with an
# SLA cannot be attached without somebody clicking Agree, so `hdiutil attach`
# fails with "attach canceled" on a headless runner. Off is what a hand-run
# `cpack -G DragNDrop` needs. The published image is built by the release
# workflow with hdiutil, not by this generator; the licence still travels
# inside the app (LICENSE, NOTICE and the generated THIRD-PARTY.txt).
set(CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE OFF)

# One description for both binary package formats, so the deb and the rpm cannot
# drift apart. It must be a single multi-line string: several quoted arguments
# make a CMake *list*, and the semicolons that join a list reached the control
# file as literal "; " at the head of every continuation line. CPackDeb strips
# and re-indents each line itself, so no leading spaces here.
set(PIST_PACKAGE_DESCRIPTION "IDE for Atari ST 68000 assembly development: editor, vasm integration,
and debugging under the bundled Hatari emulator (breakpoints, stepping,
registers, memory, disassembly, watchpoints).
Self-contained: Qt runtime, Hatari (hrdb-main fork), vasm and EmuTOS
are included. Licence: GPL-2.0-or-later; the bundled assembler and linker are
not free software and are redistributed unmodified for non-commercial use
(see /usr/share/doc/pist/copyright).")

# Debian metadata. No computed shared-library dependencies: the package is
# self-contained by design (bundled Qt, emulator, assembler, ROM), and a
# distro Qt may be older than the 6.5 floor — a libqt6 dependency would be
# wrong here.
set(CPACK_DEBIAN_PACKAGE_NAME "pist")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Koala Software")
set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "${PIST_PACKAGE_DESCRIPTION}")

# RPM metadata. No auto-computed requires: same self-contained rationale.
set(CPACK_RPM_PACKAGE_NAME "pist")
set(CPACK_RPM_PACKAGE_LICENSE "GPL-2.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Development/Tools")
set(CPACK_RPM_PACKAGE_AUTOREQPROV OFF)
set(CPACK_RPM_PACKAGE_DESCRIPTION "${PIST_PACKAGE_DESCRIPTION}")

# A stable upgrade GUID is what lets one PiST MSI replace the previous one
# rather than installing side by side. Do not change it between releases.
set(CPACK_WIX_UPGRADE_GUID "7d7f9c1e-8a2b-4f3a-9e5d-1c6b2a0d4e8f")

# Start Menu shortcuts. CPackWIX creates them only for the raw names listed in
# CPACK_PACKAGE_EXECUTABLES, matched against the installed tree; with the
# variable unset the MSI installed every file and nothing that launches them,
# which is what a clean machine reported: no Start Menu entry at all. The
# folder defaults to CPACK_PACKAGE_NAME, so the entry lands under "PiST".
set(CPACK_PACKAGE_EXECUTABLES "pist" "PiST")

# The published disk image is not built by this generator. The release
# workflow writes it with hdiutil from a folder that holds only PiST.app and
# a link to /Applications; CPack DragNDrop would pack the whole staged tree
# (the app, share/ and BUILD-INFO.txt) and that is the volume that shipped.
# These settings still apply if someone runs `cpack -G DragNDrop` by hand.
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

# Each test links the module libraries it exercises instead of re-listing
# their sources: the modules own those compilations now (src/emu/Paths.cpp
# used to appear in seven of the fifteen target lists).

qt_add_executable(tst_parsers
    tests/tst_parsers.cpp
)
target_link_libraries(tst_parsers PRIVATE pist_build pist_debug Qt6::Test)
target_compile_definitions(tst_parsers PRIVATE PIST_SOURCE_DIR="${PIST_SOURCE_DIR_CMAKE}")
add_test(NAME parsers COMMAND tst_parsers)

# Sprite document, ST palettes, paint tools and PI1/NEO/MBK/PNG codecs.
# Always runs; no emulator or display required.
qt_add_executable(tst_image
    tests/tst_image.cpp
)
target_link_libraries(tst_image PRIVATE pist_image Qt6::Test)
add_test(NAME image COMMAND tst_image)

# The OS-call reference table and the trap-sequence scanner: pure QtCore
# code, so the suite needs no display and no emulator.
qt_add_executable(tst_oscall
    tests/tst_oscall.cpp
)
target_link_libraries(tst_oscall PRIVATE pist_editor Qt6::Test)
add_test(NAME oscall COMMAND tst_oscall)

# TOS ROM identification: version must come from the image header, so the
# autostart guard cannot be skipped by an unusual filename.
qt_add_executable(tst_tosrom
    tests/tst_tosrom.cpp
)
target_link_libraries(tst_tosrom PRIVATE pist_project pist_toolchain pist_emu Qt6::Test)
add_test(NAME tosrom COMMAND tst_tosrom)

# The profile save parser: header fields and the memory-area lines that feed
# the profiler's TOS/ROM row. Pure QtCore; no display or emulator required.
qt_add_executable(tst_profile
    tests/tst_profile.cpp
)
target_link_libraries(tst_profile PRIVATE pist_emu pist_build Qt6::Test)
add_test(NAME profile COMMAND tst_profile)

# Breakpoint resolution and memory-dump parsing: pure functions, no emulator.
qt_add_executable(tst_debug
    tests/tst_debug.cpp
)
target_link_libraries(tst_debug PRIVATE pist_build pist_debug pist_project pist_emu Qt6::Test)
add_test(NAME debug COMMAND tst_debug)

# Multi-module source mapping through a linker's map file. The end-to-end case
# runs a real vasm+vlink when both are available and skips otherwise, so CI
# without vlink still exercises the parsing and the address arithmetic.
qt_add_executable(tst_link
    tests/tst_link.cpp
)
target_link_libraries(tst_link PRIVATE pist_build Qt6::Test)
add_test(NAME link COMMAND tst_link)

# Project settings persistence.
qt_add_executable(tst_settings
    tests/tst_settings.cpp
)
target_link_libraries(tst_settings PRIVATE pist_project pist_toolchain pist_emu Qt6::Test)
add_test(NAME settings COMMAND tst_settings)

# First-run fetch/install: checksum verification, archive extraction, and the
# claim that an installed tool lands where discovery actually looks. Needs no
# network — fixtures are fabricated locally; the vasm build case skips without
# make/tar.
qt_add_executable(tst_toolfetch
    tests/tst_toolfetch.cpp
)
target_link_libraries(tst_toolfetch PRIVATE pist_toolchain pist_emu Qt6::Test)
# The pin guard reads the workflows' copies of the fetch pins.
target_compile_definitions(tst_toolfetch PRIVATE PIST_SOURCE_DIR="${PIST_SOURCE_DIR_CMAKE}")
add_test(NAME toolfetch COMMAND tst_toolfetch)

# Git status, blame, commit, branches, diff and history. The panel drives a
# temporary repository; nothing here talks to a remote. Skips the live cases
# when git is not on PATH.
qt_add_executable(tst_git
    tests/tst_git.cpp
)
target_link_libraries(tst_git PRIVATE pist_git pist_editor pist_ui Qt6::Test)
add_test(NAME git COMMAND tst_git)

# The in-process core has no window. The panel letterboxes the frame it is
# handed, and a cleared session does not keep that picture.
qt_add_executable(tst_display
    tests/tst_display.cpp
)
target_link_libraries(tst_display PRIVATE pist_ui Qt6::Test)
add_test(NAME display COMMAND tst_display)

# GUI-level: constructs MainWindow and verifies the line map against a real
# vasm listing, covering the PC <-> source-line claim end to end.
qt_add_executable(tst_gui
    tests/tst_gui.cpp
)
target_link_libraries(tst_gui PRIVATE pist_ui Qt6::Test)

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
target_link_libraries(tst_remotecontrol PRIVATE pist_control pist_ui Qt6::Test)
add_dependencies(tst_remotecontrol pist)
add_test(NAME remotecontrol COMMAND tst_remotecontrol)
set_tests_properties(remotecontrol PROPERTIES TIMEOUT 120)

# MCP shim: the JSON-RPC surface an agent talks to. Driven in-process through
# handleLine/sendLine, with tool calls answered by a scripted fake IDE over
# loopback TCP — the framing table and the watch failure paths are where a
# regression would hang or mis-frame an agent. Needs no display or emulator.
qt_add_executable(tst_mcp
    tests/tst_mcp.cpp
)
target_link_libraries(tst_mcp PRIVATE pist_mcp Qt6::Test)
add_test(NAME mcp COMMAND tst_mcp)
set_tests_properties(mcp PROPERTIES TIMEOUT 120)

# Integration test: drives a real Hatari. Skips when its prerequisites are absent.
qt_add_executable(tst_emulatorhost
    tests/tst_emulatorhost.cpp
)
target_link_libraries(tst_emulatorhost PRIVATE pist_emu pist_build pist_debug pist_project pist_toolchain Qt6::Test)
add_test(NAME emulatorhost COMMAND tst_emulatorhost)
set_tests_properties(emulatorhost PROPERTIES TIMEOUT 300)

# HRDB backend against the tattlemuss/hatari hrdb-main fork. Gated on
# $PIST_HRDB_HATARI naming the fork's binary. CI builds the fork (it is the
# bundled emulator in the Linux AppImage); developer machines skip without it.
qt_add_executable(tst_hrdb
    tests/tst_hrdb.cpp
)
target_link_libraries(tst_hrdb PRIVATE pist_emu pist_project Qt6::Test)
add_test(NAME hrdb COMMAND tst_hrdb)
set_tests_properties(hrdb PROPERTIES TIMEOUT 300)

# Test binaries must be console applications on Windows. qt_add_executable marks
# them GUI-subsystem, which detaches stdout — the test runs, fails, and reports
# nothing at all, making a red run on Windows undiagnosable.
# Test binaries must be console applications on Windows. The list must name
# every test target - it had drifted once (tst_oscall and tst_profile missing),
# which on Windows left those two GUI-subsystem with a detached stdout: the
# test runs, fails, and reports nothing at all (MIN-62).
foreach(test_target IN ITEMS tst_parsers tst_image tst_oscall tst_tosrom tst_profile tst_debug tst_link tst_settings tst_toolfetch tst_git tst_gui tst_remotecontrol tst_mcp tst_emulatorhost tst_hrdb)
    if(TARGET ${test_target})
        set_target_properties(${test_target} PROPERTIES WIN32_EXECUTABLE FALSE)
    endif()
endforeach()
