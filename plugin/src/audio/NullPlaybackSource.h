#pragma once

#include "IPlaybackSource.h"

namespace MixCompare
{

/**
 * NullPlaybackSource
 *
 * ダミーの再生ソース。無音を出力するだけで、実装の足場として使用する。
 */
class NullPlaybackSource final : public IPlaybackSource
{
public:
    void prepare(double newSampleRate, int newBlockSize) override
    {
        sampleRate = newSampleRate;
        blockSize = newBlockSize;
    }

    void release() override {}

    void process(juce::AudioBuffer<float>& outBuffer, int numSamples) noexcept override
    {
        const int channels = outBuffer.getNumChannels();
        for (int ch = 0; ch < channels; ++ch)
            outBuffer.clear(ch, 0, numSamples);
    }

    void seek(double) noexcept override {}
    void setLoop(double, double, bool) noexcept override {}
    double getDurationSec() const noexcept override { return 0.0; }
    double getFileSampleRate() const noexcept override { return sampleRate; }
    double getCurrentPositionSec() const noexcept override { return 0.0; }
    void beginSwap() noexcept override {}
    void endSwap() noexcept override {}

private:
    double sampleRate{44100.0};
    int blockSize{512};
};

} // namespace MixCompare



