#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include "../audio/IPlaybackSource.h"
// StateSnapshot is removed. No include required.

namespace MixCompare
{

class AudioEngine : public juce::AsyncUpdater
{
public:
    AudioEngine();
    ~AudioEngine() override;

    /**
     * オーディオスレッドが直接参照するリアルタイム・パラメータ群。
     * - ロック不要・原子的に更新され、各ブロックで即時反映される。
     */
    struct RealtimeParams
    {
        float hostGainDb{0.0f};
        float playlistGainDb{0.0f};
        bool  lpfEnabled{false};
        float lpfFrequencyHz{20000.0f};
        int meteringMode{0}; // 0=Peak,1=Rms,2=Momentary
        bool transportPlaying{false};
        int  currentSourceIndex{0}; // 0=Host,1=Playlist
        // ループ制御（ファイルサンプルレート基準のサンプル位置）
        bool  loopEnabled{false};
        double loopStartSamples{0.0};
        double loopEndSamples{0.0};
    };

    void prepareToPlay(double sampleRate, int samplesPerBlock);
    void releaseResources();
    
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages);
    
    // Get the last processed buffers for metering
    void getLastProcessedBuffers(juce::AudioBuffer<float>& hostBuffer, 
                                 juce::AudioBuffer<float>& playlistBuffer) const;

    // 新API: 直接再生ソースを設定（原子的スワップ）
    void setPlaybackSource(std::shared_ptr<IPlaybackSource> source);
    /**
     * 事前に prepare 済みのソースを適用する（UIでの prepare 実行を避ける）。
     * - 内部ではクロスフェード切替のみ行い、prepare は呼ばない。
     */
    void setPlaybackSourcePrepared(std::shared_ptr<IPlaybackSource> preparedSource);

    // Snapshot 経路は廃止（APVTS直読・リアルタイム原子値に統一）

    // Realtime parameter update (メイン/オーディオ以外のスレッドから呼ぶ)
    void setRealtimeParams(const RealtimeParams& params) noexcept;

    /**
     * AsyncUpdater コールバック（メッセージスレッド）
     * - オーディオスレッドから退役させた再生ソースのクリーンアップ（release/reset）を行う。
     */
    void handleAsyncUpdate() override;

    /**
     * 現在のメータリングモードをリアルタイム原子値から取得する。
     * 0=Peak, 1=RMS, 2=Momentary
     * - メイン/GUI スレッドから安全に参照できる（memory_order_acquire）。
     */
    int getRealtimeMeteringMode() const { return rtMeteringMode.load(std::memory_order_acquire); }
    
    // メータリングは MeteringService に委譲
    
    // Transport control
    double getCurrentPosition() const { return currentPlaybackPosition; }
    void setPlaybackPosition(double position);
    void seekSeconds(double positionSeconds);
    double getDuration() const;
    double getFileSampleRate() const;
    bool hasCurrentSource() const noexcept;
    double getSampleRate() const noexcept { return sampleRate; }
    int getBlockSize() const noexcept { return blockSize; }
    
    // Callback for when playback reaches end of file (not looping)
    std::function<void()> onPlaybackStopped;
    std::function<void(double)> onPositionChanged;

    /**
     * UI側のちらつきを防ぐため、次の processBlock 内で 0 秒位置を一度だけ通知する。
     * - 選曲直後に旧曲の位置が一瞬表示されるのを抑止。
     */
    void requestForceZeroPositionNotify() noexcept { forceZeroPositionNotify.store(true, std::memory_order_release); }
    void setSuppressPositionNotify(bool shouldSuppress) noexcept { suppressPositionNotify.store(shouldSuppress, std::memory_order_release); }

    /**
     * ホスト同期状態を設定（オーディオスレッドで参照される原子的値）。
     * - enabled: ホスト同期を有効にするか
     * - isPlaying: ホスト側の再生状態
     * - positionSec: ホスト側の現在位置（秒）
     */
    void setHostSyncState(bool enabled, bool isPlaying, double positionSec) noexcept;

private:
    double sampleRate = 44100.0;
    int blockSize = 512;
    
