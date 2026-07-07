// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "ParameterIDs.h"
#include "util/DiagnosticLog.h"
#include "Version.h" // CMakeで自動生成されるバージョン情報ヘッダー（VERSIONファイル由来）

// CMake 未構成時（IntelliSense/分岐切替直後など、生成済み Version.h が include パスに無い状態）でも
//  コンパイル・解析が通るようフォールバックを定義する。実ビルドでは Version.h の値が優先される。
#ifndef MIXCOMPARE_VERSION_STRING
 #define MIXCOMPARE_VERSION_STRING "0.0.0-dev"
#endif
#include "core/FormatUtils.h" // 表示用フォーマット（周波数）
#include "core/MeteringService.h"
#include "KeyEventForwarder.h"
#include <unordered_map>
#include <optional>
#include <cstdlib> // _dupenv_s, _putenv_s（Windows 環境変数操作）
#include <cmath>   // std::abs（スケール差分の小数比較）
#if defined(JUCE_WINDOWS)
 #include <windows.h>
#endif

// 開発/本番での WebViewFiles 提供は条件付きインクルードのみに集約し、
// useLocalDevServer の重複定義を避ける
#if __has_include(<WebViewFiles.h>)
#include <WebViewFiles.h>
#endif

#ifndef LOCAL_DEV_SERVER_ADDRESS
#define LOCAL_DEV_SERVER_ADDRESS "http://127.0.0.1:5173"
#endif

namespace {
    
std::vector<std::byte> streamToVector(juce::InputStream& stream) {
    using namespace juce;
    const auto sizeInBytes = static_cast<size_t>(stream.getTotalLength());
    std::vector<std::byte> result(sizeInBytes);
    stream.setPosition(0);
    [[maybe_unused]] const auto bytesRead =
        stream.read(result.data(), result.size());
    jassert(bytesRead == static_cast<ssize_t>(sizeInBytes));
    return result;
}

#if !MIXCOMPARE_DEV_MODE && __has_include(<WebViewFiles.h>)
static const char* getMimeForExtension(const juce::String& extension) {
    static const std::unordered_map<juce::String, const char*> mimeMap = {
        {{"htm"}, "text/html"},
        {{"html"}, "text/html"},
        {{"txt"}, "text/plain"},
        {{"jpg"}, "image/jpeg"},
        {{"jpeg"}, "image/jpeg"},
        {{"svg"}, "image/svg+xml"},
        {{"ico"}, "image/vnd.microsoft.icon"},
        {{"json"}, "application/json"},
        {{"png"}, "image/png"},
        {{"css"}, "text/css"},
        {{"map"}, "application/json"},
        {{"js"}, "text/javascript"},
        {{"woff2"}, "font/woff2"}};

    if (const auto it = mimeMap.find(extension.toLowerCase());
        it != mimeMap.end())
        return it->second;

    jassertfalse;
    return "";
}
#endif

#if !MIXCOMPARE_DEV_MODE && __has_include(<WebViewFiles.h>)
#ifndef ZIPPED_FILES_PREFIX
#error "You must provide the prefix of zipped web UI files' paths, e.g., 'public/', in the ZIPPED_FILES_PREFIX compile definition"
#endif

std::vector<std::byte> getWebViewFileAsBytes(const juce::String& filepath) {
    juce::MemoryInputStream zipStream{webview_files::webview_files_zip,
                                      webview_files::webview_files_zipSize,
                                      false};
    juce::ZipFile zipFile{zipStream};
    
    // デバッグ用ダンプは削除
    static bool firstCall = true;
    if (firstCall) { firstCall = false; }
    
    const auto fullPath = ZIPPED_FILES_PREFIX + filepath;
    
    if (auto* zipEntry = zipFile.getEntry(fullPath)) {
        const std::unique_ptr<juce::InputStream> entryStream{
            zipFile.createStreamForEntry(*zipEntry)};

        if (entryStream == nullptr) {
            jassertfalse;
            return {};
        }

        return streamToVector(*entryStream);
    }
    
    
    return {};
}
#else
// デバッグビルド用のスタブ実装（DEV_MODE では Vite dev server から読み込むため未使用）
[[maybe_unused]] static std::vector<std::byte> getWebViewFileAsBytes(const juce::String& filepath) {
    juce::ignoreUnused(filepath);
    return {};
}
#endif

#if defined(JUCE_WINDOWS)
// HWND 基準の DPI を取得し、スケール係数へ変換
static void queryWindowDpi(HWND hwnd, int& outDpi, double& outScale)
{
    outDpi = 0;
    outScale = 1.0;
    if (hwnd == nullptr) return;

    // Windows 10 以降: GetDpiForWindow が最も信頼できる（Per-Monitor V2）
    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32)
    {
        using GetDpiForWindowFn = UINT (WINAPI*)(HWND);
        auto pGetDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(::GetProcAddress(user32, "GetDpiForWindow"));
        if (pGetDpiForWindow)
        {
            UINT dpi = pGetDpiForWindow(hwnd);
            if (dpi != 0)
            {
                outDpi = static_cast<int>(dpi);
                outScale = static_cast<double>(dpi) / 96.0; // 96dpi を 100% とする
                return;
            }
        }
    }

    // フォールバック: モニター DPI（GetDpiForMonitor）
    HMODULE shcore = ::LoadLibraryW(L"Shcore.dll");
    if (shcore)
    {
        using GetDpiForMonitorFn = HRESULT (WINAPI*)(HMONITOR, int, UINT*, UINT*);
        auto pGetDpiForMonitor = reinterpret_cast<GetDpiForMonitorFn>(::GetProcAddress(shcore, "GetDpiForMonitor"));
        if (pGetDpiForMonitor)
        {
            HMONITOR mon = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            UINT dpiX = 0, dpiY = 0;
            if (SUCCEEDED(pGetDpiForMonitor(mon, 0 /*MDT_EFFECTIVE_DPI*/, &dpiX, &dpiY)))
            {
                outDpi = static_cast<int>(dpiX);
                outScale = static_cast<double>(dpiX) / 96.0;
            }
        }
        ::FreeLibrary(shcore);
    }
}
#endif

} // namespace

// WebView2/Chromium の起動前に追加のコマンドライン引数を渡すためのヘルパー。
// JUCE 本体に手を入れず、環境変数 WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS を介して
// `--force-device-scale-factor=1` を注入する。
// 注意: WebView2 のブラウザプロセス生成前（= WebBrowserComponent の構築前）に呼ぶ必要がある。
static juce::WebBrowserComponent::Options makeWebViewOptionsWithPreLaunchArgs(const juce::AudioProcessor& /*processor*/)
{
#if defined(JUCE_WINDOWS)
    // ホスト判定で Pro Tools 実行かつ AAX ラッパーの時のみ適用。
    if (juce::PluginHostType().isProTools()
        && juce::PluginHostType::getPluginLoadedAs() == juce::AudioProcessor::WrapperType::wrapperType_AAX)
    {
       #if defined(_WIN32)
    const char* kEnvName = "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS";
    const char* kArg     = "--force-device-scale-factor=1";

    char* existing = nullptr;
    size_t len = 0;
    if (_dupenv_s(&existing, &len, kEnvName) == 0 && existing != nullptr)
    {
        std::string combined(existing);
        free(existing);

        // 既に同等の指定が含まれていなければ追記（半角スペースで区切る）
        if (combined.find("--force-device-scale-factor") == std::string::npos)
        {
            if (!combined.empty()) combined += ' ';
            combined += kArg;
            _putenv_s(kEnvName, combined.c_str());
        }
        // 既に含まれていれば何もしない（ユーザー/環境で明示設定を尊重）
    }
    else
    {
        _putenv_s(kEnvName, kArg);
    }
       #endif
    }
#endif

    // 以降、通常どおり Options を構築して呼び出し側でチェーンを続ける
    return juce::WebBrowserComponent::Options{};
}

std::atomic<uint64_t> MixCompare3AudioProcessorEditor::transportSessionIdGenerator{0};

