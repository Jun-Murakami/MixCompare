// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include "../audio/MomentaryProcessor.h"

namespace MixCompare
{

class MeteringService
{
public:
    struct MeterValues
    {
        float rmsLeft{0.0f};
        float rmsRight{0.0f};
        float peakLeft{0.0f};
        float peakRight{0.0f};
        float truePeakLeft{0.0f};
        float truePeakRight{0.0f};
        float momentaryLKFS{-70.0f};
        float momentaryHoldLKFS{-70.0f};
    };

    enum class MeterSource
    {
        Host,
        Playlist,
        Output
    };

    MeteringService();
    ~MeteringService();

    void prepareToPlay(double sampleRate, int samplesPerBlock);
    void reset();

    void processBuffer(const juce::AudioBuffer<float>& buffer, MeterSource source);

    MeterValues getMeterValues(MeterSource source) const;
    float getRMSLevel(MeterSource source, int channel) const;
    float getPeakLevel(MeterSource source, int channel) const;
    float getTruePeakLevel(MeterSource source, int channel) const;
    float getTruePeakLevelAndReset(MeterSource source, int channel);
    /**
     * TruePeak の現在値を返しつつ、次回以降の表示に向けて減衰を適用する。
     * - 表示用途のホールド/ディケイ（約 60Hz 更新想定）をサービス側に集約するためのAPI
     * - decay は 0.0〜1.0 の係数。0.95 で約 20dB/sec 程度の減衰を想定
     */
    float getTruePeakLevelAndDecay(MeterSource source, int channel, float decay = 0.95f);

    void resetTruePeakMeters();
    void resetTruePeakMeters(MeterSource source);
    
    float getMomentaryLKFS(MeterSource source) const;
    float getMomentaryHoldLKFS(MeterSource source) const;
    void resetMomentaryHold();
    void resetMomentaryHold(MeterSource source);

    void setUpdateRate(float hz);
    float getUpdateRate() const { return updateRateHz; }

    bool shouldUpdateGUI() const;
    void markGUIUpdated();

    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void meteringUpdated(MeterSource source, const MeterValues& values) { juce::ignoreUnused(source, values); }
    };

    void addListener(Listener* listener);
    void removeListener(Listener* listener);

private:
    struct SourceMeters
    {
        std::atomic<float> rmsLeft{0.0f};
        std::atomic<float> rmsRight{0.0f};
        std::atomic<float> peakLeft{0.0f};
        std::atomic<float> peakRight{0.0f};
        std::atomic<float> truePeakLeft{0.0f};
        std::atomic<float> truePeakRight{0.0f};

        juce::LinearSmoothedValue<float> smoothedRmsLeft;
        juce::LinearSmoothedValue<float> smoothedRmsRight;
        juce::LinearSmoothedValue<float> smoothedPeakLeft;
        juce::LinearSmoothedValue<float> smoothedPeakRight;
        
        std::unique_ptr<MomentaryProcessor> momentaryProcessor;

        void reset()
        {
            rmsLeft = 0.0f;
            rmsRight = 0.0f;
            peakLeft = 0.0f;
            peakRight = 0.0f;
            truePeakLeft = 0.0f;
            truePeakRight = 0.0f;
            smoothedRmsLeft.reset(static_cast<int>(0.0f));
            smoothedRmsRight.reset(static_cast<int>(0.0f));
            smoothedPeakLeft.reset(static_cast<int>(0.0f));
            smoothedPeakRight.reset(static_cast<int>(0.0f));
            if (momentaryProcessor)
                momentaryProcessor->reset();
        }

        void resetTruePeak()
        {
            truePeakLeft = 0.0f;
            truePeakRight = 0.0f;
        }
        
        void resetMomentaryHold()
        {
            if (momentaryProcessor)
                momentaryProcessor->resetPeakHold();
        }
    };

    SourceMeters hostMeters;
    SourceMeters playlistMeters;
    SourceMeters outputMeters;

    double currentSampleRate{44100.0};
    int currentBlockSize{512};
    float updateRateHz{60.0f};

    juce::uint32 lastUpdateTime{0};
    juce::uint32 updateIntervalMs{16};

    juce::ListenerList<Listener> listeners;

    void accumulateTruePeak(std::atomic<float>& atomMax, float candidate) noexcept;
    void updateRMSAndPeak(SourceMeters& meters, const juce::AudioBuffer<float>& buffer);
    void updateTruePeak(SourceMeters& meters, const juce::AudioBuffer<float>& buffer);
    void updateMomentary(SourceMeters& meters, const juce::AudioBuffer<float>& buffer);
    
    SourceMeters& getMeters(MeterSource source);
    const SourceMeters& getMeters(MeterSource source) const;

    void notifyListeners(MeterSource source);

    static constexpr float MIN_DB = -60.0f;
    static constexpr float SMOOTHING_TIME_MS = 100.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MeteringService)
};

} // namespace MixCompare