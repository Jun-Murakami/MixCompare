#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <vector>
#include <memory>
#include <mutex>
#include <thread>
#include "core/AudioEngine.h"
#include "core/StateManager.h"
#include "core/TransportManager.h"
#include "ParameterIDs.h"

// Forward declarations for managers
namespace MixCompare
{
    class AudioEngine;
    class PlaylistManager;
    class MeteringService;
}

namespace mc3 {

// プレイリストアイテム
struct PlaylistItem
{
    juce::String id;
    juce::File file;
    juce::String name;
    double duration = 0.0;
    bool isLoaded = false;
};

// オーディオソース列挙
enum class AudioSource
{
    Host = 0,
    Playlist = 1
};

// トランスポート状態
struct TransportState {
    std::atomic<bool> isPlaying{false};
    std::atomic<double> position{0.0};
    std::atomic<double> loopStart{0.0};
    std::atomic<double> loopEnd{0.0};
    std::atomic<bool> loopEnabled{false};
    
    // コピーコンストラクタ（atomic変数のコピー用）
    TransportState() = default;
    TransportState(const TransportState& other)
        : isPlaying(other.isPlaying.load()),
          position(other.position.load()),
          loopStart(other.loopStart.load()),
          loopEnd(other.loopEnd.load()),
          loopEnabled(other.loopEnabled.load())
    {}
};


} // namespace mc3

class MixCompare3AudioProcessor : public juce::AudioProcessor,
                                  public juce::AudioProcessorValueTreeState::Listener,
                                  public MixCompare::StateManager::Listener,
                                  public MixCompare::TransportManager::Listener,
                                  public juce::Timer
{
public:
    // 基底クラスの transportStateChanged を両方とも明示的に導入
    // （StateManager::Listener と TransportManager::Listener の両方で定義されているため）
    using MixCompare::StateManager::Listener::transportStateChanged;
    using MixCompare::TransportManager::Listener::transportStateChanged;

    MixCompare3AudioProcessor();
    ~MixCompare3AudioProcessor() override;

    // AudioProcessor基本
    const juce::String getName() const override;
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const juce::AudioProcessor::BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    // エディタ（GUI）
    bool hasEditor() const override;
    juce::AudioProcessorEditor* createEditor() override;

    // プロパティ
    double getTailLengthSeconds() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;

    // 状態保存/プログラム
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int) override;
    const juce::String getProgramName(int) override;
    void changeProgramName(int, const juce::String&) override;
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;
    
    // AudioProcessorValueTreeState::Listener
    void parameterChanged(const juce::String& parameterID, float newValue) override;
    
    // TransportManager::Listener
    void transportStateChanged(MixCompare::TransportManager::TransportState newState) override;
    void transportPositionChanged(double newPosition) override;
    void loopStateChanged(bool enabled, double start, double end) override;
    
    // Timer
    void timerCallback() override;

    // パラメータアクセス
    juce::AudioProcessorValueTreeState& getState() { return parameters; }
    
    // プレイリスト管理
    void addFilesToPlaylist(const juce::Array<juce::File>& files);
    void removeFromPlaylist(const juce::String& id);
    void reorderPlaylist(int fromIndex, int toIndex);
    void clearPlaylist();
    void exportPlaylistToM3U8(const juce::File& file);
    void importPlaylistFromM3U8(const juce::File& file);
    std::vector<mc3::PlaylistItem> getPlaylist() const;
    int getCurrentPlaylistIndex() const;
    
    // トランスポート制御
    void play();
    void pause();
    void stop();
    void seek(double position);
    void setLoopRange(double start, double end);
    void setLoopEnabled(bool enabled);
    mc3::TransportState getTransportState() const;
    
    // プレイリスト選択
    void selectPlaylistItem(int index);
    void selectPreviousPlaylistItem();
    void selectNextPlaylistItem();
    
    // メータリング（各ソース独立）
    std::atomic<float> hostLevelLeft{-60.0f};
    std::atomic<float> hostLevelRight{-60.0f};
    std::atomic<float> playlistLevelLeft{-60.0f};
    std::atomic<float> playlistLevelRight{-60.0f};
    
    // 出力レベル（選択されたソース）
    std::atomic<float> outputLevelLeft{0.0f};
    std::atomic<float> outputLevelRight{0.0f};

    // TruePeak（サンプル間引きなしの最大絶対値）蓄積用（リニア振幅）
    // - オーディオスレッドで compare-exchange により最大値を蓄積
    // - GUI タイマーで exchange(0) して区間最大を取り出し＆リセット
    std::atomic<float> hostTruePeakMaxLeft{0.0f};
    std::atomic<float> hostTruePeakMaxRight{0.0f};
    std::atomic<float> playlistTruePeakMaxLeft{0.0f};
    std::atomic<float> playlistTruePeakMaxRight{0.0f};
    
    // MeteringService accessor (for UI)
    MixCompare::MeteringService* getMeteringService() { return meteringService.get(); }
    
    // True peak accessors for AudioEngine metering
    float getHostTruePeak(int channel) const;
    float getPlaylistTruePeak(int channel) const;
    
    // Metering mode
    bool isPeakMode() const {
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*>(
                parameters.getParameter(mc3::id::METERING_MODE.getParamID())))
        {
            return choice->getIndex() == 0;
        }
        if (auto* param = dynamic_cast<juce::AudioParameterBool*>(
                parameters.getParameter(mc3::id::METERING_MODE.getParamID())))
        {
            return param->get();
        }
        return true;
    }
    
    int getMeteringMode() const {
        // オーディオスレッドが用いる原子値をAudioEngineから直接参照し、
        // DAWオートメーション再生中の変化をGUI側でも即時反映する。
        if (audioEngine)
            return audioEngine->getRealtimeMeteringMode();
        // フォールバック（AudioEngine未初期化時のみ）
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*>(
                parameters.getParameter(mc3::id::METERING_MODE.getParamID())))
            return choice->getIndex();
        if (auto* param = dynamic_cast<juce::AudioParameterBool*>(
                parameters.getParameter(mc3::id::METERING_MODE.getParamID())))
            return param->get() ? 0 : 1;
        return 0;
    }
    
    // Access to StateManager for bridge operations
    MixCompare::StateManager* getStateManager() const { return stateManager.get(); }
    MixCompare::TransportManager* getTransportManager() const { return transportManager.get(); }
    MixCompare::PlaylistManager* getPlaylistManager() const { return playlistManager.get(); }