MixCompare3AudioProcessorEditor::MixCompare3AudioProcessorEditor(MixCompare3AudioProcessor& p)
    : AudioProcessorEditor(&p),
      audioProcessor(p),
      webHostGainRelay{mc3::id::HOST_GAIN.getParamID()},
      webPlaylistGainRelay{mc3::id::PLAYLIST_GAIN.getParamID()},
      webLpfFreqRelay{mc3::id::LPF_FREQ.getParamID()}, 
      webLpfEnabledRelay{mc3::id::LPF_ENABLED.getParamID()},
      webSourceSelectRelay{mc3::id::SOURCE_SELECT.getParamID()},
      webMeteringModeRelay{mc3::id::METERING_MODE.getParamID()},
      webTransportPlayingRelay{mc3::id::TRANSPORT_PLAYING.getParamID()},
      webLoopEnabledRelay{mc3::id::TRANSPORT_LOOP_ENABLED.getParamID()},
      webTransportSeekNormRelay{mc3::id::TRANSPORT_SEEK_NORM.getParamID()},
      webLoopStartNormRelay{mc3::id::LOOP_START_NORM.getParamID()},
      webLoopEndNormRelay{mc3::id::LOOP_END_NORM.getParamID()},
      webPlaylistCurrentIndexNormRelay{mc3::id::PLAYLIST_CURRENT_INDEX_NORM.getParamID()},
      webHostSyncEnabledRelay{mc3::id::HOST_SYNC_ENABLED.getParamID()},
      webHostSyncCapableRelay{mc3::id::HOST_SYNC_CAPABLE.getParamID()},
      hostGainAttachment{*p.getState().getParameter(mc3::id::HOST_GAIN.getParamID()), webHostGainRelay, nullptr},
      playlistGainAttachment{*p.getState().getParameter(mc3::id::PLAYLIST_GAIN.getParamID()), webPlaylistGainRelay, nullptr},
      lpfFreqAttachment{*p.getState().getParameter(mc3::id::LPF_FREQ.getParamID()), webLpfFreqRelay, nullptr},
      lpfEnabledAttachment{*p.getState().getParameter(mc3::id::LPF_ENABLED.getParamID()), webLpfEnabledRelay, nullptr},
      sourceSelectAttachment{*p.getState().getParameter(mc3::id::SOURCE_SELECT.getParamID()), webSourceSelectRelay, nullptr},
      meteringModeAttachment{*p.getState().getParameter(mc3::id::METERING_MODE.getParamID()), webMeteringModeRelay, nullptr},
      transportPlayingAttachment{*p.getState().getParameter(mc3::id::TRANSPORT_PLAYING.getParamID()), webTransportPlayingRelay, nullptr},
      loopEnabledAttachment{*p.getState().getParameter(mc3::id::TRANSPORT_LOOP_ENABLED.getParamID()), webLoopEnabledRelay, nullptr},
      transportSeekNormAttachment{*p.getState().getParameter(mc3::id::TRANSPORT_SEEK_NORM.getParamID()), webTransportSeekNormRelay, nullptr},
      loopStartNormAttachment{*p.getState().getParameter(mc3::id::LOOP_START_NORM.getParamID()), webLoopStartNormRelay, nullptr},
      loopEndNormAttachment{*p.getState().getParameter(mc3::id::LOOP_END_NORM.getParamID()), webLoopEndNormRelay, nullptr},
      playlistCurrentIndexNormAttachment{*p.getState().getParameter(mc3::id::PLAYLIST_CURRENT_INDEX_NORM.getParamID()), webPlaylistCurrentIndexNormRelay, nullptr},
      hostSyncEnabledAttachment{*p.getState().getParameter(mc3::id::HOST_SYNC_ENABLED.getParamID()), webHostSyncEnabledRelay, nullptr},
      hostSyncCapableAttachment{*p.getState().getParameter(mc3::id::HOST_SYNC_CAPABLE.getParamID()), webHostSyncCapableRelay, nullptr},
      webView{
          makeWebViewOptionsWithPreLaunchArgs(p)
              .withBackend(juce::WebBrowserComponent::Options::Backend::webview2)
              .withWinWebView2Options(
                  juce::WebBrowserComponent::Options::WinWebView2{}
                      // WebView 背景もエディタ背景と同一色で即時塗りつぶし
                      .withBackgroundColour(juce::Colour(0xFF606F77))
                      .withUserDataFolder(juce::File::getSpecialLocation(
                          juce::File::SpecialLocationType::tempDirectory)))
              .withWebViewLifetimeListener(&webViewLifetimeGuard)
              .withNativeIntegrationEnabled()
              .withInitialisationData("vendor", "MixCompare")
              .withInitialisationData("pluginName", "MixCompare")
              .withInitialisationData("pluginVersion", MIXCOMPARE_VERSION_STRING) // バージョンはCMakeのVERSIONファイルから注入
              // WebViewPluginDemo と同様にコントロール索引をDOMに注入
              .withOptionsFrom(controlParameterIndexReceiver)
              
              // プレイリスト管理
              .withNativeFunction(
                  juce::Identifier{"playlist_action"},
                  [this](const juce::Array<juce::var>& args,
                         juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                      handlePlaylistAction(args, std::move(completion));
                  })
              
              // 旧: トランスポート制御（APVTS直結へ移行のため削除）
              
              // ウィンドウ操作（リサイズ等）
              .withNativeFunction(
                  juce::Identifier{"window_action"},
                  [this](const juce::Array<juce::var>& args,
                         juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                      handleWindowAction(args, std::move(completion));
                  })
              
              // システム操作（初期化等）
              .withNativeFunction(
                  juce::Identifier{"system_action"},
                  [this](const juce::Array<juce::var>& args,
                         juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                      handleSystemAction(args, std::move(completion));
                  })

              // URL をシステムブラウザで開く
              .withNativeFunction(
                  juce::Identifier{"open_url"},
                  [this](const juce::Array<juce::var>& args,
                         juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                      if (args.size() < 1)
                      {
                          completion(juce::var{false});
                          return;
                      }

                      const juce::String url = args[0].toString();
                      if (url.isEmpty())
                      {
                          completion(juce::var{false});
                          return;
                      }

                      // JUCEのURL::launchInDefaultBrowserを使用してOSのブラウザで開く
                      const bool success = juce::URL(url).launchInDefaultBrowser();
                      completion(juce::var{success});
                  })

              // メータリングのリセット（Momentary/TruePeak）
              // - フロント側のMomentaryホールド数値クリックから呼ばれる
              // - 引数: 省略可 / "momentary" / "peak" (true peak)
              // - 既定は両方リセット
              .withNativeFunction(
                  juce::Identifier{"metering_reset"},
                  [this](const juce::Array<juce::var>& args,
                         juce::WebBrowserComponent::NativeFunctionCompletion completion) {
                      // 安全な引数取り出し（存在しない場合は空文字）
                      const juce::String type = (args.size() >= 1) ? args[0].toString() : juce::String{};

                      // MeteringService に対して適切なリセットを実行（UIスレッド）
                      if (auto* ms = audioProcessor.getMeteringService())
                      {
                          // Momentaryホールドのリセット
                          if (type.isEmpty() || type.equalsIgnoreCase("momentary"))
                          {
                              ms->resetMomentaryHold();
                              
                          }

                          // TruePeak（区間最大）リセット
                          if (type.isEmpty() || type.equalsIgnoreCase("peak") || type.equalsIgnoreCase("truepeak"))
                          {
                              ms->resetTruePeakMeters();
                              
                          }
                      }

                      // WebUI にリセット通知（数値表示の強制更新に利用）
                      sendMeteringReset();

                      completion(juce::var{ true });
                  })
              
              // イベントリスナー
              .withEventListener("requestPlaylistUpdate",
                  [this](juce::var data) {
                      juce::ignoreUnused(data);
                      // プレイリスト状態を送信（リビジョン管理込み）
                      sendPlaylistUpdate();
                  })
              
              .withOptionsFrom(webHostGainRelay)
              .withOptionsFrom(webPlaylistGainRelay)
              .withOptionsFrom(webLpfFreqRelay)
              .withOptionsFrom(webLpfEnabledRelay)
              .withOptionsFrom(webSourceSelectRelay)
              .withOptionsFrom(webMeteringModeRelay)
              .withOptionsFrom(webTransportPlayingRelay)
              .withOptionsFrom(webLoopEnabledRelay)
              .withOptionsFrom(webTransportSeekNormRelay)
              .withOptionsFrom(webLoopStartNormRelay)
              .withOptionsFrom(webLoopEndNormRelay)
              .withOptionsFrom(webPlaylistCurrentIndexNormRelay)
              .withOptionsFrom(webHostSyncEnabledRelay)
              .withOptionsFrom(webHostSyncCapableRelay)
              // Resource provider for embedded WebUI
#if !MIXCOMPARE_DEV_MODE
              .withResourceProvider(
                  [safe = juce::Component::SafePointer(this)](const juce::String& path) -> std::optional<Resource> {
                      if (safe && !safe->isShuttingDown.load())
                          return safe->getResource(path);
                      return std::nullopt;
                  })
#endif
              }
{
    // デバッグモードでは開発サーバーを使用
#if MIXCOMPARE_DEV_MODE
    useLocalDevServer = true;
#else
    useLocalDevServer = false;
#endif
    
    addAndMakeVisible(webView);
    // エディタ自体を不透明化してホスト背景の透過を抑止
    setOpaque(true);
    // WebView は最終的に全域を覆うため不透明指定（WebView2 背景色と一致）
    webView.setOpaque(true);
    
    // エラーマネージャーのコールバック設定
    MixCompare::ErrorManager::getInstance().setErrorCallback(
        [this](const MixCompare::ErrorInfo& error)
        {
            // メッセージスレッドで実行
            juce::MessageManager::callAsync([this, error]()
            {
                sendErrorNotification(error);
            });
        });
    
    // WebView URLセット
    // Debug: dev server info（必要時のみ手動で有効化）
    // DBG("useLocalDevServer: " << (useLocalDevServer ? "true" : "false"));
    // DBG("LOCAL_DEV_SERVER_ADDRESS: " << LOCAL_DEV_SERVER_ADDRESS);
    
    if (useLocalDevServer)
    {
        // デバッグ用: version-check.htmlをテスト
        const bool testVersionCheck = false;  // メインUIに戻す
        
        if (testVersionCheck) {
            juce::String versionCheckUrl = juce::String(LOCAL_DEV_SERVER_ADDRESS) + "/version-check.html";
            webView.goToURL(versionCheckUrl);
        } else {
            webView.goToURL(LOCAL_DEV_SERVER_ADDRESS);
        }
    }
    else
    {
#if !MIXCOMPARE_DEV_MODE
        webView.goToURL(juce::WebBrowserComponent::getResourceProviderRoot());
#else

        jassertfalse;
#endif
    }

    mc3::DiagnosticLog::log("editor: created, webview URL set");

#if !MIXCOMPARE_DEV_MODE
    // ウォッチドッグ: 一定時間内に WebView からリソース要求が一度も来なければ、
    // WebView（子プロセス）が起動できていない可能性が高い。ログへ記録し、真っ白のまま
    // 放置せずログの場所を画面に案内する。WebKit のコールドスタートは遅い環境で
    // 10 秒近くかかることがある（開発機でも実測 8 秒）ため、遅れてリソース要求が
    // 届いた場合は getResource() 側が案内を自動で取り下げる。
    juce::Timer::callAfterDelay(10000, [safe = juce::Component::SafePointer(this)]
    {
        mc3::DiagnosticLog::log("watchdog: timer fired (editor="
                                + juce::String(safe != nullptr ? "alive" : "gone") + ")");

        if (safe == nullptr || safe->isShuttingDown.load())
            return;

        if (!safe->webViewFirstResourceServed.load())
        {
            mc3::DiagnosticLog::log(
                "WATCHDOG: no webview resource request within 10s — the webview (child process) "
                "most likely failed to start. Check the webview child log and the dlopen probe "
                "results above.");
            safe->showWebViewStartupFailureNotice();
        }
    });
#endif
    
    // ウィンドウサイズ設定。設計初期サイズは 392×650（AAX のみ横 450）。
    const bool isAAX = juce::PluginHostType().isProTools()
                       && juce::PluginHostType::getPluginLoadedAs() == juce::AudioProcessor::WrapperType::wrapperType_AAX;
    const int initW = isAAX ? 450 : 392;
    const int initH = 650;

    // 編集サイズの永続化。ホストのウィンドウサイズ記憶はフォーマット/ホスト依存で不安定なため、
    //  APVTS state へ editorWidth/editorHeight を自前保存し、ここで強制復元してホスト・フォーマット
    //  非依存にする（TinyVU と同方針）。保存値は論理 px。範囲は min 392×610 / max 2560×1440。
    const auto apvtsState = audioProcessor.getState().state;
    restoredFromSavedSize = apvtsState.hasProperty("editorWidth") && apvtsState.hasProperty("editorHeight");
    const int savedW = static_cast<int>(apvtsState.getProperty("editorWidth",  initW));
    const int savedH = static_cast<int>(apvtsState.getProperty("editorHeight", initH));
    const int restoreW = juce::jlimit(392, 2560, savedW);
    const int restoreH = juce::jlimit(610, 1440, savedH);
    setSize(restoreW, restoreH);

    // リサイズ制約を設定
    resizerConstraints.setMinimumSize(392, 610);
    resizerConstraints.setMaximumSize(2560, 1440);

#if JUCE_LINUX || JUCE_BSD
    // Linux: Bitwig 等のホストはウィンドウ枠ドラッグをプラグインへ転送せず、
    //  枠を広げても黒余白が増えるだけ（onSize/guiSetSize に現在サイズの echo しか来ない）。
    //  そこで「ユーザーによる枠リサイズは不可」とホストへ申告し（canResize/guiCanResize=false）、
    //  リサイズは自前 WebUI ハンドル（setSize→host request_resize）経由のみ許可する。
    //  ※ setResizeLimits は min≠max のとき resizableByHost を true に戻してしまうため使わない。
    //    独自 constrainer を設定し、サイズ制限はそちら側で管理する。
    setConstrainer(&resizerConstraints);
    setResizable(false, false);
#else
    // Windows / macOS: ホストが枠リサイズを正しく転送するため従来どおり可変。
    setResizable(true, true);
    setResizeLimits(392, 610, 2560, 1440);
#endif
    
    // リサイズグリッパー（右下の角に表示される三角形のハンドル）を作成
    resizer = std::make_unique<juce::ResizableCornerComponent>(this, &resizerConstraints);
    addAndMakeVisible(resizer.get());
    resizer->setAlwaysOnTop(true);
    
    // グリッパーのサイズを設定
    const int gripperSize = 24;
    resizer->setBounds(getWidth() - gripperSize, getHeight() - gripperSize, gripperSize, gripperSize);
    resizer->toFront(true);  // 最前面に配置
    
    // リサイズボーダーの幅を広げる
    if (auto* resizeConstrainer = getConstrainer())
    {
        resizeConstrainer->setMinimumOnscreenAmounts(50, 50, 50, 50);
    }
    
    // メータリングモードの初期キャッシュ
    cachedMeteringMode = audioProcessor.getMeteringMode();

    // タイマー開始（メータリング/トランスポート/DPIウォッチ）
    startTimer(16); // ~60Hz (16ms)

    // Manager リスナー登録（UI送信の一元化）
    if (auto* sm = audioProcessor.getStateManager()) {
        sm->addListener(this);
    }
    if (auto* tm = audioProcessor.getTransportManager()) {
        tm->addListener(this);
    }
    if (auto* pm = audioProcessor.getPlaylistManager()) {
        pm->addListener(this);
    }

    // 一部ホスト（Pro Tools AAX, Cubase など）はコンストラクタ中の setSize を無視したり、
    //  独自保存サイズで最初の resized() を呼ぶため、次のメッセージループで復元サイズへ強制復帰させる。
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer(this), restoreW, restoreH]()
    {
        if (safe == nullptr) return;
        if (safe->getWidth() != restoreW || safe->getHeight() != restoreH)
            safe->setSize(restoreW, restoreH);
    });

    // 初期レイアウトを適用（グリッパーとWebViewの配置を正しく設定）
    resized();
}

