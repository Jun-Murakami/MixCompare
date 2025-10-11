#include "InMemoryPlaybackSource.h"

namespace MixCompare
{

void InMemoryPlaybackSource::prepare(double sampleRate, int blockSize)
{
    hostSampleRate = sampleRate;
    hostBlockSize = blockSize;
    fadeSamples = static_cast<int>(std::max(1.0, hostSampleRate * 0.005));
    loopFadeSamples = static_cast<int>(std::max(1.0, hostSampleRate * 0.01));
    fadeInRemain = 0;
    fadeOutRemain = 0;
    loopFadeInActive = false;
    loopFadeInProgress = 0;
    loopDisarmedUntilBelowEnd.store(false, std::memory_order_release);
}

void InMemoryPlaybackSource::release()
{
    loopFadeInActive = false;
    loopFadeInProgress = 0;
    loopDisarmedUntilBelowEnd.store(false, std::memory_order_release);
}

bool InMemoryPlaybackSource::readAllToBuffer(juce::AudioFormatReader& reader, juce::AudioBuffer<float>& dst)
{
    const int64 length = static_cast<int64>(reader.lengthInSamples);
    const int channels = static_cast<int>(reader.numChannels);
    if (length <= 0 || channels <= 0) return false;

    dst.setSize(std::max(2, channels), static_cast<int>(length));
    dst.clear();

    juce::AudioSampleBuffer temp(channels, static_cast<int>(length));
    reader.read(&temp, 0, static_cast<int>(length), 0, true, true);

    // ch copy (expand mono to stereo)
    for (int ch = 0; ch < dst.getNumChannels(); ++ch)
    {
        const int srcCh = std::min(ch, channels - 1);
        dst.copyFrom(ch, 0, temp, srcCh, 0, static_cast<int>(length));
    }
    return true;
}

void InMemoryPlaybackSource::process(juce::AudioBuffer<float>& outBuffer, int numSamples) noexcept
{
    outBuffer.clear();
    const int totalSamples = fileBuffer.getNumSamples();
    if (totalSamples <= 0) return;

    const bool loopActive = loopEnabled.load(std::memory_order_acquire);
    const double loopStart = loopStartSec.load(std::memory_order_acquire);
    const double loopEnd = loopEndSec.load(std::memory_order_acquire);
    const double loopStartSamples = loopStart * fileSampleRate;
    const double loopEndSamples = loopEnd * fileSampleRate;
    const bool loopHasRange = loopActive && loopEndSamples > loopStartSamples + 1.0;
    bool loopDisarmed = loopDisarmedUntilBelowEnd.load(std::memory_order_acquire);

    const double seekSec = pendingSeekSec.exchange(-1.0);
    if (seekSec >= 0.0)
    {
        currentPosSamples = seekSec * fileSampleRate;
        if (loopActive && loopHasRange && currentPosSamples >= loopEndSamples)
        {
            loopDisarmedUntilBelowEnd.store(true, std::memory_order_release);
            loopDisarmed = true;
        }
        else
        {
            loopDisarmedUntilBelowEnd.store(false, std::memory_order_release);
            loopDisarmed = false;
        }
    }

    const double ratio = fileSampleRate / std::max(1.0, hostSampleRate);
    const int channels = std::min(2, outBuffer.getNumChannels());

    int loopWrapIndex = -1;

    for (int i = 0; i < numSamples; ++i)
    {
        if (loopActive)
        {
            if (loopDisarmed && loopHasRange && currentPosSamples < loopEndSamples)
            {
                loopDisarmed = false;
                loopDisarmedUntilBelowEnd.store(false, std::memory_order_release);
            }

            if (!loopDisarmed)
            {
                if (loopHasRange && currentPosSamples >= loopEndSamples)
                {
                    if (loopWrapIndex < 0)
                        loopWrapIndex = i;
                    currentPosSamples = loopStartSamples;
                }
                else if (currentPosSamples >= totalSamples)
                {
                    if (loopWrapIndex < 0)
                        loopWrapIndex = i;
                    currentPosSamples = loopHasRange ? loopStartSamples : 0.0;
                }
            }
            else
            {
                if (currentPosSamples >= totalSamples)
                    break;
            }
        }
        else if (currentPosSamples >= totalSamples)
        {
            break;
        }

        if (currentPosSamples >= totalSamples)
            break;

        const int idx = static_cast<int>(currentPosSamples);
        const float frac = static_cast<float>(currentPosSamples - idx);
        for (int ch = 0; ch < channels; ++ch)
        {
            const float a = fileBuffer.getSample(ch, idx);
            const float b = fileBuffer.getSample(ch, std::min(idx + 1, totalSamples - 1));
            const float v = a + (b - a) * frac;
            outBuffer.setSample(ch, i, v);
        }
        currentPosSamples += ratio;
    }

    applyLoopTransition(outBuffer, loopWrapIndex, numSamples);

    applyFades(outBuffer, numSamples);
}

void InMemoryPlaybackSource::applyFades(juce::AudioBuffer<float>& buffer, int numSamples) noexcept
{
    if (fadeInRemain <= 0 && fadeOutRemain <= 0) return;
    const int channels = buffer.getNumChannels();
    for (int i = 0; i < numSamples; ++i)
    {
        float g = 1.0f;
        if (fadeInRemain > 0)
        {
            const int progressed = fadeSamples - fadeInRemain;
            const float t = static_cast<float>(std::max(0, progressed)) / static_cast<float>(std::max(1, fadeSamples));
            g *= t; --fadeInRemain;
        }
        if (fadeOutRemain > 0)
        {
            const float t = static_cast<float>(fadeOutRemain) / static_cast<float>(std::max(1, fadeSamples));
            g *= t; --fadeOutRemain;
        }
        if (g != 1.0f)
        {
            for (int ch = 0; ch < channels; ++ch)
                buffer.getWritePointer(ch)[i] *= g;
        }
    }
}

void InMemoryPlaybackSource::applyLoopTransition(juce::AudioBuffer<float>& buffer,
                                                 int wrapSampleIndex,
                                                 int numSamples) noexcept
{
    if (loopFadeSamples <= 0)
        return;

    const int channels = buffer.getNumChannels();
    if (channels <= 0)
        return;

    const int fadeLength = loopFadeSamples;
    const int fadeInDenom = std::max(1, fadeLength - 1);

    auto applyFadeOut = [&](int endSample)
    {
        const int fadeOutSamples = std::min(fadeLength, endSample);
        if (fadeOutSamples <= 0)
            return;

        const int startSample = endSample - fadeOutSamples;
        for (int ch = 0; ch < channels; ++ch)
        {
            float* data = buffer.getWritePointer(ch);
            for (int i = 0; i < fadeOutSamples; ++i)
            {
                const int idx = startSample + i;
                if (idx < 0 || idx >= numSamples)
                    continue;
                const float t = fadeOutSamples == 1 ? 1.0f
                                                    : static_cast<float>(i) / static_cast<float>(std::max(1, fadeOutSamples - 1));
                data[idx] *= (1.0f - t);
            }
        }
    };

    auto applyFadeInSegment = [&](int startSample, int progressStart) -> int
    {
        const int remaining = fadeLength - progressStart;
        if (remaining <= 0)
            return 0;

        const int samplesToApply = std::min(remaining, numSamples - startSample);
        if (samplesToApply <= 0)
            return 0;

        for (int ch = 0; ch < channels; ++ch)
        {
            float* data = buffer.getWritePointer(ch);
            for (int i = 0; i < samplesToApply; ++i)
            {
                const int idx = startSample + i;
                if (idx < 0 || idx >= numSamples)
                    continue;
                const int progress = progressStart + i;
                const float t = (fadeLength == 1)
                                    ? 1.0f
                                    : static_cast<float>(progress) / static_cast<float>(fadeInDenom);
                data[idx] *= juce::jlimit(0.0f, 1.0f, t);
            }
        }
        return samplesToApply;
    };

    if (wrapSampleIndex >= 0 && wrapSampleIndex <= numSamples)
    {
        applyFadeOut(wrapSampleIndex);
        loopFadeInActive = true;
        loopFadeInProgress = 0;
        const int applied = applyFadeInSegment(wrapSampleIndex, loopFadeInProgress);
        loopFadeInProgress += applied;
        if (loopFadeInProgress >= fadeLength)
            loopFadeInActive = false;
    }
    else if (loopFadeInActive)
    {
        const int applied = applyFadeInSegment(0, loopFadeInProgress);
        loopFadeInProgress += applied;
        if (loopFadeInProgress >= fadeLength || applied == 0)
            loopFadeInActive = false;
    }
}

void InMemoryPlaybackSource::seek(double positionSeconds) noexcept
{
    pendingSeekSec.store(positionSeconds);
    fadeInRemain = fadeSamples;
    loopFadeInActive = false;
    loopFadeInProgress = 0;
}

void InMemoryPlaybackSource::setLoop(double startSec, double endSec, bool enabled) noexcept
{
    loopEnabled.store(enabled);
    loopStartSec.store(std::min(startSec, endSec));
    loopEndSec.store(std::max(startSec, endSec));
    if (!enabled)
    {
        loopFadeInActive = false;
        loopFadeInProgress = 0;
        loopDisarmedUntilBelowEnd.store(false, std::memory_order_release);
    }
}

double InMemoryPlaybackSource::getCurrentPositionSec() const noexcept
{
    return (fileSampleRate > 0.0) ? (currentPosSamples / fileSampleRate) : 0.0;
}

void InMemoryPlaybackSource::beginSwap() noexcept
{
    swapRequested.store(true);
    fadeOutRemain = fadeSamples;
}

void InMemoryPlaybackSource::endSwap() noexcept
{
    swapRequested.store(false);
    fadeOutRemain = 0;
}

} // namespace MixCompare
