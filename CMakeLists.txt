cmake_minimum_required(VERSION 3.21)
project(WindowMark VERSION 0.4.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(WINDOWMARK_BUILD_TESTS "Build WindowMark tests" ON)

add_library(windowmark_core STATIC
    src/core/BorderGeometry.cpp
    src/core/ConfigLocation.cpp
    src/core/Coordinator.cpp
    src/core/DrawerState.cpp
    src/core/Hotkey.cpp
    src/core/LayoutEngine.cpp
    src/core/PinRegistry.cpp
    src/core/Settings.cpp
    src/core/Types.cpp
)

target_include_directories(windowmark_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(windowmark_core PUBLIC cxx_std_20)

# /WX：警告即错误。C4129（非法转义）曾把 L"Software\Microsoft..." 悄悄折成
# "SoftwareMicrosoft..."，注册表读不到，强调色一直走兜底分支却没人发现。
# 这类问题编译器本来就报了，只是没人把它当回事，所以直接让它拦下构建。
if(MSVC)
    target_compile_options(windowmark_core PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(windowmark_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(WIN32)
    # A build stamp regenerated on every build. The version string alone cannot answer
    # "is the thing I am running the thing you just built?" - it only changes when someone
    # remembers to change it, and that is exactly the step that gets forgotten. The stamp
    # is written by the build itself, so it cannot be stale or wrong.
    set(WINDOWMARK_STAMP_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    add_custom_target(windowmark_stamp
        COMMAND ${CMAKE_COMMAND}
                -DOUT=${WINDOWMARK_STAMP_DIR}/BuildStamp.h
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteBuildStamp.cmake
        BYPRODUCTS ${WINDOWMARK_STAMP_DIR}/BuildStamp.h
        COMMENT "Writing build stamp")

    # Swap in a different icon by replacing this file - nothing else changes. It has to be
    # a real multi-size .ico (16/20/24/32/48/256 at minimum, 32bpp with alpha): Windows
    # picks the size it wants, and the tray and title bar take 16.
    set(WINDOWMARK_ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/wmiicon.ico")
    if(NOT EXISTS "${WINDOWMARK_ICON}")
        message(FATAL_ERROR "找不到图标 ${WINDOWMARK_ICON}")
    endif()
    # The version has exactly one source of truth: kProductVersion in AppIdentity.h, which
    # is the string the About box and the tray menu show. Read it here rather than repeat
    # it, and fail loudly if project(VERSION) has drifted from it - a release binary whose
    # properties page disagrees with its own About box is worse than one with no version.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/shared/AppIdentity.h" WINDOWMARK_APPIDENTITY)
    string(REGEX MATCH "kProductVersion\\[\\][ \t]*=[ \t]*L\"([^\"]+)\""
           WINDOWMARK_VERSION_MATCH "${WINDOWMARK_APPIDENTITY}")
    set(WINDOWMARK_VERSION_STRING "${CMAKE_MATCH_1}")
    if(NOT WINDOWMARK_VERSION_STRING)
        message(FATAL_ERROR "读不出 src/shared/AppIdentity.h 里的 kProductVersion")
    endif()
    if(NOT WINDOWMARK_VERSION_STRING MATCHES "^${PROJECT_VERSION}")
        message(FATAL_ERROR
            "版本不一致: project() 写的是 ${PROJECT_VERSION}，"
            "AppIdentity.h 写的是 ${WINDOWMARK_VERSION_STRING}")
    endif()
    # VERSIONINFO needs four integers; a suffix like -rc1 has no place in them.
    string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)"
           WINDOWMARK_VERSION_PARTS "${WINDOWMARK_VERSION_STRING}")
    set(WINDOWMARK_VERSION_COMMA "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},0")

    configure_file(res/WindowMark.rc.in "${CMAKE_CURRENT_BINARY_DIR}/WindowMark.rc" @ONLY)
    set(WINDOWMARK_RC "${CMAKE_CURRENT_BINARY_DIR}/WindowMark.rc")

    add_executable(WindowMark WIN32
        src/platform/windows/WinMain.cpp
        src/platform/windows/WinBorderBackend.cpp
        src/platform/windows/WinWindowBackend.cpp
        src/platform/windows/WinOverlayBackend.cpp
        src/platform/windows/WinPinBackend.cpp
        src/platform/windows/WinPreviewBackend.cpp
        src/platform/windows/WinControlWindow.cpp
        src/platform/windows/WinConfigPathDialog.cpp
        src/platform/windows/WinRenameDialog.cpp
        src/platform/windows/WinSelectionDialog.cpp
        src/platform/windows/WinSettingsDialog.cpp
        src/platform/windows/WinUtil.cpp
        ${WINDOWMARK_RC}
    )

    add_dependencies(WindowMark windowmark_stamp)

    target_include_directories(WindowMark PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows
        ${WINDOWMARK_STAMP_DIR}
    )

    target_compile_definitions(WindowMark PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )

    if(MSVC)
        target_compile_options(WindowMark PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    endif()

    target_link_libraries(WindowMark PRIVATE
        windowmark_core
        user32
        gdi32
        shell32
        comctl32
        comdlg32
        ole32
        advapi32
        dwmapi
        d2d1
        dwrite
        shcore
    )

    target_include_directories(WindowMark PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/shared
    )

    # Diagnostic tool. Console subsystem so its report is just stdout, and like the
    # installers it does not link windowmark_core - it inspects the running app from the
    # outside, through the window list, rather than sharing any of its code.
    add_executable(WindowMarkInspect src/tools/InspectMain.cpp ${WINDOWMARK_RC})
    target_include_directories(WindowMarkInspect PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/shared)
    # NOMINMAX matters here: windows.h defines max as a macro and this file uses std::max.
    target_compile_definitions(WindowMarkInspect PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(WindowMarkInspect PRIVATE user32 gdi32 dwmapi)
    if(MSVC)
        target_compile_options(WindowMarkInspect PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    endif()

    # Installer and uninstaller are standalone GUI executables. They deliberately do
    # not link windowmark_core: they only need process control, registry and shell APIs.
    foreach(tool_target IN ITEMS WindowMarkSetup WindowMarkUninstall)
        if(tool_target STREQUAL "WindowMarkSetup")
            set(tool_main src/installer/SetupMain.cpp)
        else()
            set(tool_main src/installer/UninstallMain.cpp)
        endif()

        # The rc goes in these too: WindowMarkSetup.exe is the file a user actually sees
        # in Explorer, and without it the installer has neither an icon nor a version.
        add_executable(${tool_target} WIN32
            ${tool_main}
            src/installer/InstallerCommon.cpp
            ${WINDOWMARK_RC}
        )

        target_include_directories(${tool_target} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/installer
            ${CMAKE_CURRENT_SOURCE_DIR}/src/shared
        )

        target_compile_definitions(${tool_target} PRIVATE
            UNICODE
            _UNICODE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
        )

        if(MSVC)
            target_compile_options(${tool_target} PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
        endif()

        target_link_libraries(${tool_target} PRIVATE
            user32
            shell32
            comctl32
            ole32
            oleaut32
            advapi32
            shlwapi
        )
    endforeach()

    # The setup executable copies the uninstaller, so it must exist first.
    add_dependencies(WindowMarkSetup WindowMarkUninstall WindowMark)
elseif(APPLE)
    enable_language(OBJCXX)
    add_executable(WindowMark
        src/platform/macos/MacMain.mm
        src/platform/macos/MacBackendStubs.mm
    )
    target_include_directories(WindowMark PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/macos
    )
    find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
    target_link_libraries(WindowMark PRIVATE windowmark_core ${COCOA_FRAMEWORK})
else()
    add_executable(WindowMark src/platform/stub/StubMain.cpp)
    target_link_libraries(WindowMark PRIVATE windowmark_core)
endif()

if(WINDOWMARK_BUILD_TESTS)
    enable_testing()
    add_executable(windowmark_core_tests tests/core_tests.cpp)
    target_link_libraries(windowmark_core_tests PRIVATE windowmark_core)
    add_test(NAME windowmark_core_tests COMMAND windowmark_core_tests)

    if(WIN32)
        add_executable(windowmark_autostart_tests tests/autostart_tests.cpp)
        target_include_directories(windowmark_autostart_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/shared)
        target_compile_definitions(windowmark_autostart_tests PRIVATE
            UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_link_libraries(windowmark_autostart_tests PRIVATE advapi32)
        if(MSVC)
            target_compile_options(windowmark_autostart_tests PRIVATE
                /W4 /WX /permissive- /EHsc /utf-8)
        endif()
        add_test(NAME windowmark_autostart_tests COMMAND windowmark_autostart_tests)
    endif()
endif()