MixCompare3AudioProcessorEditor::~MixCompare3AudioProcessorEditor()
{
    mc3::DiagnosticLog::log("editor: destroyed");

    // シャットダウンフラグを最初に立てる
    isShuttingDown.store(true);

    // タイマーを即座に停止
    stopTimer();

    // 保留中のリサイズ ack completion は呼ばずに破棄（破棄中の WebView へのコールバックを避ける）。
    //  JS 側は自前の安全タイムアウトで in-flight を解除するため未解決のままで問題ない。
    resizeAckPending = false;
    pendingResizeCompletion = {};
    
    // WebViewを先に停止・非表示にする（リソースプロバイダーの呼び出しを防ぐ）
    if (webViewLifetimeGuard.isConstructed())
    {
        // about:blank へ遷移してから stop しないと WebView2 が無効化後にエラーを返すため、先に遷移させる
#if !MIXCOMPARE_DEV_MODE
        webView.goToURL("about:blank");
#endif
        webView.stop();  // stop after navigating to blank to avoid WebView2 warnings
        webView.setVisible(false);
        
    }
    
    // WebViewの親コンポーネントから削除して明示的に破棄順序を制御
    removeChildComponent(&webView);
    
    // Manager リスナー解除
    if (auto* sm = audioProcessor.getStateManager()) {
        sm->removeListener(this);
    }
    if (auto* tm = audioProcessor.getTransportManager()) {
        tm->removeListener(this);
    }
    if (auto* pm = audioProcessor.getPlaylistManager()) {
        pm->removeListener(this);
    }
    
    // エラーマネージャーのコールバックをクリア
    MixCompare::ErrorManager::getInstance().clearErrorCallback();
}

// ===== State/Managers Listener implementations =====
void MixCompare3AudioProcessorEditor::stateChanged(const juce::ValueTree& newState)
{
    juce::ignoreUnused(newState);
}

void MixCompare3AudioProcessorEditor::playlistChanged()
{
    // どのスレッドから来てもUIスレッドへマーシャリング
    if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer(this)]
        {
            if (safe && !safe->isShuttingDown.load())
                safe->sendPlaylistUpdate();
        });
        return;
    }
    sendPlaylistUpdate();
}

void MixCompare3AudioProcessorEditor::transportStateChanged(MixCompare::TransportManager::TransportState newState)
{
    juce::ignoreUnused(newState);
    if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer(this)]
        {
            if (safe && !safe->isShuttingDown.load())
                safe->sendTransportStateUpdate();
        });
        return;
    }
    sendTransportStateUpdate();
}

void MixCompare3AudioProcessorEditor::transportPositionChanged(double newPosition)
{
    if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer(this), pos = newPosition]
        {
            if (safe && !safe->isShuttingDown.load())
                safe->sendTransportPositionUpdate(pos);
        });
        return;
    }
    sendTransportPositionUpdate(newPosition);
}

void MixCompare3AudioProcessorEditor::loopStateChanged(bool enabled, double start, double end)
{
    juce::ignoreUnused(enabled, start, end);
    if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer(this)]
        {
            if (safe && !safe->isShuttingDown.load())
                safe->sendTransportStateUpdate();
        });
        return;
    }
    sendTransportStateUpdate();
}

void MixCompare3AudioProcessorEditor::currentItemChanged(const juce::String& itemId)
{
    juce::ignoreUnused(itemId);
    // トラック切替は大きな状態変化なので一発でまとめて送る
    sendTrackChangeUpdate();
}

void MixCompare3AudioProcessorEditor::paint(juce::Graphics& g)
{
    // 初期白表示を抑制：エディタ背景を最終UIと同一色で即時塗りつぶし
    g.fillAll(juce::Colour(0xFF606F77));
    
    // グリッパー背景のみ描画（WebViewは全域に配置し、余白は作らない）
    const int gripperSize = 24;
    g.setColour(juce::Colour(0xFF606F77));
    g.fillRect(getWidth() - gripperSize, getHeight() - gripperSize, gripperSize, gripperSize);
    
    // リサイズグリッパーのドットパターンを描画
    if (resizer)
    {
        g.setColour(juce::Colour(0xFF5E5E5E));
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j)
            {
                if (i + j >= 2)  // 右下三角形のパターン
                {
                    int x = getWidth() - gripperSize + 4 + i * 4;
                    int y = getHeight() - gripperSize + 4 + j * 4;
                    g.fillEllipse(static_cast<float>(x), static_cast<float>(y), 2.0f, 2.0f);
                }
            }
        }
    }
}

void MixCompare3AudioProcessorEditor::resized()
{
    // WebViewをウィンドウ全域に配置（余白なし）
    const int gripperSize = 24;
    webView.setBounds(getLocalBounds());

    if (webViewFailureNotice != nullptr)
        webViewFailureNotice->setBounds(getLocalBounds());

    // リサイズグリッパーの位置を更新（最前面に）
    if (resizer)
    {
        resizer->setBounds(getWidth() - gripperSize,
                          getHeight() - gripperSize,
                          gripperSize,
                          gripperSize);
        resizer->toFront(true);  // 最前面に移動
    }

    // 編集サイズを APVTS state に保存し、次回オープン時にホスト保存値ではなくこの値で復元する。
    //  property 名は parameter ID と衝突しないため APVTS listener には影響しない。論理 px で保存。
    auto state = audioProcessor.getState().state;
    state.setProperty("editorWidth",  getWidth(),  nullptr);
    state.setProperty("editorHeight", getHeight(), nullptr);

#if JUCE_LINUX || JUCE_BSD
    // Linux 限定のリサイズ・バックプレッシャ（[[linux-dpi-resize-scaling]] 参照）。
    //  ホスト主導の resized()（= guiSetSize/onSize の echo）が着地したら、保留中の resizeTo を
    //  確定して JS に「往復完了」を通知する。自分の setSize 起因の resized()（resizeSelfDriven）は
    //  ホスト確定ではないので無視する。
    if (resizeAckPending && !resizeSelfDriven)
        resolveResizeAck();
#endif
}

void MixCompare3AudioProcessorEditor::resolveResizeAck()
{
    if (!resizeAckPending)
        return;
    resizeAckPending = false;
    auto completion = std::move(pendingResizeCompletion);
    pendingResizeCompletion = {};
    if (completion)
        completion(juce::var{ true });
}

