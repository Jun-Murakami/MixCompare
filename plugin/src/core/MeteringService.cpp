#include "MeteringService.h"
#include <cmath>

namespace MixCompare
{

MeteringService::MeteringService()
{
    updateIntervalMs = static_cast<juce::uint32>(1000.0f / updateRateHz);
    
    // Momentaryプロセッサを初期化
    hostMeters.momentaryProcessor = std::make_unique<MomentaryProcessor>();
    playlistMeters.momentaryProcessor = std::make_unique<MomentaryProcessor>();
    outputMeters.momentaryProcessor = std::make_unique<MomentaryProcessor>();
}

MeteringService::~MeteringService() = default;

void MeteringService::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentBlockSize = samplesPerBlock;

    const double smoothingTimeSeconds = SMOOTHING_TIME_MS / 1000.0;
    const int smoothingSamples = static_cast<int>(sampleRate * smoothingTimeSeconds);
    juce::ignoreUnused(smoothingSamples);

    auto prepareSmoothing = [&](SourceMeters& meters)
    {
        meters.smoothedRmsLeft.reset(sampleRate, smoothingTimeSeconds);
        meters.smoothedRmsRight.reset(sampleRate, smoothingTimeSeconds);
        meters.smoothedPeakLeft.reset(sampleRate, smoothingTimeSeconds);
        meters.smoothedPeakRight.reset(sampleRate, smoothingTimeSeconds);
        
        meters.smoothedRmsLeft.setCurrentAndTargetValue(0.0f);
        meters.smoothedRmsRight.setCurrentAndTargetValue(0.0f);
        meters.smoothedPeakLeft.setCurrentAndTargetValue(0.0f);
        meters.smoothedPeakRight.setCurrentAndTargetValue(0.0f);
    };

    prepareSmoothing(hostMeters);
    prepareSmoothing(playlistMeters);
    prepareSmoothing(outputMeters);
    
    // Momentaryプロセッサも準備
    if (hostMeters.momentaryProcessor)
        hostMeters.momentaryProcessor->prepareToPlay(sampleRate, samplesPerBlock);
    if (playlistMeters.momentaryProcessor)
        playlistMeters.momentaryProcessor->prepareToPlay(sampleRate, samplesPerBlock);
    if (outputMeters.momentaryProcessor)
        outputMeters.momentaryProcessor->prepareToPlay(sampleRate, samplesPerBlock);
}

void MeteringService::reset()
{
    hostMeters.reset();
    playlistMeters.reset();
    outputMeters.reset();
}

void MeteringService::processBuffer(const juce::AudioBuffer<float>& buffer, MeterSource source)
{
    auto& meters = getMeters(source);
    
    updateRMSAndPeak(meters, buffer);
    updateTruePeak(meters, buffer);
    updateMomentary(meters, buffer);
}

MeteringService::MeterValues MeteringService::getMeterValues(MeterSource source) const
{
    const auto& meters = getMeters(source);
    
    MeterValues values;
    values.rmsLeft = meters.rmsLeft.load();
    values.rmsRight = meters.rmsRight.load();
    values.peakLeft = meters.peakLeft.load();
    values.peakRight = meters.peakRight.load();
    values.truePeakLeft = meters.truePeakLeft.load();
    values.truePeakRight = meters.truePeakRight.load();
    
    if (meters.momentaryProcessor)
    {
        values.momentaryLKFS = meters.momentaryProcessor->getMomentaryLKFS();
        values.momentaryHoldLKFS = meters.momentaryProcessor->getPeakHoldLKFS();
    }
    
    return values;
}

float MeteringService::getRMSLevel(MeterSource source, int channel) const
{
    const auto& meters = getMeters(source);
    
    if (channel == 0)
        return meters.rmsLeft.load();
    else if (channel == 1)
        return meters.rmsRight.load();
    
    return 0.0f;
}

float MeteringService::getPeakLevel(MeterSource source, int channel) const
{
    const auto& meters = getMeters(source);
    
    if (channel == 0)
        return meters.peakLeft.load();
    else if (channel == 1)
        return meters.peakRight.load();
    
    return 0.0f;
}

float MeteringService::getTruePeakLevel(MeterSource source, int channel) const
{
    const auto& meters = getMeters(source);
    
    if (channel == 0)
        return meters.truePeakLeft.load();
    else if (channel == 1)
        return meters.truePeakRight.load();
    
    return 0.0f;
}

float MeteringService::getTruePeakLevelAndReset(MeterSource source, int channel)
{
    auto& meters = getMeters(source);
    
    if (channel == 0)
        return meters.truePeakLeft.exchange(0.0f);
    else if (channel == 1)
        return meters.truePeakRight.exchange(0.0f);
    
    return 0.0f;
}

float MeteringService::getTruePeakLevelAndDecay(MeterSource source, int channel, float decay)
{
    auto& meters = getMeters(source);
    std::atomic<float>* target = nullptr;
    if (channel == 0)
        target = &meters.truePeakLeft;
    else if (channel == 1)
        target = &meters.truePeakRight;
    else
        return 0.0f;

    // 現在値を読み取り、減衰後の値をCASで書き戻す
    float current = target->load(std::memory_order_relaxed);
    float decayed = current * decay;
    while (!target->compare_exchange_weak(current, decayed, std::memory_order_relaxed, std::memory_order_relaxed))
    {
        decayed = current * decay;
    }
    return current; // 減衰前の値を返す（表示用）
}

void MeteringService::resetTruePeakMeters()
{
    hostMeters.resetTruePeak();
    playlistMeters.resetTruePeak();
    outputMeters.resetTruePeak();
}

