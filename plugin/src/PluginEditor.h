#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"
#include "core/ErrorManager.h"
#include <optional>
#include <vector>

class MixCompare3AudioProcessorEditor : public juce::AudioProcessorEditor,
                                       public MixCompare::StateManager::Listener,
                                       public MixCompare::TransportManager::Listener,
                                       public MixCompare::PlaylistManager::Listener,
                                       private juce::Timer
{
public:
    // 基底クラスの transportStateChanged を両方とも明示的に導入
    // （StateManager::Listener と TransportManager::Listener の両方で定義されているため）
    using MixCompare::StateManager::Listener::transportStateChanged;
    using MixCompare::TransportManager::Listener::transportStateChanged;

    explicit MixCompare3AudioProcessorEditor(MixCompare3AudioProcessor&);
    ~MixCompare3AudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    
    // プレイリスト更新をWebUIに送信
    void sendPlaylistUpdate();
    void sendTransportStateUpdate();

    void sendTransportPositionUpdate(double positionInSeconds);
    void sendTrackChangeUpdate(); // Combined update for track switching
    void sendMeteringReset(); // Send metering reset notification to WebUI
    
    // エラー通知をWebUIに送信
    void sendErrorNotification(const MixCompare::ErrorInfo& error);

    // メータリングモードの更新通知（PluginProcessorから呼ばれる）
    void updateMeteringModeCache();

private:
    void timerCallback() override;
    // StateManager / Managers のリスナー実装（UI送信の一元化）
    void stateChanged(const juce::ValueTree& newState) override;
    void playlistChanged() override;
    void transportStateChanged(MixCompare::TransportManager::TransportState newState) override;
    void transportPositionChanged(double newPosition) override;
    void loopStateChanged(bool enabled, double start, double end) override;
    void currentItemChanged(const juce::String& itemId) override;
    
    // リソースプロバイダ
    using Resource = juce::WebBrowserComponent::Resource;
    std::optional<Resource> getResource(const juce::String& url) const;
    
    // ネイティブ関数ハンドラ
    void handlePlaylistAction(const juce::Array<juce::var>& args,
                              juce::WebBrowserComponent::NativeFunctionCompletion completion);
    void handleWindowAction(const juce::Array<juce::var>& args,
                            juce::WebBrowserComponent::NativeFunctionCompletion completion);
    void handleSystemAction(const juce::Array<juce::var>& args,
                           juce::WebBrowserComponent::NativeFunctionCompletion completion);
    
    // 初期パラメータ送信
    void sendInitialParameters();
                           
    
    // JUCE の AudioProcessorEditor::processor と名前が衝突しないように、
    // 型付き参照は audioProcessor という名前で保持する
    MixCompare3AudioProcessor& audioProcessor;
    
    // Webパラメータリレー（WebBrowserComponentより先に宣言）
    juce::WebSliderRelay webHostGainRelay;
    juce::WebSliderRelay webPlaylistGainRelay;
    juce::WebSliderRelay webLpfFreqRelay;
    juce::WebToggleButtonRelay webLpfEnabledRelay;
    juce::WebComboBoxRelay webSourceSelectRelay;
    juce::WebComboBoxRelay webMeteringModeRelay;
    juce::WebToggleButtonRelay webTransportPlayingRelay;
    juce::WebToggleButtonRelay webLoopEnabledRelay;
    juce::WebSliderRelay webTransportSeekNormRelay;
    juce::WebSliderRelay webLoopStartNormRelay;
    juce::WebSliderRelay webLoopEndNormRelay;
    juce::WebSliderRelay webPlaylistCurrentIndexNormRelay;
    juce::WebToggleButtonRelay webHostSyncEnabledRelay;
    juce::WebToggleButtonRelay webHostSyncCapableRelay;
    
    // パラメータアタッチメント
    juce::WebSliderParameterAttachment hostGainAttachment;
    juce::WebSliderParameterAttachment playlistGainAttachment;
    juce::WebSliderParameterAttachment lpfFreqAttachment;
    juce::WebToggleButtonParameterAttachment lpfEnabledAttachment;
    juce::WebComboBoxParameterAttachment sourceSelectAttachment;
    juce::WebComboBoxParameterAttachment meteringModeAttachment;
    juce::WebToggleButtonParameterAttachment transportPlayingAttachment;
    juce::WebToggleButtonParameterAttachment loopEnabledAttachment;
    juce::WebSliderParameterAttachment transportSeekNormAttachment;
    juce::WebSliderParameterAttachment loopStartNormAttachment;
    juce::WebSliderParameterAttachment loopEndNormAttachment;
    juce::WebSliderParameterAttachment playlistCurrentIndexNormAttachment;
    juce::WebToggleButtonParameterAttachment hostSyncEnabledAttachment;
    juce::WebToggleButtonParameterAttachment hostSyncCapableAttachment;

    // WebViewPluginDemo 準拠: DOM要素にパラメータインデックスを注入して自動バインド
    juce::WebControlParameterIndexReceiver controlParameterIndexReceiver;
    
    // WebView ライフタイム監視（構築/破棄の時点を把握して安全に停止・破棄するため）
    struct WebViewLifetimeGuard : public juce::WebViewLifetimeListener
    {
        std::atomic<bool> constructed{ false };
        void webViewConstructed(juce::WebBrowserComponent*) override { constructed.store(true, std::memory_order_release); }
        void webViewDestructed(juce::WebBrowserComponent*) override { constructed.store(false, std::memory_order_release); }
        bool isConstructed() const { return constructed.load(std::memory_order_acquire); }
    } webViewLifetimeGuard;
    
    // WebView関連（最後に宣言）
    juce::WebBrowserComponent webView;
    
    // デバッグモード
    bool useLocalDevServer = false;
    
    // シーケンス番号（トランスポート更新の競合防止用）
    std::atomic<int> transportSequenceNumber{0};
    int lastSentSequenceNumber = 0;

    // セッションID（Editor再生成時など、古いシーケンスを無効化するための世代識別子）
    std::atomic<uint64_t> transportSessionId{0};
    static std::atomic<uint64_t> transportSessionIdGenerator;

    // 更新リビジョン（遅延着信の古い更新を破棄するための単調増加番号）
    //  - playlistRevision: プレイリストの並べ替え/追加/削除/選択などの変更ごとに++
    //  - transportRevision: 再生/一時停止/停止/シーク/ループ設定変更などの変更ごとに++
    std::atomic<uint64_t> playlistRevision{0};
    std::atomic<uint64_t> transportRevision{0};
    
    // トランスポート状態のキャッシュ（変更検出用）
    bool lastTransportIsPlaying{false};
    [[maybe_unused]] double lastTransportPosition{0.0}; // 現在未使用。将来的な位置変化検出のために保持し、未使用警告のみ抑止
    bool lastTransportLoopEnabled{false};

    // メータリングモードのキャッシュ（変更検出用）
    int cachedMeteringMode{0};

    // リサイズグリッパー（スタンドアロン用）
    std::unique_ptr<juce::ResizableCornerComponent> resizer;
    juce::ComponentBoundsConstrainer resizerConstraints;
    
    // シャットダウン中フラグ（非同期イベントの早期リターンに使用）
    std::atomic<bool> isShuttingDown{ false };
    std::atomic<bool> initialParamsSent{ false };
    // OSネイティブダイアログ（NSOpenPanel等）表示中のカウンタ
    std::atomic<int> activeModalDialogs{ 0 };

#if defined(JUCE_WINDOWS)
    // ウィンドウ固有DPI監視（グローバルDPIではなく HWND 基準）
    double lastHwndScaleFactor { 0.0 };     // GetDpiForWindow→スケール換算
    int    lastHwndDpi        { 0 };        // 物理解像度DPI(Per-Monitor V2)
    void   pollAndMaybeNotifyDpiChange();   // タイマー内から呼ぶ
#endif
    
    // 初期パラメータ値送信フラグ（未使用のため削除）

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixCompare3AudioProcessorEditor)
};