void MixCompare3AudioProcessorEditor::applyDisplayScale()
{
#if JUCE_LINUX || JUCE_BSD
    //  ※ この transform 補正は Linux 専用。macOS(WKWebView)/Windows(WebView2) は OS/WebView が Retina・
    //    高DPI を native に処理するため transform 不要。macOS では getPlatformScaleFactor() が Retina でも
    //    1.0 を返す一方 devicePixelRatio は 2.0 のため、無条件適用すると s=2.0 で窓が倍に膨らむ（VST3/CLAP の
    //    実害）。Windows は getPlatformScaleFactor==devicePixelRatio で偶然 s=1.0 に収束するだけなので、
    //    将来の DPI 不一致による事故も含め Linux/BSD 以外では一切走らせない。
    //
    //  補正の目的は「WebView の CSS ビューポートを設計値へ一致させる」こと。WebView 物理px = 設計CSS × T ×
    //  peerScale、CSS ビューポート = 物理px / webViewDpr。一致解は wrapperType に依らず T = webViewDpr / peerScale。
    //  peerScale = getPlatformScaleFactor() は「OS/JUCE が既にウィンドウを何倍に物理拡大したか」の権威値
    //  （Linux は display->scale / globalScale）なので、この 1 式で「OS 拡大済み(peerScale==webViewDpr)→T=1.0」
    //  「OS 未拡大(peerScale=1.0,webViewDpr=2.0)→T=2.0」の双方が成立し二重拡大も自動回避される。
    //  かつて Standalone を setTransform({}) で除外していたが、KDE/Wayland(XWayland) では JUCE の display->scale が
    //  gsettings(scaling-factor=1) を拾って 1.0 になる一方 WebKitGTK は GDK スケール 2 で webViewDpr=2.0 のため、
    //  transform 無しだと窓が小さく CSS ビューポートが潰れてレイアウトが崩れる。よって Standalone も同式を適用する
    //  （StandaloneFilterWindow の getSizeToContainEditor が editor->getTransform() を見て窓サイズを追従させる）。
    double peerScale = 1.0;
    if (auto* p = getPeer())
    {
        const double ps = p->getPlatformScaleFactor();
        if (ps > 0.0)
            peerScale = ps;
    }
    const float s = (lastWebViewDpr > 0.0) ? (float) (lastWebViewDpr / peerScale) : 1.0f;
    setTransform(juce::AffineTransform::scale(s));

    // transform を遅延/再適用すると editor の resized() が自動発火せず WebView ネイティブ子窓が取り残される
    //  （灰色余白）。settle 再同期ジグル（2-tick の 1px 揺らし）を再武装し、新 transform 下で
    //  webView.setBounds と guiRequestResize を再発火させて窓・editor・WebView子窓を収束させる。
    settleReconcileDone = false;
    lastResizeActivityMs = juce::Time::getMillisecondCounter();
#endif
}

void MixCompare3AudioProcessorEditor::setScaleFactor(float /*newHostScale*/)
{
    // ホスト(VST3 setContentScaleFactor / CLAP guiSetScale)が宣言する scale は誤判定するため使わず、
    //  applyDisplayScale が webViewDpr/peerScale で正しい transform を（再）適用する。
    applyDisplayScale();
}

void MixCompare3AudioProcessorEditor::timerCallback()
{
#if JUCE_LINUX || JUCE_BSD
    // Linux 限定のリサイズ・バックプレッシャ／再同期（[[linux-dpi-resize-scaling]] 参照）。
    //  Bitwig 等は枠ドラッグをプラグインへ転送しないため枠リサイズは無効化し（setResizable(false)）、
    //  自前ハンドルのみ許可している。ハンドルの高頻度リサイズで取り残された黒残り/見切れを、
    //  ホストの echo 待ち（バックプレッシャ）と落ち着き後の強制再同期で収束させる。
    // ack 安全タイムアウト: ホストが echo を返さない場合でも保留 completion を必ず解決し、
    //  JS のバックプレッシャがフリーズしないようにする（~45ms = 最低 ~22fps を保証）。
    if (resizeAckPending
        && (juce::Time::getMillisecondCounter() - resizeAckStartMs) > 45)
        resolveResizeAck();
#endif

    // 終了中・モーダルダイアログ表示中は送出を停止（OpenPanel 等の操作性確保）
    if (isShuttingDown.load() || activeModalDialogs.load(std::memory_order_acquire) > 0)
        return;

#if JUCE_LINUX || JUCE_BSD
    // リサイズ落ち着き後の強制再同期（2 tick に分割した 1px ジグル）。
    //  editor が既に最終サイズだと resized() が発火せず、ホストのコンテナ窓が中間サイズで
    //  取り残されても再同期されない。1px だけ変えて戻すことで guiRequestResize と
    //  webView.setBounds を確実に再発火させ収束させる。ジグルを 2 tick に分けて間に ~16ms
    //  空けるのは、同期連続 setBounds が WebKitGTK の描画を固める不具合を避けるため。
    //  step2: 前 tick で 1px 縮めた分を元へ戻す。
    if (resyncStep2Pending)
    {
        resyncStep2Pending = false;
        const juce::ScopedValueSetter<bool> selfDriven(resizeSelfDriven, true);
        setSize(resyncTargetW, resyncTargetH);
    }
    //  step1: 直近のリサイズから ~120ms アイドルになったら 1px 縮めて再同期を開始。
    else if (!settleReconcileDone
        && !resizeAckPending
        && isVisible()
        && (juce::Time::getMillisecondCounter() - lastResizeActivityMs) > 120)
    {
        settleReconcileDone = true;
        resyncTargetW = getWidth();
        resyncTargetH = getHeight();
        resyncStep2Pending = true;
        const juce::ScopedValueSetter<bool> selfDriven(resizeSelfDriven, true);
        setSize(resyncTargetW, juce::jmax(1, resyncTargetH - 1));
    }
#endif

#if defined(JUCE_WINDOWS)
    // 各フレームで HWND ベースの DPI をポーリングし、変化を検出
    pollAndMaybeNotifyDpiChange();
#endif

    // ウィンドウが非表示の場合はスキップ
    if (!isVisible())
        return;
    
    // チュートリアル準拠の簡易イベント（フロントが fetch で JSON を取得）
    // - WebUI 側で Plotly などのデモ用途に利用可能
    webView.emitEventIfBrowserIsVisible("outputLevel", juce::var{});

    // 初期パラメータ値の送信はWebUIからのリクエストに応じて行う（下記のsendInitialParametersメソッド参照）
    
    // メータリング情報送信（メータリングは常に送信）
    {
        juce::DynamicObject::Ptr meterData{new juce::DynamicObject{}};

        // メータリングモードは毎フレーム APVTS から直接取得し、
        // DAW オートメーション再生時の変更を即時に反映させる。
        // （キャッシュは初期表示や明示リセット時のみに利用）
        const int meteringMode = audioProcessor.getMeteringMode();
        const bool isPeakMode = (meteringMode == 0);
        const bool isMomentaryMode = (meteringMode == 2);
        meterData->setProperty("meteringMode", meteringMode);
        
        // HOST側メーター
        juce::DynamicObject::Ptr hostMeter{new juce::DynamicObject{}};
        
        // Use memory_order_acquire for proper synchronization
        const float hostLeft = audioProcessor.hostLevelLeft.load(std::memory_order_acquire);
        const float hostRight = audioProcessor.hostLevelRight.load(std::memory_order_acquire);
        const float playlistLeft = audioProcessor.playlistLevelLeft.load(std::memory_order_acquire);
        const float playlistRight = audioProcessor.playlistLevelRight.load(std::memory_order_acquire);
        
        if (isMomentaryMode)
        {
            // Momentaryモード: LKFS値を送信（単一値）
            if (auto* stateManager = audioProcessor.getStateManager())
            {
                if (auto* meteringService = stateManager->getMeteringService())
                {
                    // Host
                    float hostLKFS = meteringService->getMomentaryLKFS(MixCompare::MeteringService::MeterSource::Host);
                    float hostHoldLKFS = meteringService->getMomentaryHoldLKFS(MixCompare::MeteringService::MeterSource::Host);
                    hostMeter->setProperty("momentary", hostLKFS);
                    hostMeter->setProperty("momentaryHold", hostHoldLKFS);
                    
                    // Playlist
                    juce::DynamicObject::Ptr playlistMeter{new juce::DynamicObject{}};
                    float playlistLKFS = meteringService->getMomentaryLKFS(MixCompare::MeteringService::MeterSource::Playlist);
                    float playlistHoldLKFS = meteringService->getMomentaryHoldLKFS(MixCompare::MeteringService::MeterSource::Playlist);
                    playlistMeter->setProperty("momentary", playlistLKFS);
                    playlistMeter->setProperty("momentaryHold", playlistHoldLKFS);
                    meterData->setProperty("playlist", playlistMeter.get());
                    
                    // デバッグ出力（頻度を制限）
                    static int momentaryDebugCounter = 0;
                    if (++momentaryDebugCounter >= 60) // 1秒に1回
                    {
                        momentaryDebugCounter = 0;
                        
                    }
                    
                    // Output
                    float outputLKFS = meteringService->getMomentaryLKFS(MixCompare::MeteringService::MeterSource::Output);
                    float outputHoldLKFS = meteringService->getMomentaryHoldLKFS(MixCompare::MeteringService::MeterSource::Output);
                    meterData->setProperty("momentary", outputLKFS);
                    meterData->setProperty("momentaryHold", outputHoldLKFS);
                }
            }
        }
        else if (isPeakMode)
        {
            // ピークモード: TruePeak値を両方に送信
            const float tpL = audioProcessor.getHostTruePeak(0);
            const float tpR = audioProcessor.getHostTruePeak(1);
            const double tpLdB = juce::Decibels::gainToDecibels(tpL, -60.0f);
            const double tpRdB = juce::Decibels::gainToDecibels(tpR, -60.0f);
            hostMeter->setProperty("left", tpLdB);  // メーターバー用
            hostMeter->setProperty("right", tpRdB);
            hostMeter->setProperty("truePeakLeft", tpLdB);  // ラベル用
            hostMeter->setProperty("truePeakRight", tpRdB);
        }
        else
        {
            // RMSモード: MeteringService から取得して dB へ変換して送信
            double rmsLdB = -60.0;
            double rmsRdB = -60.0;
            if (auto* ms = audioProcessor.getMeteringService())
            {
                const auto values = ms->getMeterValues(MixCompare::MeteringService::MeterSource::Host);
                rmsLdB = juce::Decibels::gainToDecibels(values.rmsLeft, -60.0f);
                rmsRdB = juce::Decibels::gainToDecibels(values.rmsRight, -60.0f);
            }
            else
            {
                rmsLdB = hostLeft;
                rmsRdB = hostRight;
            }
            hostMeter->setProperty("rmsLeft", rmsLdB);
            hostMeter->setProperty("rmsRight", rmsRdB);
        }
        meterData->setProperty("host", hostMeter.get());
        
        // PLAYLIST側メーター（Momentaryモード以外）
        if (!isMomentaryMode)
        {
            juce::DynamicObject::Ptr playlistMeter{new juce::DynamicObject{}};
            if (isPeakMode)
        {
            // ピークモード: TruePeak値を両方に送信
            const float pTpL = audioProcessor.getPlaylistTruePeak(0);
            const float pTpR = audioProcessor.getPlaylistTruePeak(1);
            const double pTpLdB = juce::Decibels::gainToDecibels(pTpL, -60.0f);
            const double pTpRdB = juce::Decibels::gainToDecibels(pTpR, -60.0f);
            playlistMeter->setProperty("left", pTpLdB);  // メーターバー用
            playlistMeter->setProperty("right", pTpRdB);
            playlistMeter->setProperty("truePeakLeft", pTpLdB);  // ラベル用
            playlistMeter->setProperty("truePeakRight", pTpRdB);
        }
            else
        {
                // RMSモード: MeteringService から取得
                double rmsLdB = -60.0;
                double rmsRdB = -60.0;
                if (auto* ms = audioProcessor.getMeteringService())
                {
                    const auto values = ms->getMeterValues(MixCompare::MeteringService::MeterSource::Playlist);
                    rmsLdB = juce::Decibels::gainToDecibels(values.rmsLeft, -60.0f);
                    rmsRdB = juce::Decibels::gainToDecibels(values.rmsRight, -60.0f);
                }
                else
                {
                    rmsLdB = playlistLeft;
                    rmsRdB = playlistRight;
                }
                playlistMeter->setProperty("rmsLeft", rmsLdB);
                playlistMeter->setProperty("rmsRight", rmsRdB);
        }
            meterData->setProperty("playlist", playlistMeter.get());
        }
        
        // 出力メーター（Momentary以外は RMS を MeteringService(Output) から取得して dB 化）
        if (!isMomentaryMode)
        {
            double outLdB = -60.0;
            double outRdB = -60.0;
            if (auto* ms = audioProcessor.getMeteringService())
            {
                const auto outValues = ms->getMeterValues(MixCompare::MeteringService::MeterSource::Output);
                outLdB = juce::Decibels::gainToDecibels(outValues.rmsLeft, -60.0f);
                outRdB = juce::Decibels::gainToDecibels(outValues.rmsRight, -60.0f);
            }
            else
            {
                outLdB = hostLeft;
                outRdB = hostRight;
            }
            meterData->setProperty("left", outLdB);
            meterData->setProperty("right", outRdB);
        }
        
        // Debug log removed for clean build
        
        // メータリング更新は常に送信（タイマー自体が60Hzなので追加のスロットリングは不要）
        webView.emitEventIfBrowserIsVisible("meterUpdate", meterData.get());
    }
    
    // トランスポート状態送信（変更があった場合のみ）
    const auto* tm = audioProcessor.getTransportManager();
    const bool curIsPlaying = tm ? tm->isPlaying() : false;
    const bool curLoopEnabled = tm ? tm->isLoopEnabled() : false;
    bool transportChanged = false;
    
    if (lastTransportIsPlaying != curIsPlaying ||
        lastTransportLoopEnabled != curLoopEnabled)
    {
        transportChanged = true;
        lastTransportIsPlaying = curIsPlaying;
        lastTransportLoopEnabled = curLoopEnabled;
    }
    
    if (transportChanged)
    {
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        // 送信ごとにトランスポートのリビジョンを単調増加
        const auto trev = transportRevision.fetch_add(1, std::memory_order_relaxed) + 1;
        juce::DynamicObject::Ptr transportData{new juce::DynamicObject{}};
        transportData->setProperty("isPlaying", curIsPlaying);
        // transportUpdate では position を送らない（位置は transportPositionUpdate のみ）
        if (tm)
        {
            transportData->setProperty("loopStart", tm->getLoopStart());
            transportData->setProperty("loopEnd", tm->getLoopEnd());
            transportData->setProperty("loopEnabled", tm->isLoopEnabled());
        }
        else
        {
            transportData->setProperty("loopStart", 0.0);
            transportData->setProperty("loopEnd", 0.0);
            transportData->setProperty("loopEnabled", false);
        }
        
        // Add current track duration and index for UI synchronization
        double currentDuration = 0.0;
        auto playlist = audioProcessor.getPlaylist();
        int currentIndex = audioProcessor.getCurrentPlaylistIndex();
        if (currentIndex >= 0 && currentIndex < static_cast<int>(playlist.size()))
        {
            // currentIndex は int、vector の index は size_t なので明示キャストで符号変換警告を抑止
            currentDuration = playlist[static_cast<size_t>(currentIndex)].duration;
        }
        transportData->setProperty("duration", currentDuration);
        transportData->setProperty("currentIndex", currentIndex);
        
        // 現在のシーケンス番号を取得して送信
        int currentSeq = transportSequenceNumber.load();
        transportData->setProperty("sequenceNumber", currentSeq);
        lastSentSequenceNumber = currentSeq;
        // リビジョン番号を付加（古い更新の破棄用）
        transportData->setProperty("revision", static_cast<double>(trev));
        transportData->setProperty("sessionId", static_cast<double>(transportSessionId.load()));
        transportData->setProperty("positionEpoch", static_cast<double>(audioProcessor.getPositionEpoch()));
        
        // 送出頻度を 30Hz 程度に制限（連続シーク等でのスパム防止）
        static double lastSentMs = 0.0;
        if (nowMs - lastSentMs > 33.0)
        {
            lastSentMs = nowMs;
            webView.emitEventIfBrowserIsVisible("transportUpdate", transportData.get());
        }
    }
}

