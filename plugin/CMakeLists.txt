# MixCompare plugin target — iPlug2 の iplug_add_plugin ヘルパーを使う。
# 旧 JUCE 版は CMakeLists.juce.txt を参照。雛形は Synth80/plugin/CMakeLists.txt。

if(NOT DEFINED IPLUG2_DIR OR NOT EXISTS "${IPLUG2_DIR}/iPlug")
    message(FATAL_ERROR
        "[MixCompare] iPlug2 not found at IPLUG2_DIR='${IPLUG2_DIR}'.\n"
        "  Run `git submodule update --init --recursive` and rerun cmake."
    )
endif()

find_package(iPlug2 REQUIRED)

# macOS Standalone (SWELL) は IPlugAPP_main.cpp が resources/main.rc_mac_dlg /
# main.rc_mac_menu を #include する。main.rc から swell_resgen.pl で生成する派生物で、
# iPlug2 helper は自動実行しないため configure 時に走らせる。
if(APPLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/main.rc")
    set(_resgen_pl "${IPLUG2_DIR}/WDL/swell/swell_resgen.pl")
    set(_main_rc "${CMAKE_CURRENT_SOURCE_DIR}/resources/main.rc")
    set(_rc_dlg "${CMAKE_CURRENT_SOURCE_DIR}/resources/main.rc_mac_dlg")
    set(_rc_menu "${CMAKE_CURRENT_SOURCE_DIR}/resources/main.rc_mac_menu")
    if(EXISTS "${_resgen_pl}")
        if(NOT EXISTS "${_rc_dlg}" OR NOT EXISTS "${_rc_menu}"
           OR "${_main_rc}" IS_NEWER_THAN "${_rc_dlg}"
           OR "${_main_rc}" IS_NEWER_THAN "${_rc_menu}")
            message(STATUS "[MixCompare] Generating SWELL resources from main.rc")
            execute_process(
                COMMAND perl "${_resgen_pl}" "${_main_rc}"
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources"
                RESULT_VARIABLE _resgen_rc
                OUTPUT_QUIET
            )
            if(NOT _resgen_rc EQUAL 0)
                message(FATAL_ERROR "swell_resgen.pl failed (rc=${_resgen_rc}). Make sure perl is installed.")
            endif()
        endif()
    endif()
endif()

# オーディオデコード層 (Phase 2)。dr_libs/stb は単一ヘッダ/単一 C ソース (パブリックドメイン)。
# stb_vorbis.c は C として別途コンパイルし、AudioDecoder.cpp は HEADER_ONLY で宣言だけ取り込む。
set(MIXCOMPARE_AUDIO_SOURCES
    audio/IPlaybackSource.h
    audio/InMemorySource.h
    audio/AudioDecoder.h
    audio/AudioDecoder.cpp
    audio/StreamingDecoder.h
    audio/StreamingDecoder.cpp
    audio/StreamingSource.h
    audio/StreamingSource.cpp
    audio/MediaFoundationDecoder.h
    audio/MediaFoundationDecoder.cpp
    audio/AudioToolboxDecoder.h
    audio/MonkeyAudioDecoder.h
    audio/MonkeyAudioDecoder.cpp
    audio/FileDialog.h
    lib/decoders/dr_wav.h
    lib/decoders/dr_flac.h
    lib/decoders/dr_mp3.h
    lib/decoders/stb_vorbis.c
)
if(WIN32)
    list(APPEND MIXCOMPARE_AUDIO_SOURCES audio/FileDialog_win.cpp)
elseif(APPLE)
    list(APPEND MIXCOMPARE_AUDIO_SOURCES
        audio/FileDialog_mac.mm
        audio/AudioToolboxDecoder.mm)
endif()

iplug_add_plugin(MixCompare
    SOURCES
        MixCompare.cpp
        MixCompare.h
        config.h
        ParameterIDs.h
        core/AudioEngine.h
        core/AudioEngine.cpp
        # 共有 DSP プリミティブ (plugin / wasm 共通の SSoT)。
        dsp/svt_filter.h
        dsp/smoothed_value.h
        dsp/metering.h
        dsp/momentary_processor.h
        ${MIXCOMPARE_AUDIO_SOURCES}
        resources/resource.h
    UI WEBVIEW
    EXCLUDE_FORMATS WAM AUV3 VST2
)

# -----------------------------------------------------------------------------
# Windows: WebUI を全フォーマットの埋め込みリソースとして同梱する。
# iplug_add_plugin は main.rc を APP ターゲットにしか自動追加しないため、
# VST3 / CLAP / AAX には明示的に付与する。これにより IDR_INDEX_HTML
# (= vite-plugin-singlefile が出力した単一 index.html) が全 EXE/DLL に同梱され、
# MixCompare::LoadIndexHtmlForCurrentBuild が FindResource → LoadHTML で読み出せる。
# -----------------------------------------------------------------------------
if(WIN32)
    set(_mixcompare_main_rc "${CMAKE_CURRENT_SOURCE_DIR}/resources/main.rc")
    set(_mixcompare_web_index "${CMAKE_CURRENT_SOURCE_DIR}/resources/web/index.html")
    if(EXISTS "${_mixcompare_main_rc}")
        # rc.exe は RCDATA の web/index.html を依存として追跡しないため、
        # `npm run build` で index.html だけが更新されても main.res が再コンパイル
        # されず古い UI が焼き込まれたままになる。OBJECT_DEPENDS で追跡させる。
        if(EXISTS "${_mixcompare_web_index}")
            set_source_files_properties("${_mixcompare_main_rc}" PROPERTIES
                OBJECT_DEPENDS "${_mixcompare_web_index}")
        endif()
        foreach(_fmt vst3 clap aax)
            set(_target "MixCompare-${_fmt}")
            if(TARGET ${_target})
                target_sources(${_target} PRIVATE "${_mixcompare_main_rc}")
            endif()
        endforeach()
    endif()

    # OpenUrl の ShellExecuteW、ファイルダイアログ COM、Media Foundation (AAC/M4A デコード)。
    foreach(_fmt vst3 clap aax app)
        set(_target "MixCompare-${_fmt}")
        if(TARGET ${_target})
            target_link_libraries(${_target} PRIVATE
                Shell32 Ole32 Shlwapi mfplat mfreadwrite mfuuid)
        endif()
    endforeach()
endif()

# Monkey's Audio (.ape) デコード用 MACLib を全フォーマットにリンク (include は PUBLIC 伝播)。
foreach(_fmt vst3 clap aax au app)
    set(_target "MixCompare-${_fmt}")
    if(TARGET ${_target})
        target_link_libraries(${_target} PRIVATE MACLib)
    endif()
endforeach()

# macOS: AAC/M4A デコード (AudioToolbox) とファイルダイアログ (Cocoa) 用フレームワーク。
if(APPLE)
    foreach(_fmt vst3 clap aax au app)
        set(_target "MixCompare-${_fmt}")
        if(TARGET ${_target})
            target_link_libraries(${_target} PRIVATE
                "-framework AudioToolbox" "-framework Foundation" "-framework Cocoa")
        endif()
    endforeach()
endif()

# -----------------------------------------------------------------------------
# AAX target には AAX_Export OBJECT library の .obj を直接食わせる必要がある
# (INTERFACE → OBJECT の連鎖が MSBuild generator で .obj を伝播しない iPlug2 既知の制約)。
# Phase 1 では AAX SDK 未配置のため AAX target は生成されないが、配置後に有効化される。
# -----------------------------------------------------------------------------
if(TARGET MixCompare-aax AND TARGET AAX_Export)
    target_sources(MixCompare-aax PRIVATE $<TARGET_OBJECTS:AAX_Export>)
    message(STATUS "[MixCompare] Wired AAX_Export object files into MixCompare-aax")
endif()

# -----------------------------------------------------------------------------
# macOS: resources/web/ を各 bundle の Contents/Resources/web/ へ POST_BUILD コピー。
# (Windows は RCDATA 埋め込みなので不要)
# -----------------------------------------------------------------------------
if(APPLE)
    set(MIXCOMPARE_WEB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources/web")
    foreach(_fmt vst3 clap aax au app)
        set(_target "MixCompare-${_fmt}")
        if(TARGET ${_target})
            add_custom_command(TARGET ${_target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${MIXCOMPARE_WEB_DIR}"
                    "$<TARGET_FILE_DIR:${_target}>/../Resources/web"
                COMMENT "[MixCompare] Staging WebUI resources into ${_target} bundle Resources/web"
                VERBATIM
            )
        endif()
    endforeach()
endif()
