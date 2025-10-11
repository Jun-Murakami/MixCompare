#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "ParameterIDs.h"
#include "Version.h" // CMakeで自動生成されるバージョン情報ヘッダー（VERSIONファイル由来）
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
    
    // ウィンドウサイズ設定　AAXは横幅450px, 縦幅650px
    if (juce::PluginHostType().isProTools()
        && juce::PluginHostType::getPluginLoadedAs() == juce::AudioProcessor::WrapperType::wrapperType_AAX)
    {
        setSize(450, 650);
    }
    else
    {
        setSize(392, 650);
    }
    
    // リサイズ可能に設定（プラグイン・スタンドアロン共通）
    setResizable(true, true);
    setResizeLimits(392, 610, 2560, 1440);
    
    // リサイズ制約を設定
    resizerConstraints.setMinimumSize(392, 610);
    resizerConstraints.setMaximumSize(2560, 1440);
    
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

    // 初期レイアウトを適用（グリッパーとWebViewの配置を正しく設定）
    resized();
}

MixCompare3AudioProcessorEditor::~MixCompare3AudioProcessorEditor()
{
    // シャットダウンフラグを最初に立てる
    isShuttingDown.store(true);

    // タイマーを即座に停止
    stopTimer();
    
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
    
    // リサイズグリッパーの位置を更新（最前面に）
    if (resizer)
    {
        resizer->setBounds(getWidth() - gripperSize, 
                          getHeight() - gripperSize, 
                          gripperSize, 
                          gripperSize);
        resizer->toFront(true);  // 最前面に移動
    }
}

void MixCompare3AudioProcessorEditor::timerCallback()
{
    // 終了中・モーダルダイアログ表示中は送出を停止（OpenPanel 等の操作性確保）
    if (isShuttingDown.load() || activeModalDialogs.load(std::memory_order_acquire) > 0)
        return;

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
        juce::String supportedFormats = "*.wav;*.aif;*.aiff;*.mp3;*.flac;*.ogg;*.m4a;*.aac";
        
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

    if (action == "resizeTo" && args.size() >= 3)
    {
        const int w = juce::roundToInt((double)args[1]);
        const int h = juce::roundToInt((double)args[2]);
        setSize(w, h);
        completion(juce::var{ true });
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

// getMimeForExtension関数は既に名前空間内で定義されているため削除