#if defined(JUCE_WINDOWS)
// HWND ベースの DPI をポーリングし、変化時に WebUI へ通知 + レイアウト再確定を促す
void MixCompare3AudioProcessorEditor::pollAndMaybeNotifyDpiChange()
{
    if (auto* peer = getPeer())
    {
        HWND hwnd = (HWND) peer->getNativeHandle();
        int dpi = 0; double scale = 1.0;
        queryWindowDpi(hwnd, dpi, scale);

        if (dpi > 0)
        {
            const bool scaleChanged = std::abs(lastHwndScaleFactor - scale) >= 0.01;
            const bool dpiChanged   = lastHwndDpi != dpi;
            if (scaleChanged || dpiChanged)
            {
                lastHwndScaleFactor = scale;
                lastHwndDpi = dpi;
                

                juce::DynamicObject::Ptr payload{ new juce::DynamicObject{} };
                payload->setProperty("scale", scale);
                payload->setProperty("dpi", dpi);
                webView.emitEventIfBrowserIsVisible("dpiScaleChanged", payload.get());

                const int w = getWidth();
                const int h = getHeight();
                setSize(w + 1, h + 1);
                setSize(w, h);
            }
        }
    }
}
#endif

void MixCompare3AudioProcessorEditor::sendPlaylistUpdate()
{
    // シャットダウン中は送信を行わない（WebView2 破棄との競合回避）
    if (isShuttingDown.load())
        return;

    // リビジョンを単調増加させてから送信（重複・遅延更新の排除に有効）
    const auto rev = playlistRevision.fetch_add(1, std::memory_order_relaxed) + 1;
    juce::DynamicObject::Ptr playlistData{new juce::DynamicObject{}};
    juce::Array<juce::var> items;
    
    
    for (const auto& item : audioProcessor.getPlaylist())
    {
        juce::DynamicObject::Ptr itemObj{new juce::DynamicObject{}};
        itemObj->setProperty("id", item.id);
        itemObj->setProperty("name", item.name);
        itemObj->setProperty("duration", item.duration);
        itemObj->setProperty("isLoaded", item.isLoaded);
        itemObj->setProperty("file", item.file.getFullPathName());
        // フロントエンドで欠落ファイルを識別するための存在フラグを付与
        // ファイルの存在チェックはメインスレッド側（エディタ）で軽量に実行
        itemObj->setProperty("exists", item.file.existsAsFile());
        items.add(itemObj.get());
    }
    
    playlistData->setProperty("items", items);
    playlistData->setProperty("currentIndex", audioProcessor.getCurrentPlaylistIndex());
    // プレイリスト更新のリビジョン番号を付加
    playlistData->setProperty("revision", static_cast<double>(rev));
    
    webView.emitEventIfBrowserIsVisible("playlistUpdate", playlistData.get());
}

