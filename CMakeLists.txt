cmake_minimum_required(VERSION 3.21)
project(WindowMark VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(WINDOWMARK_BUILD_TESTS "Build WindowMark core tests" ON)

add_library(windowmark_core STATIC
    src/core/Coordinator.cpp
    src/core/DrawerState.cpp
    src/core/LayoutEngine.cpp
    src/core/Settings.cpp
)

target_include_directories(windowmark_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(windowmark_core PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(windowmark_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(windowmark_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    add_executable(WindowMark WIN32
        src/platform/windows/WinMain.cpp
        src/platform/windows/WinWindowBackend.cpp
        src/platform/windows/WinOverlayBackend.cpp
        src/platform/windows/WinPreviewBackend.cpp
        src/platform/windows/WinControlWindow.cpp
        src/platform/windows/WinRenameDialog.cpp
        src/platform/windows/WinSelectionDialog.cpp
        src/platform/windows/WinSettingsDialog.cpp
        src/platform/windows/WinUtil.cpp
    )

    target_include_directories(WindowMark PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows
    )

    target_compile_definitions(WindowMark PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )

    if(MSVC)
        target_compile_options(WindowMark PRIVATE /W4 /permissive- /EHsc /utf-8)
    endif()

    target_link_libraries(WindowMark PRIVATE
        windowmark_core
        user32
        gdi32
        shell32
        comctl32
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

    # Installer and uninstaller are standalone GUI executables. They deliberately do
    # not link windowmark_core: they only need process control, registry and shell APIs.
    foreach(tool_target IN ITEMS WindowMarkSetup WindowMarkUninstall)
        if(tool_target STREQUAL "WindowMarkSetup")
            set(tool_main src/installer/SetupMain.cpp)
        else()
            set(tool_main src/installer/UninstallMain.cpp)
        endif()

        add_executable(${tool_target} WIN32
            ${tool_main}
            src/installer/InstallerCommon.cpp
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
            target_compile_options(${tool_target} PRIVATE /W4 /permissive- /EHsc /utf-8)
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
endif()
