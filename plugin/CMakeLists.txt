# MixCompare プラグイン専用の CMake 設定
# - ここではプラグイン固有のターゲット定義・依存・定義のみを扱います
# - 共有のオプションや WebUI の補助ターゲットはルート側に置きます

# 親（ルート）から渡されたソースディレクトリを使用
# - 物理的なファイル移動を伴わずに段階的に分割するため
if(NOT DEFINED MIXCOMPARE_SRC_DIR)
    # 既定は plugin/src を指す（単体でも解決できるようにする）
    set(MIXCOMPARE_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()

# WebView Files のソースディレクトリ
set(WEBVIEW_FILES_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ui/public")

# JUCE のフロントエンドライブラリをコピー
file(COPY "${JUCE_MODULES_DIR}/juce_gui_extra/native/javascript/" DESTINATION "${WEBVIEW_FILES_SOURCE_DIR}/js/juce/")

# プラットフォーム別アイコン引数を組み立て（JUCE が PNG から各プラットフォーム向けに変換生成します）
# - Windows: juceaide が .ico を生成
# - macOS: juceaide が .icns を生成
set(ICON_ARGS)
set(MIXCOMPARE_ICON_PNG "${CMAKE_SOURCE_DIR}/cmake/icon.png")
if(APPLE)
    list(APPEND ICON_ARGS
        ICON_BIG   "${MIXCOMPARE_ICON_PNG}"
    )
endif()
if(WIN32)
    list(APPEND ICON_ARGS
        ICON_BIG   "${MIXCOMPARE_ICON_PNG}"
        ICON_SMALL "${MIXCOMPARE_ICON_PNG}"
    )
endif()

# macOS 固有の設定（wraptool / codesign のヘッダパッド不足対策）
# - PACE wraptool および Apple の codesign は Mach-O のヘッダ領域に追記が必要になる場合がある
# - リンク時に余白を確保しておくことで、後段の署名・ラップ処理の失敗を防ぐ

# AAX の自動コピー先をユーザー書き込み可能な場所に変更（開発時の権限エラー回避）
if(APPLE)
    # juce_add_plugin がコピー手順を生成する前にグローバルプロパティで上書き
    set_property(GLOBAL PROPERTY JUCE_AAX_COPY_DIR "${CMAKE_BINARY_DIR}/AAX_Plugins")
endif()

# プラグイン定義（JUCE v8）
juce_add_plugin(MixCompare
    VERSION ${PROJECT_VERSION}                     # プラグインのバージョン（JUCEは数値のみ受け付ける）
    PRODUCT_NAME "MixCompare"                      # 製品名
    COMPANY_NAME "Jun Murakami"                    # 会社名
    BUNDLE_ID "com.junmurakami.mixcompare"         # Bundle ID（スペースなし）
    PLUGIN_MANUFACTURER_CODE Jmbc                  # メーカーコード（4文字）
    PLUGIN_CODE Mx03                               # プラグインコード（4文字）
    FORMATS AU VST3 Standalone AAX  # 対応フォーマット（AAXが有効な場合のみビルドされる）
    PLUGIN_AU_MAIN_TYPE "aufx"                     # Audio Unit メインタイプ（エフェクト）
    VST3_CATEGORIES "Analyzer"                     # VST3カテゴリ
    AAX_CATEGORY "AAX_ePlugInCategory_SWGenerators|AAX_ePlugInCategory_SoundField"  # AAXカテゴリ（インストゥルメント扱い）
    IS_SYNTH FALSE                                 # シンセではない（AAX では後で上書き）
    NEEDS_MIDI_INPUT FALSE                         # MIDI 入力不要
    NEEDS_MIDI_OUTPUT FALSE                        # MIDI 出力不要
    IS_MIDI_EFFECT FALSE                           # MIDI エフェクトではない
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE               # エディタにキーボードフォーカス
    COPY_PLUGIN_AFTER_BUILD TRUE                   # ビルド後に所定場所へコピー
    ${ICON_ARGS}

    # WebView を利用する旨を JUCE に通知
    NEEDS_WEB_BROWSER TRUE
    NEEDS_WEBVIEW2 TRUE                            # Windows の WebView2 を要求
)

# JUCE の共通ヘッダー(JuceHeader.h)を生成し、インクルードパスへ追加
juce_generate_juce_header(MixCompare)

# ソースファイルの登録
target_sources(MixCompare PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PluginEditor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PluginEditor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PluginProcessor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PluginProcessor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/MediaFoundationAACFormat.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/MediaFoundationAACFormat.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/MonkeyAudioFormat.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/MonkeyAudioFormat.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/InMemoryPlaybackSource.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/InMemoryPlaybackSource.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/MomentaryProcessor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/MomentaryProcessor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/StateManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/StateManager.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/AudioEngine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/AudioEngine.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/TransportManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/TransportManager.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/PlaylistManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/PlaylistManager.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/MeteringService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/MeteringService.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/CrashHandler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/CrashHandler.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/NullPlaybackSource.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/StreamingPlaybackSource.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/StreamingPlaybackSource.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/ErrorManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/ErrorManager.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/SystemInfo.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/KeyEventForwarder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/KeyEventForwarder.h
)

if(APPLE)
    target_sources(MixCompare PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/KeyEventForwarder_mac.mm
    )
endif()

# バージョン情報ヘッダーの生成
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/Version.h
    @ONLY
)

# インクルードパス
target_include_directories(MixCompare PRIVATE
    ${MIXCOMPARE_SRC_DIR}
    ${MIXCOMPARE_SRC_DIR}/audio
    ${MIXCOMPARE_SRC_DIR}/bridge
    ${MIXCOMPARE_SRC_DIR}/core
    ${CMAKE_CURRENT_BINARY_DIR}  # 生成されたVersion.hのために追加
)

# JUCE ヘッダーをシステムヘッダーとして追加（エディタ/静的解析向け）
target_include_directories(MixCompare SYSTEM PUBLIC ${JUCE_MODULES_DIR})

# 必要な JUCE モジュールと推奨フラグ
target_link_libraries(MixCompare
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra                 # WebView に必要
        MACLib                               # Monkey's Audio SDK（APE デコーダー）
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# macOS 固有の設定（wraptool / codesign のヘッダパッド不足対策）
# - PACE wraptool および Apple の codesign は Mach-O のヘッダ領域に追記が必要になる場合がある
# - juce_add_plugin によりターゲットが生成された「後」で設定を適用する
if(APPLE)
    # メインターゲット
    if (TARGET MixCompare)
        target_link_options(MixCompare PRIVATE -Wl,-headerpad,578)
        # macOS のキーボードレイアウト API は ApplicationServices 経由で解決可能
        target_link_libraries(MixCompare PUBLIC "-framework ApplicationServices")
    endif()

    # JUCE が生成するバリアントターゲット（存在する場合のみ）
    foreach(wrapperTarget IN ITEMS MixCompare_VST3 MixCompare_AU MixCompare_Standalone MixCompare_AAX)
        if (TARGET ${wrapperTarget})
            target_link_options(${wrapperTarget} PRIVATE -Wl,-headerpad,578)
            target_link_libraries(${wrapperTarget} PRIVATE "-framework ApplicationServices")
        endif()
    endforeach()
endif()

# 共通のコンパイル定義（WebView を有効化）
target_compile_definitions(MixCompare PUBLIC
    JUCE_WEB_BROWSER=1                         # WebBrowserComponent（WebView）有効化
    JUCE_USE_CURL=0                            # curl は不要
    JUCE_VST3_CAN_REPLACE_VST2=0               # VST2 置換を無効
    JUCE_WIN_PER_MONITOR_DPI_AWARE=1           # Windows で DPI 対応
    $<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:JUCE_DEBUG=1>  # Debug/RelWithDebInfo で DevTools 有効
    $<$<PLATFORM_ID:Windows>:JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1>  # WebView2静的リンク

    JUCE_USE_FLAC=1                            # FLACデコーダー有効化
    JUCE_USE_OGGVORBIS=1                        # Ogg Vorbisデコーダー有効化
    JUCE_USE_MP3AUDIOFORMAT=1

    # プラグイン情報（オーディオホスト向けメタデータ）
    # Name/Desc/Manufacturer/Version は juce_add_plugin() から自動生成されるため重複定義を避ける
    JucePlugin_ManufacturerCode=0x4a6d6263     # 'Jmbc'
    JucePlugin_PluginCode=0x4d783033           # 'Mx03'
    JucePlugin_IsSynth=0
    JucePlugin_WantsMidiInput=0
    JucePlugin_ProducesMidiOutput=0
    JucePlugin_IsMidiEffect=0
    JucePlugin_EditorRequiresKeyboardFocus=1
)

# AAX ビルド時のみ IS_SYNTH を TRUE に上書きして、入力無しでもレンダーが走るようにする
if (JUCE_BUILD_AAX)
    target_compile_definitions(MixCompare PUBLIC
        JucePlugin_IsSynth=1
        JucePlugin_WantsMidiInput=0
        JucePlugin_ProducesMidiOutput=0
        JucePlugin_IsMidiEffect=0)
endif()

# Dev/Prod の読み分け（Dev: Vite, Prod: 埋め込み）
# Visual Studio 等のマルチコンフィグ環境でも正しく動作するよう、
# $<CONFIG:...> のジェネレータ式で切り替える。

# Debug/RelWithDebInfo では Dev サーバーを使用
target_compile_definitions(MixCompare PRIVATE
    $<$<CONFIG:Debug>:MIXCOMPARE_DEV_MODE=1>
    $<$<CONFIG:Debug>:LOCAL_DEV_SERVER_ADDRESS="http://127.0.0.1:5173">
    $<$<NOT:$<CONFIG:Debug>>:MIXCOMPARE_DEV_MODE=0>
)

# ZIP 化および埋め込み（Prod 系コンフィグ向け）
set(WEBVIEW_FILES_ZIP_NAME "webview_files.zip")
set(TARGET_WEBVIEW_FILES_ZIP_PATH "${CMAKE_BINARY_DIR}/${WEBVIEW_FILES_ZIP_NAME}")
cmake_path(ABSOLUTE_PATH WEBVIEW_FILES_SOURCE_DIR NORMALIZE OUTPUT_VARIABLE PUBLIC_PATH )
cmake_path(GET PUBLIC_PATH PARENT_PATH WORKING_DIRECTORY)

# 初回構成時のみ、既存の zip を削除してクリーンな再生成を強制する
if (NOT DEFINED MIXCOMPARE_WEBVIEW_INITIAL_CLEAN_DONE OR NOT MIXCOMPARE_WEBVIEW_INITIAL_CLEAN_DONE)
    if (EXISTS "${TARGET_WEBVIEW_FILES_ZIP_PATH}")
        file(REMOVE "${TARGET_WEBVIEW_FILES_ZIP_PATH}")
    endif()
    set(MIXCOMPARE_WEBVIEW_INITIAL_CLEAN_DONE ON CACHE BOOL "Initial WebView zip clean executed" FORCE)
endif()

# 入力アセットの変更があるときのみ zip を再生成するようにする
file(GLOB_RECURSE WEBVIEW_ASSETS CONFIGURE_DEPENDS
    "${WEBVIEW_FILES_SOURCE_DIR}/**/*")

add_custom_command(
    OUTPUT "${TARGET_WEBVIEW_FILES_ZIP_PATH}"
    COMMAND ${CMAKE_COMMAND} -E tar cvf
            "${TARGET_WEBVIEW_FILES_ZIP_PATH}"
            --format=zip
            "${PUBLIC_PATH}"
    DEPENDS ${WEBVIEW_ASSETS}
    WORKING_DIRECTORY "${WORKING_DIRECTORY}"
    COMMENT "Zipping WebView files (up-to-date aware)..."
    VERBATIM
)

add_custom_target(ZipWebViewFiles
    DEPENDS "${TARGET_WEBVIEW_FILES_ZIP_PATH}"
)

# ZIP プレフィックスは Prod 系コンフィグのみ定義
cmake_path(GET PUBLIC_PATH FILENAME ZIPPED_FILES_PREFIX)
target_compile_definitions(MixCompare PRIVATE
    $<$<NOT:$<CONFIG:Debug>>:ZIPPED_FILES_PREFIX="${ZIPPED_FILES_PREFIX}/">
)

# WebUI ソースをバイナリデータとしてパッケージ（ターゲット自体は常に生成）
juce_add_binary_data(WebViewFiles
    HEADER_NAME WebViewFiles.h
    NAMESPACE webview_files
    SOURCES ${TARGET_WEBVIEW_FILES_ZIP_PATH}
)
add_dependencies(WebViewFiles ZipWebViewFiles)

# 埋め込みライブラリのリンクは Prod 系コンフィグのときだけ有効
target_link_libraries(MixCompare PRIVATE
    $<$<NOT:$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>>:WebViewFiles>
)

# Windows 固有の設定
if(WIN32)
    # Windows の冗長マクロ対策
    target_compile_definitions(MixCompare PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    # UTF-8エンコーディングを強制
    target_compile_options(MixCompare PRIVATE
        /utf-8
    )
    target_link_libraries(MixCompare PRIVATE Dbghelp)
endif()
if(MSVC)
    # 全ビルド構成で PDB を生成
    foreach(wrapperTarget IN ITEMS MixCompare MixCompare_VST3 MixCompare_Standalone MixCompare_AAX)
        if (TARGET ${wrapperTarget})
            target_compile_options(${wrapperTarget} PRIVATE
                $<$<CONFIG:Debug>:/Zi>
                $<$<CONFIG:Release>:/Zi>
                $<$<CONFIG:RelWithDebInfo>:/Zi>
                $<$<CONFIG:MinSizeRel>:/Zi>
            )
            target_link_options(${wrapperTarget} PRIVATE
                $<$<CONFIG:Debug>:/DEBUG:FULL>
                $<$<CONFIG:Release>:/DEBUG:FULL>
                $<$<CONFIG:RelWithDebInfo>:/DEBUG:FULL>
                $<$<CONFIG:MinSizeRel>:/DEBUG:FULL>
            )
        endif()
    endforeach()

    # Ogg Vorbis の C4703 をターゲット全体で抑止（安全のため各ラッパーターゲットにも付与）
    foreach(wrapperTarget IN ITEMS MixCompare MixCompare_VST3 MixCompare_Standalone MixCompare_AAX)
        if (TARGET ${wrapperTarget})
            target_compile_options(${wrapperTarget} PRIVATE /wd4703)
        endif()
    endforeach()
endif()