void MixCompare3AudioProcessorEditor::sendTransportStateUpdate()
{
    // シャットダウン中は送信を行わない
    if (isShuttingDown.load())
        return;

    // 明示送信用（選曲や明示アクション直後に使う）
    juce::DynamicObject::Ptr transportData{new juce::DynamicObject{}};
    // TransportManager を真実源として取得（宣言的・単一路線）
    const auto* tm = audioProcessor.getTransportManager();
    const bool isPlaying = tm ? tm->isPlaying() : false;
    const double position = tm ? tm->getPosition() : 0.0;
    const bool loopEnabled = tm ? tm->isLoopEnabled() : false;
    const double loopStart = tm ? tm->getLoopStart() : 0.0;
    const double loopEnd = tm ? tm->getLoopEnd() : 0.0;

    transportData->setProperty("isPlaying", isPlaying);
    transportData->setProperty("position", position);
    transportData->setProperty("loopStart", loopStart);
    transportData->setProperty("loopEnd", loopEnd);
    transportData->setProperty("loopEnabled", loopEnabled);
    
    // Add current track duration for proper UI synchronization
    double currentDuration = 0.0;
    auto playlist = audioProcessor.getPlaylist();
    int currentIndex = audioProcessor.getCurrentPlaylistIndex();
    if (currentIndex >= 0 && currentIndex < static_cast<int>(playlist.size()))
    {
        currentDuration = playlist[static_cast<size_t>(currentIndex)].duration;
    }
    transportData->setProperty("duration", currentDuration);
    transportData->setProperty("currentIndex", currentIndex);
    
    // 明示送信ではシーケンス番号をインクリメントして古い更新を確実に無効化
    int currentSeq = ++transportSequenceNumber;
    transportData->setProperty("sequenceNumber", currentSeq);
    const auto trev = transportRevision.fetch_add(1, std::memory_order_relaxed) + 1;
    transportData->setProperty("revision", static_cast<double>(trev));
    webView.emitEventIfBrowserIsVisible("transportUpdate", transportData.get());
}

void MixCompare3AudioProcessorEditor::sendTransportPositionUpdate(double positionInSeconds)
{
    // シャットダウン中は送信を行わない
    if (isShuttingDown.load())
        return;

    // 再生位置のみの軽量な更新
    juce::DynamicObject::Ptr positionData{new juce::DynamicObject{}};
    positionData->setProperty("position", positionInSeconds);
    // TransportManager から isPlaying を取得
    if (const auto* tm = audioProcessor.getTransportManager())
        positionData->setProperty("isPlaying", tm->isPlaying());
    else
        positionData->setProperty("isPlaying", false);
    
    // Add current track duration for proper progress bar calculation
    double currentDuration = 0.0;
    auto playlist = audioProcessor.getPlaylist();
    int currentIndex = audioProcessor.getCurrentPlaylistIndex();
    if (currentIndex >= 0 && currentIndex < static_cast<int>(playlist.size()))
    {
        currentDuration = playlist[static_cast<size_t>(currentIndex)].duration;
    }
    positionData->setProperty("duration", currentDuration);
    // セッションIDを付加して古い更新をUI側で除外可能に
    positionData->setProperty("sessionId", static_cast<double>(transportSessionId.load()));
    // positionEpoch を付与：古い seek 前の遅延更新をUI側で破棄可能に
    positionData->setProperty("positionEpoch", static_cast<double>(audioProcessor.getPositionEpoch()));
    
    webView.emitEventIfBrowserIsVisible("transportPositionUpdate", positionData.get());
}

void MixCompare3AudioProcessorEditor::sendTrackChangeUpdate()
{
    // シャットダウン中は送信を行わない
    if (isShuttingDown.load())
        return;

    // Combined update for track switching to prevent UI flickering
    // セッションIDを進め、古いtransportUpdate/PositionUpdateをUI側で無視できるようにする
    transportSessionId = ++transportSessionIdGenerator;
    juce::DynamicObject::Ptr trackChangeData{new juce::DynamicObject{}};
    
    // Add playlist info
    juce::Array<juce::var> items;
    for (const auto& item : audioProcessor.getPlaylist())
    {
        juce::DynamicObject::Ptr itemObj{new juce::DynamicObject{}};
        itemObj->setProperty("id", item.id);
        itemObj->setProperty("name", item.name);
        itemObj->setProperty("duration", item.duration);
        itemObj->setProperty("isLoaded", item.isLoaded);
        itemObj->setProperty("file", item.file.getFullPathName());
        itemObj->setProperty("exists", item.file.existsAsFile());
        items.add(itemObj.get());
    }
    
    int currentIndex = audioProcessor.getCurrentPlaylistIndex();
    trackChangeData->setProperty("items", items);
    trackChangeData->setProperty("currentIndex", currentIndex);
    
    // Add transport state
    // 選曲直後の一瞬のチラつきを避けるため、position は常に 0 を送る。
    // ループ範囲は初期化（0..duration）を即時反映させる。
    bool isPlaying = false;
    bool loopEnabled = false;
    if (const auto* tm = audioProcessor.getTransportManager())
    {
        isPlaying = tm->isPlaying();
        loopEnabled = tm->isLoopEnabled();
    }
    trackChangeData->setProperty("isPlaying", isPlaying);
    trackChangeData->setProperty("position", 0.0);
    trackChangeData->setProperty("loopStart", 0.0);
    trackChangeData->setProperty("loopEnabled", loopEnabled);
    
    // Add duration for current track
    double currentDuration = 0.0;
    auto playlist = audioProcessor.getPlaylist();
    if (currentIndex >= 0 && currentIndex < static_cast<int>(playlist.size()))
    {
        currentDuration = playlist[static_cast<size_t>(currentIndex)].duration;
    }
    trackChangeData->setProperty("duration", currentDuration);
    trackChangeData->setProperty("loopEnd", currentDuration);
    
    // Add revision numbers
    const auto playlistRev = playlistRevision.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto transportRev = transportRevision.fetch_add(1, std::memory_order_relaxed) + 1;
    trackChangeData->setProperty("playlistRevision", static_cast<double>(playlistRev));
    trackChangeData->setProperty("transportRevision", static_cast<double>(transportRev));
    trackChangeData->setProperty("sessionId", static_cast<double>(transportSessionId.load()));
    
    // Send as single combined message
    webView.emitEventIfBrowserIsVisible("trackChange", trackChangeData.get());

    // 抑止窓は廃止（sessionIdベースでフロント側が破棄）
}

std::optional<MixCompare3AudioProcessorEditor::Resource> 
MixCompare3AudioProcessorEditor::getResource(const juce::String& url) const
{
    // 最初のリソース要求 = WebView 子プロセスが起動しコンテンツを要求してきた証拠
    if (!webViewFirstResourceServed.exchange(true))
    {
        mc3::DiagnosticLog::log("webview: first resource request received (" + url + ")");

        // ウォッチドッグ発動後に届いた場合（= 単に起動が遅かった）、失敗案内を自動で消す。
        // 通知 Label は WebView を覆っているため、放置すると「動いているのに失敗に見える」。
        juce::MessageManager::callAsync(
            [safe = juce::Component::SafePointer(const_cast<MixCompare3AudioProcessorEditor*>(this))]
            {
                if (safe != nullptr && safe->webViewFailureNotice != nullptr)
                {
                    mc3::DiagnosticLog::log(
                        "webview: resources arrived AFTER the watchdog — startup was just slow; "
                        "dismissing the failure notice");
                    safe->webViewFailureNotice.reset();
                }
            });
    }

    const auto resourceToRetrieve =
        url == "/" ? "index.html" : url.fromFirstOccurrenceOf("/", false, false);
    
    
    // チュートリアル準拠の簡易 JSON エンドポイント
    if (resourceToRetrieve == "outputLevel.json")
    {
        juce::DynamicObject::Ptr levelData{ new juce::DynamicObject{} };
        // 出力は Processor 側で常時計測している RMS を dB 化したものを利用
        levelData->setProperty("left", audioProcessor.outputLevelLeft.load());
        levelData->setProperty("right", audioProcessor.outputLevelRight.load());
        const auto jsonString = juce::JSON::toString(levelData.get());
        juce::MemoryInputStream stream{ jsonString.getCharPointer(), jsonString.getNumBytesAsUTF8(), false };
        return Resource{ streamToVector(stream), juce::String{"application/json"} };
    }

#if !MIXCOMPARE_DEV_MODE && __has_include(<WebViewFiles.h>)
    // プロダクションビルド: ZIPファイルから読み込み
    const auto resource = getWebViewFileAsBytes(resourceToRetrieve);
    if (!resource.empty()) {
        const auto extension =
            resourceToRetrieve.fromLastOccurrenceOf(".", false, false);
        return Resource{std::move(resource), getMimeForExtension(extension)};
    }
    
#endif
    
    // デバッグビルドまたはリソースが見つからない場合
    return std::nullopt;
}

