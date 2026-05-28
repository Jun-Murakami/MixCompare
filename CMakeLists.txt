# MixCompare — Mix / リファレンス比較プラグイン
# iPlug2 + WebView (Vite + React + MUI) 構成
#
# JUCE からの移植版。旧 JUCE 構成は CMakeLists.juce.txt / plugin/CMakeLists.juce.txt
# として参照用に残置している。雛形は Synth80 (../Synth80) を踏襲。

cmake_minimum_required(VERSION 3.22)

# macOS の最小対応バージョンを全 SDK/サブモジュールで揃える。
if(APPLE)
    set(MIXCOMPARE_MIN_MACOS "10.15")
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "${MIXCOMPARE_MIN_MACOS}" CACHE STRING "Minimum supported macOS version" FORCE)
    elseif(CMAKE_OSX_DEPLOYMENT_TARGET VERSION_GREATER MIXCOMPARE_MIN_MACOS)
        message(STATUS "Overriding CMAKE_OSX_DEPLOYMENT_TARGET (${CMAKE_OSX_DEPLOYMENT_TARGET}) -> ${MIXCOMPARE_MIN_MACOS}")
        set(CMAKE_OSX_DEPLOYMENT_TARGET "${MIXCOMPARE_MIN_MACOS}" CACHE STRING "Minimum supported macOS version" FORCE)
    endif()
    message(STATUS "macOS deployment target: ${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()

# バージョン文字列の単一情報源 (VERSION ファイル)。
include(${CMAKE_SOURCE_DIR}/cmake/Version.cmake)

project(MixCompare VERSION ${PROJECT_VERSION} LANGUAGES C CXX)

# C++17 ベースライン。局所的に C++20 機能をターゲット単位で opt-in 可。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC は既定でソースを Active Code Page で読むため、日本語 (UTF-8) ソース中の
# 2 バイト文字が SJIS 0x5C(\) と衝突して行継続→構文エラーになる。/utf-8 を強制。
if(MSVC)
    add_compile_options(/utf-8)
endif()

# --------------------------------------------------------------------------
# iPlug2 integration
# --------------------------------------------------------------------------
# iPlug2 は git submodule (./iPlug2)。Synth80 と同 commit (400bed76b) に pin。
# 取得: git submodule update --init --recursive
# SDK : iPlug2/Dependencies/IPlug/download-iplug-sdks.sh (VST3/CLAP 等)
set(IPLUG2_DIR "${CMAKE_SOURCE_DIR}/iPlug2" CACHE PATH "Path to the iPlug2 framework checkout")

if(NOT EXISTS "${IPLUG2_DIR}/iPlug")
    message(FATAL_ERROR
        "iPlug2 not found at ${IPLUG2_DIR}.\n"
        "  Run `git submodule update --init --recursive` then the SDK download step."
    )
endif()

include(${IPLUG2_DIR}/iPlug2.cmake)

# --------------------------------------------------------------------------
# Plugin deploy paths (Windows)
# --------------------------------------------------------------------------
# Synth80 は system-wide (C:\Program Files\Common Files\{VST3,CLAP}) に固定して
# いるが、そちらは書き込みに管理者権限が要る。MixCompare の移植開発フェーズでは
# iPlug2 既定の per-user パス (%LOCALAPPDATA%\Programs\Common\{VST3,CLAP}) を
# そのまま使い、管理者ターミナル不要で Debug ビルドできるようにする。
# 配布時に system-wide へ切り替えたい場合は -DIPLUG_VST3_DEPLOY_PATH=... を渡す。

# --------------------------------------------------------------------------
# AAX SDK (optional)
# --------------------------------------------------------------------------
# iPlug2 は AAX SDK を iPlug2/Dependencies/IPlug/AAX_SDK 固定で探す。
# 既存 aax-sdk/ をそこへコピーすると BUILD_AAX が ON になる (Phase 1 では未配置=OFF)。
set(AAX_SDK_PATH "${IPLUG2_DIR}/Dependencies/IPlug/AAX_SDK" CACHE PATH "Path to AAX SDK")
if(EXISTS "${AAX_SDK_PATH}/Interfaces/AAX.h")
    message(STATUS "AAX SDK found at: ${AAX_SDK_PATH}")
    set(BUILD_AAX ON)
else()
    message(STATUS "AAX SDK not found at: ${AAX_SDK_PATH} - AAX format will be disabled")
    set(BUILD_AAX OFF)
endif()

# Monkey's Audio SDK (.ape デコード)。静的ライブラリ MACLib をビルドして plugin にリンクする。
add_subdirectory(plugin/lib/mac_sdk)
if(UNIX AND NOT APPLE)
    # Linux 共有ライブラリ(VST3/CLAP)に混ぜるため PIC 化 (Windows/macOS は不要)。
    set_target_properties(MACLib PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# プラグイン定義は plugin/CMakeLists.txt (iplug_add_plugin)。
add_subdirectory(plugin)

# --------------------------------------------------------------------------
# WebUI helper targets
# --------------------------------------------------------------------------
add_custom_target(webui_install
    COMMAND npm install
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/webui
    COMMENT "Installing WebUI dependencies..."
)
add_custom_target(webui_dev
    COMMAND npm run dev
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/webui
    COMMENT "Starting WebUI development server..."
)
add_custom_target(webui_build
    COMMAND npm run build
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/webui
    COMMENT "Building WebUI for production..."
)

# --------------------------------------------------------------------------
# Configuration summary
# --------------------------------------------------------------------------
message(STATUS "")
message(STATUS "MixCompare configuration:")
message(STATUS "  Version:        ${PROJECT_VERSION}")
message(STATUS "  Build type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard:   ${CMAKE_CXX_STANDARD}")
message(STATUS "  iPlug2 dir:     ${IPLUG2_DIR}")
if(BUILD_AAX)
    message(STATUS "  AAX build:      Enabled (${AAX_SDK_PATH})")
else()
    message(STATUS "  AAX build:      Disabled (AAX SDK not found)")
endif()
message(STATUS "")
