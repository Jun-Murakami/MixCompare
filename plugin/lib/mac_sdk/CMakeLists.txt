# Monkey's Audio SDK - 静的ライブラリとして MixCompare に組み込む
# SDK version: 12.60 (BSD 3-Clause License)

cmake_minimum_required(VERSION 3.15)

set(MAC_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# Shared ソース（プラットフォーム共通）
set(MAC_SHARED_SOURCES
    ${MAC_SDK_DIR}/Shared/BufferIO.cpp
    ${MAC_SDK_DIR}/Shared/CharacterHelper.cpp
    ${MAC_SDK_DIR}/Shared/CircleBuffer.cpp
    ${MAC_SDK_DIR}/Shared/CPUFeatures.cpp
    ${MAC_SDK_DIR}/Shared/CRC.cpp
    ${MAC_SDK_DIR}/Shared/GlobalFunctions.cpp
    ${MAC_SDK_DIR}/Shared/MemoryIO.cpp
    ${MAC_SDK_DIR}/Shared/Semaphore.cpp
    ${MAC_SDK_DIR}/Shared/Thread.cpp
    ${MAC_SDK_DIR}/Shared/WholeFileIO.cpp
)

# プラットフォーム別 I/O
if(WIN32)
    list(APPEND MAC_SHARED_SOURCES ${MAC_SDK_DIR}/Shared/WinFileIO.cpp)
else()
    list(APPEND MAC_SHARED_SOURCES ${MAC_SDK_DIR}/Shared/StdLibFileIO.cpp)
endif()

# MACLib ソース
set(MAC_LIB_SOURCES
    ${MAC_SDK_DIR}/MACLib/APECompress.cpp
    ${MAC_SDK_DIR}/MACLib/APECompressCore.cpp
    ${MAC_SDK_DIR}/MACLib/APECompressCreate.cpp
    ${MAC_SDK_DIR}/MACLib/APEDecompress.cpp
    ${MAC_SDK_DIR}/MACLib/APEDecompressCore.cpp
    ${MAC_SDK_DIR}/MACLib/APEHeader.cpp
    ${MAC_SDK_DIR}/MACLib/APEInfo.cpp
    ${MAC_SDK_DIR}/MACLib/APELink.cpp
    ${MAC_SDK_DIR}/MACLib/APETag.cpp
    ${MAC_SDK_DIR}/MACLib/BitArray.cpp
    ${MAC_SDK_DIR}/MACLib/FloatTransform.cpp
    ${MAC_SDK_DIR}/MACLib/MACLib.cpp
    ${MAC_SDK_DIR}/MACLib/MACProgressHelper.cpp
    ${MAC_SDK_DIR}/MACLib/MD5.cpp
    ${MAC_SDK_DIR}/MACLib/NewPredictor.cpp
    ${MAC_SDK_DIR}/MACLib/NNFilter.cpp
    ${MAC_SDK_DIR}/MACLib/NNFilterGeneric.cpp
    ${MAC_SDK_DIR}/MACLib/Prepare.cpp
    ${MAC_SDK_DIR}/MACLib/UnBitArray.cpp
    ${MAC_SDK_DIR}/MACLib/UnBitArrayBase.cpp
    ${MAC_SDK_DIR}/MACLib/WAVInputSource.cpp
    ${MAC_SDK_DIR}/MACLib/Old/AntiPredictorOld.cpp
    ${MAC_SDK_DIR}/MACLib/Old/AntiPredictorExtraHighOld.cpp
    ${MAC_SDK_DIR}/MACLib/Old/AntiPredictorFastOld.cpp
    ${MAC_SDK_DIR}/MACLib/Old/AntiPredictorHighOld.cpp
    ${MAC_SDK_DIR}/MACLib/Old/AntiPredictorNormalOld.cpp
    ${MAC_SDK_DIR}/MACLib/Old/APEDecompressCoreOld.cpp
    ${MAC_SDK_DIR}/MACLib/Old/APEDecompressOld.cpp
    ${MAC_SDK_DIR}/MACLib/Old/UnBitArrayOld.cpp
    ${MAC_SDK_DIR}/MACLib/Old/UnMACOld.cpp
)

# アーキテクチャ別 SIMD フィルタ
# Universal Binary (macOS) で複数アーキテクチャを含む場合は両方のファイルを追加する。
# CMAKE_SYSTEM_PROCESSOR はホスト CPU を返すため、クロスコンパイル時に不正確になる。
set(_include_x86_simd OFF)
set(_include_arm_simd OFF)

if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    foreach(_arch IN LISTS CMAKE_OSX_ARCHITECTURES)
        if(_arch MATCHES "x86_64")
            set(_include_x86_simd ON)
        elseif(_arch MATCHES "arm64")
            set(_include_arm_simd ON)
        endif()
    endforeach()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[3-6]86")
    set(_include_x86_simd ON)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    set(_include_arm_simd ON)
endif()

if(_include_x86_simd)
    list(APPEND MAC_LIB_SOURCES
        ${MAC_SDK_DIR}/MACLib/NNFilterSSE2.cpp
        ${MAC_SDK_DIR}/MACLib/NNFilterSSE4.1.cpp
        ${MAC_SDK_DIR}/MACLib/NNFilterAVX2.cpp
        ${MAC_SDK_DIR}/MACLib/NNFilterAVX512.cpp
    )
endif()
if(_include_arm_simd)
    list(APPEND MAC_LIB_SOURCES
        ${MAC_SDK_DIR}/MACLib/NNFilterNeon.cpp
    )
endif()

# 静的ライブラリターゲット
add_library(MACLib STATIC ${MAC_SHARED_SOURCES} ${MAC_LIB_SOURCES})

target_include_directories(MACLib PUBLIC
    ${MAC_SDK_DIR}/Shared
    ${MAC_SDK_DIR}/MACLib
)

# プラットフォーム定義
if(WIN32)
    target_compile_definitions(MACLib PUBLIC PLATFORM_WINDOWS)
elseif(APPLE)
    target_compile_definitions(MACLib PUBLIC PLATFORM_APPLE)
else()
    target_compile_definitions(MACLib PUBLIC PLATFORM_LINUX)
endif()

# MSVC 固有の設定
if(MSVC)
    target_compile_options(MACLib PRIVATE
        /utf-8
        /wd4005   # macro redefinition (DLLEXPORT, WIN32_LEAN_AND_MEAN)
        /wd4244   # conversion warnings
        /wd4267   # size_t to int
        /wd4018   # signed/unsigned mismatch
        /wd4100   # unreferenced formal parameter
        /wd4127   # conditional expression is constant
        /wd4189   # local variable initialized but not referenced
        /wd4456   # declaration hides previous local
        /wd4457   # declaration hides function parameter
        /wd4458   # declaration hides class member
        /wd4706   # assignment within conditional expression
        /wd4996   # deprecated functions
    )
    # 静的リンク時は DLLEXPORT を空にする + Windows マクロ衝突対策
    target_compile_definitions(MACLib PRIVATE DLLEXPORT= NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(MACLib PRIVATE
        -w  # SDK 内部の警告を全て抑制
    )
    # SIMD コンパイルフラグ（GCC/Clang）
    if(_include_x86_simd)
        if(APPLE AND _include_arm_simd)
            # Universal Binary: -Xarch_x86_64 で x86_64 ビルドにのみ SIMD フラグを渡す
            # arm64 ビルド時はフラグがスキップされ、ソース内の #ifdef ガードにより空実装になる
            set_source_files_properties(${MAC_SDK_DIR}/MACLib/NNFilterSSE2.cpp
                PROPERTIES COMPILE_FLAGS "-Xarch_x86_64 -msse2")
            set_source_files_properties(${MAC_SDK_DIR}/MACLib/NNFilterSSE4.1.cpp
                PROPERTIES COMPILE_FLAGS "-Xarch_x86_64 -msse4.1")
            set_source_files_properties(${MAC_SDK_DIR}/MACLib/NNFilterAVX2.cpp
                PROPERTIES COMPILE_FLAGS "-Xarch_x86_64 -mavx2")
            set_source_files_properties(${MAC_SDK_DIR}/MACLib/NNFilterAVX512.cpp
                PROPERTIES COMPILE_FLAGS "-Xarch_x86_64 -mavx512dq -Xarch_x86_64 -mavx512bw")
        else()
            # 単一アーキテクチャ（x86_64 のみ）または非 Apple
            set_source_files_properties(${MAC_SDK_DIR}/MACLib/NNFilterSSE2.cpp
                PROPERTIES COMPILE_FLAGS "-msse2")
            set_source_files_properties(${MAC_SDK_DIR}/MACLib/NNFilterSSE4.1.cpp
                PROPERTIES COMPILE_FLAGS "-msse4.1")
            set_source_files_properties(${MAC_SDK_DIR}/MACLib/NNFilterAVX2.cpp
                PROPERTIES COMPILE_FLAGS "-mavx2")
            set_source_files_properties(${MAC_SDK_DIR}/MACLib/NNFilterAVX512.cpp
                PROPERTIES COMPILE_FLAGS "-mavx512dq -mavx512bw")
        endif()
    endif()
endif()