void MixCompare3AudioProcessorEditor::handlePlaylistAction(
    const juce::Array<juce::var>& args,
    juce::WebBrowserComponent::NativeFunctionCompletion completion)
{
    if (args.size() < 1)
    {
        completion(juce::var{false});
        return;
    }
    
    juce::String action = args[0].toString();

    const bool actionShowsModal = (action == "add" || action == "export" || action == "import");
    if (actionShowsModal && activeModalDialogs.load(std::memory_order_acquire) > 0)
    {
        // すでにネイティブダイアログを開いている最中は新しいダイアログを開かない
        completion(juce::var{false});
        return;
    }

    struct ModalDialogGuard
    {
        explicit ModalDialogGuard(MixCompare3AudioProcessorEditor* ownerIn) : owner(ownerIn)
        {
            if (owner != nullptr)
                owner->activeModalDialogs.fetch_add(1, std::memory_order_acq_rel);
        }

        ~ModalDialogGuard()
        {
            if (owner != nullptr)
                owner->activeModalDialogs.fetch_sub(1, std::memory_order_acq_rel);
        }

    private:
        MixCompare3AudioProcessorEditor* owner;
    };
    
    if (action == "add")
    {
        // ファイル選択ダイアログ（非同期版使用）- 英語タイトル、複数選択対応
        // Media Foundation経由でWindowsでもM4A/AACをサポート
        juce::String supportedFormats = "*.wav;*.aif;*.aiff;*.mp3;*.flac;*.ogg;*.m4a;*.aac;*.ape";
        
        auto fileChooser = std::make_shared<juce::FileChooser>("Select Audio Files", 
                                                               juce::File{}, 
                                                               supportedFormats);
        auto modalGuard = std::make_shared<ModalDialogGuard>(this);
        
        // macOS のネイティブダイアログでは canSelectFiles を明示しないと
        // ファイルが選択不可（グレーアウト）になるため、フラグを追加
        fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::canSelectMultipleItems,
            [safe = juce::Component::SafePointer(this), completion, fileChooser, modalGuard](const juce::FileChooser& chooser)
            {
                if (!safe || safe->isShuttingDown.load()) { completion(juce::var{false}); return; }
                auto files = chooser.getResults();
                if (!files.isEmpty())
                {
                    safe->audioProcessor.addFilesToPlaylist(files);
                    // 即時更新を送信（sendPlaylistUpdate内でrevisionを進める）
                    safe->sendPlaylistUpdate();
                    completion(juce::var{true});
                }
                else
                {
                    completion(juce::var{false});
                }
            });
    }
    else if (action == "remove" && args.size() > 1)
    {
        audioProcessor.removeFromPlaylist(args[1].toString());
        // UI更新を即座に送信
        sendPlaylistUpdate();
        completion(juce::var{true});
    }
    else if (action == "reorder" && args.size() > 2)
    {
        audioProcessor.reorderPlaylist(args[1], args[2]);
        // UI更新を即座に送信
        sendPlaylistUpdate();
        completion(juce::var{true});
    }
    else if (action == "select" && args.size() > 1)
    {
        // APVTS 直結: PLAYLIST_CURRENT_INDEX_NORM を設定
        const int targetIndex = juce::roundToInt((double)args[1]);
        const auto items = audioProcessor.getPlaylist();
        const int num = static_cast<int>(items.size());
        if (num > 0)
        {
            const int clamped = juce::jlimit(0, num - 1, targetIndex);
            const float norm = (num > 1) ? static_cast<float>(clamped) / static_cast<float>(num - 1) : 0.0f;
            if (auto* p = audioProcessor.getState().getParameter(mc3::id::PLAYLIST_CURRENT_INDEX_NORM.getParamID()))
                p->setValueNotifyingHost(norm);
            completion(juce::var{true});
        }
        else
        {
            completion(juce::var{false});
        }
    }
    else if (action == "clear")
    {
        audioProcessor.clearPlaylist();
        sendPlaylistUpdate();
        completion(juce::var{true});
    }
    else if (action == "export")
    {
        // M3U8形式でエクスポート
        auto fileChooser = std::make_shared<juce::FileChooser>("Export Playlist as M3U8",
                                                               juce::File{},
                                                               "*.m3u8");
        auto modalGuard = std::make_shared<ModalDialogGuard>(this);
        
        fileChooser->launchAsync(juce::FileBrowserComponent::saveMode
                                 | juce::FileBrowserComponent::canSelectFiles
                                 | juce::FileBrowserComponent::warnAboutOverwriting,
            [safe = juce::Component::SafePointer(this), completion, fileChooser, modalGuard](const juce::FileChooser& chooser)
            {
                if (!safe || safe->isShuttingDown.load()) { completion(juce::var{false}); return; }
                auto file = chooser.getResult();
                if (file != juce::File{})
                {
                    safe->audioProcessor.exportPlaylistToM3U8(file);
                    completion(juce::var{true});
                }
                else
                {
                    completion(juce::var{false});
                }
            });
    }
    else if (action == "import")
    {
        // M3U8形式でインポート
        auto fileChooser = std::make_shared<juce::FileChooser>("Import Playlist from M3U8",
                                                               juce::File{},
                                                               "*.m3u8;*.m3u");
        auto modalGuard = std::make_shared<ModalDialogGuard>(this);
        
        fileChooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
            [safe = juce::Component::SafePointer(this), completion, fileChooser, modalGuard](const juce::FileChooser& chooser)
            {
                if (!safe || safe->isShuttingDown.load()) { completion(juce::var{false}); return; }
                auto file = chooser.getResult();
                if (file.existsAsFile())
                {
                    safe->audioProcessor.importPlaylistFromM3U8(file);
                    safe->sendPlaylistUpdate();
                    completion(juce::var{true});
                }
                else
                {
                    completion(juce::var{false});
                }
            });
    }
    else
    {
        completion(juce::var{false});
    }
}

// 旧: handleTransportAction は APVTS 直結へ移行済みのため削除

// WebUI の設計サイズ（すべて WebView の CSS ピクセル基準）。
//  実際の論理 px / 物理 px へは apply_layout / resizeBegin で確定する ratio を掛けて換算する。
namespace {
    constexpr int kDesignInitW = 392;
    constexpr int kDesignInitH = 650;
    constexpr int kDesignMinW  = 392;
    constexpr int kDesignMinH  = 610;
    constexpr int kDesignMaxW  = 2560;
    constexpr int kDesignMaxH  = 1440;
}

void MixCompare3AudioProcessorEditor::handleWindowAction(
    const juce::Array<juce::var>& args,
    juce::WebBrowserComponent::NativeFunctionCompletion completion)
{
    if (args.size() < 1)
    {
        completion(juce::var{ false });
        return;
    }

    const juce::String action = args[0].toString();

    if (action == "resizeBegin" && args.size() >= 3)
    {
        // ドラッグ開始時、サイズが安定している瞬間に CSS px → 論理 px の換算比率を 1 回だけ確定。
        //  ratio = getWidth()/innerWidth は数学的に「論理px / CSS px」(≒ devicePixelRatio/peerScale)
        //  に一致する。確定後は次の resizeBegin まで固定で使う（毎フレーム再計算すると発散するため）。
        const double cssW = (double)args[1];
        const double cssH = (double)args[2];
        webResizeRatioW = (cssW > 0.0) ? (double)getWidth()  / cssW : 1.0;
        webResizeRatioH = (cssH > 0.0) ? (double)getHeight() / cssH : 1.0;
        completion(juce::var{ true });
        return;
    }

    if (action == "apply_layout" && args.size() >= 3)
    {
        // WebUI 読み込み完了時に、現在の innerWidth/innerHeight(CSS px) を受け取り、
        //  ratio = getWidth()/innerWidth（=「論理px / CSS px」≒ devicePixelRatio / ホスト総スケール）を確定。
        //  これは WebView が拾う「真の表示倍率」を反映する。ホストが分数スケーリングを誤判定して
        //  間違った総スケールでウィンドウを作っても、ratio 経由で設計 CSS 値どおりの見た目に合わせられる。
        //  設計値（CSS px）: 初期 392x650 / 最小 392x610 / 最大 2560x1440。
        const double cssW = (double)args[1];
        const double cssH = (double)args[2];
        webResizeRatioW = (cssW > 0.0) ? (double)getWidth()  / cssW : 1.0;
        webResizeRatioH = (cssH > 0.0) ? (double)getHeight() / cssH : 1.0;

        // 真のディスプレイ倍率 webViewDpr(= args[3] の devicePixelRatio)を確定し、
        //  applyDisplayScale で Linux 埋め込み時のウィンドウ物理サイズを補正する。
        //  初期サイズ・リサイズ上限はコンストラクタで設計px直値として確定済みのため、ここでは触れない
        //  （旧 ratio 換算での上書き／globalScale 実測＋ディスクキャッシュ方式はプロセスグローバル汚染で
        //   破綻したため完全撤去。視覚スケールは applyDisplayScale の transform に一元化）。
        lastWebViewDpr = (args.size() >= 4) ? (double)args[3] : -1.0;
        applyDisplayScale();
        completion(juce::var{ true });
        return;
    }

    if (action == "resizeTo" && args.size() >= 3)
    {
        // WebUI から来る w,h は WebView の CSS ピクセル。先に CSS 空間で最小/最大にクランプし、
        //  そのあと固定比率(resizeBegin / apply_layout で確定)を掛けて論理 px へ換算する。
        //  （CSS でクランプしないと ratio≠1 の環境で最小値が論理px基準にずれる）。
        const double cssW = juce::jlimit<double>(kDesignMinW, kDesignMaxW, (double)args[1]);
        const double cssH = juce::jlimit<double>(kDesignMinH, kDesignMaxH, (double)args[2]);
        const int targetW = juce::roundToInt(cssW * webResizeRatioW);
        const int targetH = juce::roundToInt(cssH * webResizeRatioH);

#if JUCE_LINUX || JUCE_BSD
        // Linux 限定の「真のバックプレッシャ」: completion を即返さず、ホストが実際に
        //  リサイズし終える（resized() が再発火する）まで保留する。これにより JS は往復1件ずつ
        //  送るようになり、高頻度送信でホストがリクエストを取りこぼす齟齬を防ぐ。
        //  Windows / macOS はホストのリサイズ授受が素直なので従来どおり即完了させる（下の #else）。
        resolveResizeAck();  // 以前の保留が残っていれば先に解決（安全策）

        // リサイズ活動を記録（アイドル後の強制再同期トリガ用）。
        lastResizeActivityMs = juce::Time::getMillisecondCounter();
        settleReconcileDone = false;

        const bool sizeChanges = (getWidth() != targetW || getHeight() != targetH);
        if (sizeChanges)
        {
            // 自分の setSize 起因の resized() はホスト確定と区別する（resizeSelfDriven）。
            pendingResizeCompletion = std::move(completion);
            resizeAckPending = true;
            resizeAckStartMs = juce::Time::getMillisecondCounter();
            const juce::ScopedValueSetter<bool> selfDriven(resizeSelfDriven, true);
            setSize(targetW, targetH);
            // ホストが echo を返さない/サイズ据え置きの場合は timerCallback の安全タイムアウトで確定。
        }
        else
        {
            completion(juce::var{ true });  // サイズ不変なら往復不要
        }
#else
        // Windows / macOS: 従来どおり即時 setSize + 即完了。
        setSize(targetW, targetH);
        completion(juce::var{ true });
#endif
        return;
    }

    if (action == "resizeBy" && args.size() >= 3)
    {
        const int dw = juce::roundToInt((double)args[1]);
        const int dh = juce::roundToInt((double)args[2]);
        setSize(getWidth() + dw, getHeight() + dh);
        completion(juce::var{ true });
        return;
    }

    completion(juce::var{ false });
}

