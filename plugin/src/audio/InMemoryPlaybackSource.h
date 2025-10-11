#pragma once

#include <JuceHeader.h>
#include <atomic>
#include "IPlaybackSource.h"

namespace MixCompare
{

/**
 * InMemoryPlaybackSource
 * - 起動時に全ファイルをデコードしてメモリに展開し、以降はメモリから再生する。
 * - m4a/aac などストリーミングで不安定なフォーマット向けのフォールバック用。
 */
class InMemoryPlaybackSource final : public IPlaybackSource
{
public:
    InMemoryPlaybackSource() = default;
    // フィールド宣言順（fileSampleRate, fileBuffer）に合わせて初期化
    explicit InMemoryPlaybackSource(juce::AudioBuffer<float> pcm, double fileSR)
        : fileSampleRate(fileSR), fileBuffer(std::move(pcm))
    {
        const int numSamples = fileBuffer.getNumSamples();
        durationSec = (fileSampleRate > 0.0) ? (static_cast<double>(numSamples) / fileSampleRate) : 0.0;
    }

    ~InMemoryPlaybackSource() override = default;

    void prepare(double sampleRate, int blockSize) override;
    void release() override;
    void process(juce::AudioBuffer<float>& outBuffer, int numSamples) noexcept override;

    void seek(double positionSeconds) noexcept override;
    void setLoop(double startSec, double endSec, bool enabled) noexcept override;

    double getDurationSec() const noexcept override { return durationSec; }
    double getFileSampleRate() const noexcept override { return fileSampleRate; }
    double getCurrentPositionSec() const noexcept override;

    void beginSwap() noexcept override;
    void endSwap() noexcept override;

    // ユーティリティ: リーダーから全PCMを読み取る
    static bool readAllToBuffer(juce::AudioFormatReader& reader, juce::AudioBuffer<float>& dst);

private:
    // 再生用パラメータ
    double hostSampleRate{44100.0};
    int hostBlockSize{512};
    double fileSampleRate{44100.0};
    double durationSec{0.0};

    // データ
    juce::AudioBuffer<float> fileBuffer; // ch x samples

    // 再生位置（ファイルSR基準のサンプル）
    double currentPosSamples{0.0};
    std::atomic<double> pendingSeekSec{-1.0};

    // ループ
    std::atomic<bool> loopEnabled{false};
    std::atomic<double> loopStartSec{0.0};
    std::atomic<double> loopEndSec{0.0};
    std::atomic<bool> loopDisarmedUntilBelowEnd{false};

    // スワップ用フェード
    int fadeSamples{0};
    int fadeInRemain{0};
    int fadeOutRemain{0};
    std::atomic<bool> swapRequested{false};
    int loopFadeSamples{0};
    bool loopFadeInActive{false};
    int loopFadeInProgress{0};

    void applyFades(juce::AudioBuffer<float>& buffer, int numSamples) noexcept;
    void applyLoopTransition(juce::AudioBuffer<float>& buffer, int wrapSampleIndex, int numSamples) noexcept;
};

} // namespace MixCompare