void MeteringService::resetTruePeakMeters(MeterSource source)
{
    getMeters(source).resetTruePeak();
}

void MeteringService::setUpdateRate(float hz)
{
    updateRateHz = juce::jlimit(1.0f, 120.0f, hz);
    updateIntervalMs = static_cast<juce::uint32>(1000.0f / updateRateHz);
}

bool MeteringService::shouldUpdateGUI() const
{
    const auto now = juce::Time::getMillisecondCounter();
    return (now - lastUpdateTime) >= updateIntervalMs;
}

void MeteringService::markGUIUpdated()
{
    lastUpdateTime = juce::Time::getMillisecondCounter();
}

void MeteringService::addListener(Listener* listener)
{
    listeners.add(listener);
}

void MeteringService::removeListener(Listener* listener)
{
    listeners.remove(listener);
}

void MeteringService::accumulateTruePeak(std::atomic<float>& atomMax, float candidate) noexcept
{
    const float absValue = std::abs(candidate);
    float prev = atomMax.load(std::memory_order_relaxed);
    
    while (absValue > prev)
    {
        if (atomMax.compare_exchange_weak(prev, absValue,
                                          std::memory_order_relaxed,
                                          std::memory_order_relaxed))
        {
            break;
        }
    }
}

void MeteringService::updateRMSAndPeak(SourceMeters& meters, const juce::AudioBuffer<float>& buffer)
{
    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    if (numChannels >= 1 && numSamples > 0)
    {
        float rms = 0.0f;
        float peak = 0.0f;
        const float* channelData = buffer.getReadPointer(0);
        
        for (int i = 0; i < numSamples; ++i)
        {
            const float sample = channelData[i];
            const float absSample = std::abs(sample);
            rms += sample * sample;
            peak = std::max(peak, absSample);
        }
        
        rms = std::sqrt(rms / static_cast<float>(numSamples));
        
        meters.smoothedRmsLeft.setTargetValue(rms);
        meters.smoothedPeakLeft.setTargetValue(peak);
        
        // Skip to the end of this block for proper smoothing
        for (int i = 0; i < numSamples; ++i)
        {
            meters.rmsLeft = meters.smoothedRmsLeft.getNextValue();
            meters.peakLeft = meters.smoothedPeakLeft.getNextValue();
        }
    }
    
    if (numChannels >= 2 && numSamples > 0)
    {
        float rms = 0.0f;
        float peak = 0.0f;
        const float* channelData = buffer.getReadPointer(1);
        
        for (int i = 0; i < numSamples; ++i)
        {
            const float sample = channelData[i];
            const float absSample = std::abs(sample);
            rms += sample * sample;
            peak = std::max(peak, absSample);
        }
        
        rms = std::sqrt(rms / static_cast<float>(numSamples));
        
        meters.smoothedRmsRight.setTargetValue(rms);
        meters.smoothedPeakRight.setTargetValue(peak);
        
        // Skip to the end of this block for proper smoothing
        for (int i = 0; i < numSamples; ++i)
        {
            meters.rmsRight = meters.smoothedRmsRight.getNextValue();
            meters.peakRight = meters.smoothedPeakRight.getNextValue();
        }
    }
}

void MeteringService::updateTruePeak(SourceMeters& meters, const juce::AudioBuffer<float>& buffer)
{
    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    if (numChannels >= 1 && numSamples > 0)
    {
        const float* channelData = buffer.getReadPointer(0);
        for (int i = 0; i < numSamples; ++i)
        {
            accumulateTruePeak(meters.truePeakLeft, channelData[i]);
        }
    }
    
    if (numChannels >= 2 && numSamples > 0)
    {
        const float* channelData = buffer.getReadPointer(1);
        for (int i = 0; i < numSamples; ++i)
        {
            accumulateTruePeak(meters.truePeakRight, channelData[i]);
        }
    }
}

MeteringService::SourceMeters& MeteringService::getMeters(MeterSource source)
{
    switch (source)
    {
        case MeterSource::Host:
            return hostMeters;
        case MeterSource::Playlist:
            return playlistMeters;
        case MeterSource::Output:
        default:
            return outputMeters;
    }
}

const MeteringService::SourceMeters& MeteringService::getMeters(MeterSource source) const
{
    switch (source)
    {
        case MeterSource::Host:
            return hostMeters;
        case MeterSource::Playlist:
            return playlistMeters;
        case MeterSource::Output:
        default:
            return outputMeters;
    }
}

void MeteringService::notifyListeners(MeterSource source)
{
    const auto values = getMeterValues(source);
    listeners.call(&Listener::meteringUpdated, source, values);
}

void MeteringService::updateMomentary(SourceMeters& meters, const juce::AudioBuffer<float>& buffer)
{
    if (meters.momentaryProcessor)
    {
        meters.momentaryProcessor->processBlock(buffer);
    }
}

float MeteringService::getMomentaryLKFS(MeterSource source) const
{
    const auto& meters = getMeters(source);
    if (meters.momentaryProcessor)
        return meters.momentaryProcessor->getMomentaryLKFS();
    return -70.0f;
}

float MeteringService::getMomentaryHoldLKFS(MeterSource source) const
{
    const auto& meters = getMeters(source);
    if (meters.momentaryProcessor)
        return meters.momentaryProcessor->getPeakHoldLKFS();
    return -70.0f;
}

void MeteringService::resetMomentaryHold()
{
    hostMeters.resetMomentaryHold();
    playlistMeters.resetMomentaryHold();
    outputMeters.resetMomentaryHold();
}

void MeteringService::resetMomentaryHold(MeterSource source)
{
    getMeters(source).resetMomentaryHold();
}

} // namespace MixCompare