private:
    // Internal methods
    void handlePlaybackStopped();
    void cancelBackgroundLoadThread();
    void runBackgroundLoad(uint64_t epoch);
    void applyLoadedSource(bool loaded,
                           uint64_t epoch,
                           std::shared_ptr<MixCompare::IPlaybackSource> preparedSource);
    
    // Refactored managers
    std::unique_ptr<MixCompare::StateManager> stateManager;
    std::unique_ptr<MixCompare::AudioEngine> audioEngine;
    std::unique_ptr<MixCompare::TransportManager> transportManager;
    std::unique_ptr<MixCompare::PlaylistManager> playlistManager;
    std::unique_ptr<MixCompare::MeteringService> meteringService;
    
    // パラメータ
    juce::AudioProcessorValueTreeState parameters;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    
    // プレイリスト
    std::vector<mc3::PlaylistItem> playlist;
    std::atomic<int> currentPlaylistIndex{-1};
    
    juce::AudioBuffer<float> tempBuffer;
    
    // トランスポート（Processor ローカルキャッシュは廃止、TransportManager を真実源とする）
    std::atomic<double> playbackPosition{0.0};
    double currentSampleRate = 44100.0;
    // シーク順序の単調増加番号（position更新の旧イベント無視用）
    std::atomic<uint64_t> positionEpoch{0};
    
    // オーディオスレッドからの位置更新用（メッセージスレッドで処理）
    std::atomic<double> pendingPositionUpdate{-1.0};
    std::atomic<bool> hasPendingPositionUpdate{false};
    std::atomic<uint64_t> pendingPositionEpoch{0};
    // 選曲後のロード/ソース適用のフォールバック用フラグ（メッセージスレッドで処理）
    std::atomic<bool> pendingSourceRefresh{false};
    std::atomic<bool> isLoadingSource{false};
    std::atomic<uint64_t> loadEpoch{0};
    void asyncLoadAndApplyCurrentItem();
    std::mutex backgroundLoadMutex;
    std::thread backgroundLoadThread;
    std::atomic<bool> cancelBackgroundLoad{false};

    // HOST_SYNC_CAPABLE のランタイム自動判定（-1=未要求, 0/1=要求値）
    std::atomic<int> pendingHostSyncCapable{-1};

    // オーディオスレッドからの同期OFF要求（メッセージスレッドで処理）
    std::atomic<bool> pendingDisableHostSync{false};

    // オーディオスレッド→メッセージスレッド: ホスト同期状態のUI反映（TransportManager側）
    std::atomic<bool> pendingTransportHostUpdate{false};
    std::atomic<double> pendingTransportHostPosSec{0.0};
    std::atomic<bool> pendingTransportHostIsPlaying{false};
    
    // ソース管理
    std::atomic<mc3::AudioSource> currentSource{mc3::AudioSource::Host};

public:
    uint64_t getPositionEpoch() const { return positionEpoch.load(std::memory_order_acquire); }

    // 同期
    juce::CriticalSection playlistLock;
    juce::CriticalSection bufferLock;

    JUCE_DECLARE_WEAK_REFERENCEABLE(MixCompare3AudioProcessor)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixCompare3AudioProcessor)
};




