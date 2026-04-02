# MixCompare - WAVファイルプレイヤー内蔵の仮想ソースセレクター
# JUCE + WebView（Vite/React/MUI）構成

cmake_minimum_required(VERSION 3.22)

# macOS の最小対応バージョンを明示
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

# バージョン情報を読み込む
include(${CMAKE_SOURCE_DIR}/cmake/Version.cmake)

# プロジェクト定義（バージョンは Version.cmake から取得）
project(MixCompare VERSION ${PROJECT_VERSION})

# C++17標準を使用
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCEを追加
add_subdirectory(JUCE)

# AAX SDKの設定
set(AAX_SDK_PATH "${CMAKE_SOURCE_DIR}/aax-sdk" CACHE PATH "Path to AAX SDK")
if(EXISTS "${AAX_SDK_PATH}/Interfaces/AAX.h")
    message(STATUS "AAX SDK found at: ${AAX_SDK_PATH}")
    juce_set_aax_sdk_path("${AAX_SDK_PATH}")
    
    # AAXライブラリのパスを設定（プラットフォーム別）
    if(WIN32)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            # 64-bit Windows
            set(AAX_LIBRARY_PATH "${AAX_SDK_PATH}/Libs/Release/AAXLibrary.lib")
        else()
            # 32-bit Windows (rarely used)
            set(AAX_LIBRARY_PATH "${AAX_SDK_PATH}/Libs/Release/AAXLibrary_x86.lib")
        endif()
    elseif(APPLE)
        # macOS - アーキテクチャに応じてパスを設定
        if(CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
            # Apple Silicon
            set(AAX_LIBRARY_PATH "${AAX_SDK_PATH}/Libs/arm64/Release/libAAXLibrary.a")
        elseif(CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
            # Intel Mac
            set(AAX_LIBRARY_PATH "${AAX_SDK_PATH}/Libs/x86_64/Release/libAAXLibrary.a")
        else()
            # Universal Binary or default
            set(AAX_LIBRARY_PATH "${AAX_SDK_PATH}/Libs/x86_64_arm64/Release/libAAXLibrary.a")
            # フォールバック：単一アーキテクチャライブラリを探す
            if(NOT EXISTS "${AAX_LIBRARY_PATH}")
                if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm64")
                    set(AAX_LIBRARY_PATH "${AAX_SDK_PATH}/Libs/arm64/Release/libAAXLibrary.a")
                else()
                    set(AAX_LIBRARY_PATH "${AAX_SDK_PATH}/Libs/x86_64/Release/libAAXLibrary.a")
                endif()
            endif()
        endif()
    endif()
    
    if(EXISTS "${AAX_LIBRARY_PATH}")
        message(STATUS "AAX Library found at: ${AAX_LIBRARY_PATH}")
        set(BUILD_AAX ON)
    else()
        message(WARNING "AAX Library not found at: ${AAX_LIBRARY_PATH} - AAX format will be disabled")
        set(BUILD_AAX OFF)
    endif()
else()
    message(WARNING "AAX SDK not found at: ${AAX_SDK_PATH} - AAX format will be disabled")
    set(BUILD_AAX OFF)
endif()

# Monkey's Audio SDK（APE デコーダー）
add_subdirectory(plugin/lib/mac_sdk)

# 共有設定: 子ディレクトリへ渡すソースルート（plugin 配下へ集約）
set(MIXCOMPARE_SRC_DIR "${CMAKE_SOURCE_DIR}/plugin/src")

# プラグイン定義は plugin/CMakeLists.txt に分離
add_subdirectory(plugin)

# カスタムターゲット: WebUIのビルド
add_custom_target(webui_build
    COMMAND npm run build
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/webui
    COMMENT "Building WebUI with Vite..."
)

# カスタムターゲット: WebUIの開発サーバー起動
add_custom_target(webui_dev
    COMMAND npm run dev
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/webui
    COMMENT "Starting WebUI development server..."
)

# WebUIの依存関係インストール
add_custom_target(webui_install
    COMMAND npm install
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/webui
    COMMENT "Installing WebUI dependencies..."
)

# WebView2 静的リンク切替オプション（既定: OFF = 動的リンク）
option(MIXCOMPARE_WEBVIEW2_STATIC_LINKING "Link WebView2 statically on Windows" OFF)

# Windows用WebView2の追加設定
if(WIN32)
    message(STATUS "Configuring WebView2 for Windows...")
    if(MIXCOMPARE_WEBVIEW2_STATIC_LINKING)
        message(STATUS "  WebView2: Static linking enabled")
    else()
        message(STATUS "  WebView2: Dynamic linking enabled")
    endif()
endif()

message(STATUS "")
message(STATUS "MixCompare Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Source Directory: ${CMAKE_SOURCE_DIR}")
message(STATUS "  Binary Directory: ${CMAKE_BINARY_DIR}")
if(BUILD_AAX)
    message(STATUS "  AAX SDK: ${AAX_SDK_PATH}")
    message(STATUS "  AAX Build: Enabled")
else()
    message(STATUS "  AAX Build: Disabled (AAX SDK not found)")
endif()
if(WIN32)
    if(MIXCOMPARE_WEBVIEW2_STATIC_LINKING)
        message(STATUS "  WebView2: Static linking enabled")
    else()
        message(STATUS "  WebView2: Dynamic linking enabled")
    endif()
endif()
message(STATUS "")
message(STATUS "Available targets:")
message(STATUS "  MixCompare         - Build the main plugin")
message(STATUS "  webui_install      - Install WebUI dependencies")
message(STATUS "  webui_dev          - Start WebUI development server")
message(STATUS "  webui_build        - Build WebUI for production")
message(STATUS "  run-tests          - Run unit tests")
message(STATUS "  run-tests-ci       - Run tests with CI output format")
message(STATUS "")

# テストのビルドオプション
option(MIXCOMPARE_BUILD_TESTS "Build unit tests" OFF)
if(MIXCOMPARE_BUILD_TESTS)
    add_subdirectory(tests)
    message(STATUS "Unit tests enabled")
endif()