    // Last processed buffers for metering
    mutable juce::AudioBuffer<float> lastHostBuffer;
    mutable juce::AudioBuffer<float> lastPlaylistBuffer;
    
    // State snapshot for real-time access
    // Using raw pointer with atomic for lock-free access
    // Snapshot 経路は廃止
    
    // LPF filters (still needed for DSP processing)
    juce::dsp::StateVariableTPTFilter<float> lowPassFilterLeft;
    juce::dsp::StateVariableTPTFilter<float> lowPassFilterRight;
    
    // Audio sources (streaming only)
    // std::shared_ptr は非トリビアル型のため、std::atomic<shared_ptr> は使わず
    // std::atomic_load/store (C++11) を用いたロックフリーアクセスで同期する。
    std::shared_ptr<IPlaybackSource> currentSource;
    std::shared_ptr<IPlaybackSource> nextSource;
    
    // Playback tracking (local state for audio thread)
    double currentPlaybackPosition{0.0};
    double previousPosition{0.0};  // Track previous position for loop detection
    // 外部からのシーク（setPlaybackPosition）を検知するためのフラグ。
    // - ユーザーがループ範囲内で再生中に範囲外へシークした場合は、
    //   次のブロックでのループ・ラップ動作を抑止したい。
    // - setPlaybackPosition() 呼び出し毎に true にセットし、
    //   processPlaylistAudio() の先頭で 1 回だけ読み出して false に戻す。
    std::atomic<bool> externalSeekPending{false};
    
    // メータリング内部状態は保持しない（MeteringService に集約）

    // Realtime params (atomic for lock-free reads on audio thread)
    std::atomic<float> rtHostGainDb{0.0f};
    std::atomic<float> rtPlaylistGainDb{0.0f};
    std::atomic<bool>  rtLpfEnabled{false};
    std::atomic<float> rtLpfFrequencyHz{20000.0f};
    std::atomic<int> rtMeteringMode{0};
    std::atomic<bool>  rtTransportPlaying{false};
    std::atomic<int>   rtCurrentSourceIndex{0};
    std::atomic<bool>  rtLoopEnabled{false};
    std::atomic<double> rtLoopStartSamples{0.0};
    std::atomic<double> rtLoopEndSamples{0.0};

    // ホスト同期用リアルタイム値
    std::atomic<bool>   rtHostSyncEnabled{false};
    std::atomic<bool>   rtHostIsPlaying{false};
    std::atomic<double> rtHostPositionSec{0.0};

    // 位置通知の強制ゼロ（次ブロックで1回だけ）
    std::atomic<bool> forceZeroPositionNotify{false};
    std::atomic<bool> suppressPositionNotify{false};

    // EOF 通知の多重発火防止フラグは廃止（差分で検出）

    // メータリング関連の内部更新は廃止
    void applyLowPassFilter(juce::AudioBuffer<float>& buffer, int numSamples);

    // ワークバッファ（固定確保）
    juce::AudioBuffer<float> workHostBuffer;
    juce::AudioBuffer<float> workPlaylistBuffer;
    juce::AudioBuffer<float> workNextPlaylistBuffer; // スワップ時の次ソース描画用

    // スワップ用フェード（サンプル数）
    int crossfadeSamples{0};
    std::atomic<int> remainingCrossfadeSamples{0};

    // 退役ソース（メッセージスレッドで解放するための一時保管）
    // - release/reset を非同期で行うため atomic に退避
    std::shared_ptr<IPlaybackSource> retiredSourceOwned;

    // ブロック間で使い回すスムージングとフィルタ用ワークバッファ
    juce::LinearSmoothedValue<float> hostGainSmoothed{1.0f};
    juce::LinearSmoothedValue<float> playlistGainSmoothed{1.0f};
    juce::LinearSmoothedValue<float> sourceBlendSmoothed{0.0f}; // 0=Host,1=Playlist
    juce::LinearSmoothedValue<float> lpfMixSmoothed{0.0f};
    juce::AudioBuffer<float> workFilteredBuffer;
    bool lastLpfEnabled{false};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};

} // namespace MixCompare