void MixCompare3AudioProcessorEditor::handleSystemAction(
    const juce::Array<juce::var>& args,
    juce::WebBrowserComponent::NativeFunctionCompletion completion)
{
    if (args.size() < 1)
    {
        completion(juce::var{ false });
        return;
    }

    const juce::String action = args[0].toString();

    if (action == "ready")
    {
        mc3::DiagnosticLog::log("webview: frontend 'ready' received — UI is alive");

        // 初期パラメータ送信は一度だけ
        if (!initialParamsSent.exchange(true))
        {
            
            sendInitialParameters();
        }
        completion(juce::var{ true });
        return;
    }
    
    if (action == "forward_key_event")
    {
        if (args.size() >= 2)
        {
            // KeyEventForwarderクラスを使用してキーイベントを転送
            const bool result = MixCompare::KeyEventForwarder::forwardKeyEventToHost(args[1], this);
            completion(juce::var{ result });
            return;
        }
        completion(juce::var{ false });
        return;
    }
    
    // 開発用：テストエラー発生
    #if JUCE_DEBUG
    if (action == "test_error")
    {
        if (args.size() >= 2)
        {
            const juce::String errorType = args[1].toString();
            
            if (errorType == "file_not_found")
            {
                MixCompare::ErrorManager::getInstance().reportError(
                    MixCompare::ErrorCode::FileNotFound,
                    "Test: Audio file not found",
                    "This is a test error. The file was not found at the specified location.",
                    "C:/TestFiles/missing_audio.wav");
            }
            else if (errorType == "format_not_supported")
            {
                MixCompare::ErrorManager::getInstance().reportError(
                    MixCompare::ErrorCode::FileFormatNotSupported,
                    "Test: Unsupported audio format",
                    "The file format '.xyz' is not supported on this platform.",
                    "C:/TestFiles/unsupported.xyz");
            }
            else if (errorType == "file_corrupted")
            {
                MixCompare::ErrorManager::getInstance().reportError(
                    MixCompare::ErrorCode::FileCorrupted,
                    "Test: Corrupted audio file",
                    "The audio file appears to be corrupted and cannot be decoded.",
                    "C:/TestFiles/corrupted.wav");
            }
            else if (errorType == "file_too_large")
            {
                MixCompare::ErrorManager::getInstance().reportError(
                    MixCompare::ErrorCode::FileTooLarge,
                    "Test: File size exceeds limit",
                    "The file is 512MB, which exceeds the maximum size of 256MB.",
                    "C:/TestFiles/huge_file.wav");
            }
            else if (errorType == "memory")
            {
                MixCompare::ErrorManager::getInstance().reportError(
                    MixCompare::ErrorCode::OutOfMemory,
                    "Test: Out of memory",
                    "Failed to allocate memory for audio buffer (requested: 1024MB)");
            }
            else if (errorType == "warning")
            {
                MixCompare::ErrorManager::getInstance().reportWarning(
                    MixCompare::ErrorCode::SampleRateMismatch,
                    "Test: Sample rate mismatch",
                    "Audio file sample rate (96kHz) differs from project rate (48kHz)");
            }
            else if (errorType == "info")
            {
                MixCompare::ErrorManager::getInstance().reportInfo(
                    "Test: Information message",
                    "This is a test information message. Everything is working correctly.");
            }
            else if (errorType == "critical")
            {
                MixCompare::ErrorManager::getInstance().reportCritical(
                    MixCompare::ErrorCode::AudioBufferOverflow,
                    "Test: Critical audio buffer overflow",
                    "Audio processing buffer has overflowed. This may cause audio dropouts.");
            }
            else
            {
                // デフォルトのエラー
                MixCompare::ErrorManager::getInstance().reportError(
                    MixCompare::ErrorCode::UnknownError,
                    "Test: Unknown error occurred",
                    "An unexpected error has occurred during testing.");
            }
            
            completion(juce::var{ true });
            return;
        }
    }
    #endif

    completion(juce::var{ false });
}

void MixCompare3AudioProcessorEditor::sendInitialParameters()
{
    // 現在のパラメータ値を送信
    juce::DynamicObject::Ptr paramData{new juce::DynamicObject{}};
    
    // 各パラメータの現在値を取得
    if (auto* hostGain = dynamic_cast<juce::AudioParameterFloat*>(
        audioProcessor.getState().getParameter("HOST_GAIN")))
    {
        paramData->setProperty("hostGain", hostGain->get());
    }
    
    if (auto* playlistGain = dynamic_cast<juce::AudioParameterFloat*>(
        audioProcessor.getState().getParameter("PLAYLIST_GAIN")))
    {
        paramData->setProperty("playlistGain", playlistGain->get());
    }
    
    if (auto* lpfFreq = dynamic_cast<juce::AudioParameterFloat*>(
        audioProcessor.getState().getParameter("LPF_FREQ")))
    {
        paramData->setProperty("lpfFreq", lpfFreq->get());
    }
    
    if (auto* lpfEnabled = dynamic_cast<juce::AudioParameterBool*>(
        audioProcessor.getState().getParameter("LPF_ENABLED")))
    {
        paramData->setProperty("lpfEnabled", lpfEnabled->get());
    }
    
    // SOURCE_SELECT は Choice(0=Host,1=Playlist)。index==1 を true として送る（後方互換）
    if (auto* sourceChoice = dynamic_cast<juce::AudioParameterChoice*>(
        audioProcessor.getState().getParameter("SOURCE_SELECT")))
    {
        const bool isPlaylist = (sourceChoice->getIndex() == 1);
        paramData->setProperty("sourceSelect", isPlaylist);
        paramData->setProperty("sourceSelectIndex", sourceChoice->getIndex());
    }
    else if (auto* sourceBool = dynamic_cast<juce::AudioParameterBool*>(
        audioProcessor.getState().getParameter("SOURCE_SELECT")))
    {
        // 旧バージョン互換：Bool の場合
        paramData->setProperty("sourceSelect", sourceBool->get());
        paramData->setProperty("sourceSelectIndex", sourceBool->get() ? 1 : 0);
    }
    
    if (auto* meteringModeChoice = dynamic_cast<juce::AudioParameterChoice*>(
        audioProcessor.getState().getParameter("METERING_MODE")))
    {
        const auto index = meteringModeChoice->getIndex();
        paramData->setProperty("meteringModeIndex", index);
    }
    // 追加: HOST_SYNC_ENABLED / HOST_SYNC_CAPABLE を初期送信
    if (auto* hostSyncEnabled = dynamic_cast<juce::AudioParameterBool*>(
        audioProcessor.getState().getParameter("HOST_SYNC_ENABLED")))
    {
        paramData->setProperty("hostSyncEnabled", hostSyncEnabled->get());
    }
    if (auto* hostSyncCapable = dynamic_cast<juce::AudioParameterBool*>(
        audioProcessor.getState().getParameter("HOST_SYNC_CAPABLE")))
    {
        paramData->setProperty("hostSyncCapable", hostSyncCapable->get());
    }
    
    

    // デバッグビルド時のみ詳細なメータリングモードをログ出力
    // リリースビルドでは未使用変数警告を避けるために計算自体を含めない
    

    
    
    webView.emitEventIfBrowserIsVisible("initialParameters", paramData.get());
    
    // プレイリストの初期状態も送信
    sendPlaylistUpdate();
    
    // トランスポート状態も送信
    sendTransportStateUpdate();
}

void MixCompare3AudioProcessorEditor::sendMeteringReset()
{
    // シャットダウン中は送信を行わない
    if (isShuttingDown.load())
        return;

    // Send metering reset notification to WebUI
    webView.emitEventIfBrowserIsVisible("meteringReset", juce::var{});
    
}

void MixCompare3AudioProcessorEditor::updateMeteringModeCache()
{
    // メータリングモードのキャッシュを更新
    cachedMeteringMode = audioProcessor.getMeteringMode();
    
}

void MixCompare3AudioProcessorEditor::sendErrorNotification(const MixCompare::ErrorInfo& error)
{
    // シャットダウン中は送信を行わない
    if (isShuttingDown.load())
        return;

    juce::DynamicObject::Ptr errorData{new juce::DynamicObject{}};
    
    errorData->setProperty("code", static_cast<int>(error.code));
    errorData->setProperty("severity", 
        error.severity == MixCompare::ErrorSeverity::Info ? "info" :
        error.severity == MixCompare::ErrorSeverity::Warning ? "warning" :
        error.severity == MixCompare::ErrorSeverity::Error ? "error" : "critical");
    errorData->setProperty("message", error.message);
    errorData->setProperty("details", error.details);
    errorData->setProperty("filePath", error.filePath);
    
    
    webView.emitEventIfBrowserIsVisible("errorNotification", errorData.get());
}

void MixCompare3AudioProcessorEditor::showWebViewStartupFailureNotice()
{
    if (webViewFailureNotice != nullptr)
        return;

    juce::String text;
    text << "MixCompare " << MIXCOMPARE_VERSION_STRING << "\n\n"
         << juce::String(juce::CharPointer_UTF8("プラグインUI (WebView) を起動できませんでした。\n"))
         << "The plugin UI (WebView) failed to start.\n\n"
         << juce::String(juce::CharPointer_UTF8("診断ログ / Diagnostic logs:\n"))
         << mc3::DiagnosticLog::getLogDirectory().getFullPathName() << "\n\n";

   #if JUCE_LINUX
    text << juce::String(juce::CharPointer_UTF8("Linux では WebKitGTK 4.1 が必要です / WebKitGTK 4.1 is required:\n"))
         << "  Fedora:        sudo dnf install webkit2gtk4.1\n"
         << "  Ubuntu/Debian: sudo apt install libwebkit2gtk-4.1-0\n\n";
   #endif

    text << juce::String(juce::CharPointer_UTF8("読み込みが遅いだけの場合、この表示は自動的に消えます。\n"))
         << "If loading is just slow, this notice will disappear automatically.\n\n"
         << juce::String(juce::CharPointer_UTF8("上記フォルダのログを添えてサポートまでご連絡ください。\n"))
         << "Please contact support with the log files from the folder above.";

    webViewFailureNotice = std::make_unique<juce::Label>();
    webViewFailureNotice->setJustificationType(juce::Justification::centred);
    webViewFailureNotice->setColour(juce::Label::backgroundColourId, juce::Colour(0xFF37474F));
    webViewFailureNotice->setColour(juce::Label::textColourId, juce::Colours::white);
    webViewFailureNotice->setFont(juce::FontOptions(15.0f));
    webViewFailureNotice->setMinimumHorizontalScale(0.7f);
    webViewFailureNotice->setText(text, juce::dontSendNotification);

    addAndMakeVisible(*webViewFailureNotice);
    webViewFailureNotice->setBounds(getLocalBounds());
    webViewFailureNotice->toFront(false);
}

// getMimeForExtension関数は既に名前空間内で定義されているため削除
