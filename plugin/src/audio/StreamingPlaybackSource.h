// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include "IPlaybackSource.h"

namespace MixCompare
{

class StreamingPlaybackSource final : public IPlaybackSource
{
public:
    StreamingPlaybackSource(const juce::File& audioFile,
                            juce::AudioFormatManager& formatManager);
    explicit StreamingPlaybackSource(std::unique_ptr<juce::AudioFormatReader> prebuiltReader);
    ~StreamingPlaybackSource() override;

    void prepare(double sampleRate, int blockSize) override;
    void release() override;
    void process(juce::AudioBuffer<float>& outBuffer, int numSamples) noexcept override;

    void seek(double positionSeconds) noexcept override;
    void setLoop(double startSec, double endSec, bool enabled) noexcept override;

    double getDurationSec() const noexcept override { return durationSec; }
    double getFileSampleRate() const noexcept override { return fileSampleRate; }
    double getCurrentPositionSec() const noexcept override
    {
        if (transport)
            return transport->getCurrentPosition();
        return 0.0;
    }

    void beginSwap() noexcept override;
    void endSwap() noexcept override;

private:
    juce::File file;
    std::unique_ptr<juce::AudioFormatReader> reader;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    std::unique_ptr<juce::TimeSliceThread> readAheadThread;
    std::unique_ptr<juce::AudioTransportSource> transport;

    double hostSampleRate{44100.0};
    int hostBlockSize{512};
    double fileSampleRate{44100.0};
    double durationSec{0.0};

    std::atomic<bool> loopEnabled{false};
    std::atomic<double> loopStartSec{0.0};
    std::atomic<double> loopEndSec{0.0};
    std::atomic<double> pendingSeekSec{-1.0};
    
    // シーク直後の1ブロックはループチェックをスキップ（範囲外シークを許可）
    std::atomic<bool> skipLoopCheckOnce{false};

    // ループ無効化フラグ（再武装条件: 再生位置が一度 loopEnd より下に戻ったとき）
    // - 目的: ループ範囲の未来側へシークした際に即時巻き戻しを防ぎ、そのまま再生を許可する。
    // - 動作: disarmed 中は wrap しない。pos < loopEnd を検出したら自動で再武装（wrap が有効化）。
    std::atomic<bool> loopDisarmedUntilBelowEnd{false};

    int fadeSamples{0};
    int fadeInRemain{0};
    int fadeOutRemain{0};
    std::atomic<bool> swapRequested{false};
    int loopFadeSamples{0};
    bool loopFadeInActive{false};
    int loopFadeInProgress{0};

    void applyFades(juce::AudioBuffer<float>& buffer, int numSamples) noexcept;
    void applyLoopFadeOut(juce::AudioBuffer<float>& buffer, int numSamples) noexcept;
    void applyLoopFadeIn(juce::AudioBuffer<float>& buffer, int numSamples) noexcept;
};

} // namespace MixCompare
