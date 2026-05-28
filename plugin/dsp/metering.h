// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#pragma once
#include <cmath>
#include <algorithm>
#include <memory>
#include "smoothed_value.h"
#include "momentary_processor.h"

namespace mc_wasm
{

/// メータリングサービス — RMS / Peak / TruePeak / LKFS
/// プラグイン版 MeteringService の JUCE-free 移植
class MeteringService
{
public:
    struct MeterValues
    {
        float rmsLeft = 0.0f;
        float rmsRight = 0.0f;
        float peakLeft = 0.0f;
        float peakRight = 0.0f;
        float truePeakLeft = 0.0f;
        float truePeakRight = 0.0f;
        float momentaryLKFS = -70.0f;
        float momentaryHoldLKFS = -70.0f;
    };

    enum class Source { Host, Playlist, Output };

    MeteringService()
    {
        hostMeters.momentary = std::make_unique<MomentaryProcessor>();
        playlistMeters.momentary = std::make_unique<MomentaryProcessor>();
        outputMeters.momentary = std::make_unique<MomentaryProcessor>();
    }

    void prepareToPlay(double sampleRate, int samplesPerBlock)
    {
        currentSampleRate = sampleRate;
        const double smoothSec = SMOOTHING_TIME_MS / 1000.0;

        auto prepSmooth = [&](SourceMeters& m) {
            m.smoothedRmsLeft.reset(sampleRate, smoothSec);
            m.smoothedRmsRight.reset(sampleRate, smoothSec);
            m.smoothedPeakLeft.reset(sampleRate, smoothSec);
            m.smoothedPeakRight.reset(sampleRate, smoothSec);
            m.smoothedRmsLeft.setCurrentAndTargetValue(0.0f);
            m.smoothedRmsRight.setCurrentAndTargetValue(0.0f);
            m.smoothedPeakLeft.setCurrentAndTargetValue(0.0f);
            m.smoothedPeakRight.setCurrentAndTargetValue(0.0f);
        };
        prepSmooth(hostMeters);
        prepSmooth(playlistMeters);
        prepSmooth(outputMeters);

        if (hostMeters.momentary)
            hostMeters.momentary->prepareToPlay(sampleRate, samplesPerBlock);
        if (playlistMeters.momentary)
            playlistMeters.momentary->prepareToPlay(sampleRate, samplesPerBlock);
        if (outputMeters.momentary)
            outputMeters.momentary->prepareToPlay(sampleRate, samplesPerBlock);
    }

    void reset()
    {
        hostMeters.reset();
        playlistMeters.reset();
        outputMeters.reset();
    }

    /// チャンネルごとのポインタ配列でバッファを処理
    void processBuffer(const float* const* channelData, int numChannels, int numSamples, Source source)
    {
        auto& meters = getMeters(source);
        updateRMSAndPeak(meters, channelData, numChannels, numSamples);
        updateTruePeak(meters, channelData, numChannels, numSamples);
        if (meters.momentary)
            meters.momentary->processBlock(channelData, numChannels, numSamples);
    }

    MeterValues getMeterValues(Source source) const
    {
        const auto& m = getMeters(source);
        MeterValues v;
        v.rmsLeft = m.rmsLeft;
        v.rmsRight = m.rmsRight;
        v.peakLeft = m.peakLeft;
        v.peakRight = m.peakRight;
        v.truePeakLeft = m.truePeakLeft;
        v.truePeakRight = m.truePeakRight;
        if (m.momentary)
        {
            v.momentaryLKFS = m.momentary->getMomentaryLKFS();
            v.momentaryHoldLKFS = m.momentary->getPeakHoldLKFS();
        }
        return v;
    }

    float getTruePeakLevelAndDecay(Source source, int channel, float decay = 0.95f)
    {
        auto& m = getMeters(source);
        float* target = (channel == 0) ? &m.truePeakLeft : &m.truePeakRight;
        float current = *target;
        *target = current * decay;
        return current;
    }

    void resetTruePeakMeters()
    {
        hostMeters.truePeakLeft = hostMeters.truePeakRight = 0.0f;
        playlistMeters.truePeakLeft = playlistMeters.truePeakRight = 0.0f;
        outputMeters.truePeakLeft = outputMeters.truePeakRight = 0.0f;
    }

    void resetTruePeakMeters(Source source)
    {
        auto& m = getMeters(source);
        m.truePeakLeft = m.truePeakRight = 0.0f;
    }

    void resetMomentaryHold()
    {
        if (hostMeters.momentary) hostMeters.momentary->resetPeakHold();
        if (playlistMeters.momentary) playlistMeters.momentary->resetPeakHold();
        if (outputMeters.momentary) outputMeters.momentary->resetPeakHold();
    }

    void resetMomentaryHold(Source source)
    {
        auto& m = getMeters(source);
        if (m.momentary) m.momentary->resetPeakHold();
    }

private:
    static constexpr float SMOOTHING_TIME_MS = 100.0f;

    struct SourceMeters
    {
        float rmsLeft = 0.0f, rmsRight = 0.0f;
        float peakLeft = 0.0f, peakRight = 0.0f;
        float truePeakLeft = 0.0f, truePeakRight = 0.0f;
        SmoothedValue smoothedRmsLeft, smoothedRmsRight;
        SmoothedValue smoothedPeakLeft, smoothedPeakRight;
        std::unique_ptr<MomentaryProcessor> momentary;

        void reset()
        {
            rmsLeft = rmsRight = peakLeft = peakRight = 0.0f;
            truePeakLeft = truePeakRight = 0.0f;
            smoothedRmsLeft.setCurrentAndTargetValue(0.0f);
            smoothedRmsRight.setCurrentAndTargetValue(0.0f);
            smoothedPeakLeft.setCurrentAndTargetValue(0.0f);
            smoothedPeakRight.setCurrentAndTargetValue(0.0f);
            if (momentary) momentary->reset();
        }
    };

    SourceMeters hostMeters;
    SourceMeters playlistMeters;
    SourceMeters outputMeters;
    double currentSampleRate = 44100.0;

    SourceMeters& getMeters(Source s)
    {
        if (s == Source::Host) return hostMeters;
        if (s == Source::Playlist) return playlistMeters;
        return outputMeters;
    }
    const SourceMeters& getMeters(Source s) const
    {
        if (s == Source::Host) return hostMeters;
        if (s == Source::Playlist) return playlistMeters;
        return outputMeters;
    }

    void updateRMSAndPeak(SourceMeters& m, const float* const* data, int numCh, int numSamples)
    {
        for (int ch = 0; ch < std::min(numCh, 2); ++ch)
        {
            float rms = 0.0f, peak = 0.0f;
            const float* d = data[ch];
            for (int i = 0; i < numSamples; ++i)
            {
                float s = d[i];
                rms += s * s;
                peak = std::max(peak, std::abs(s));
            }
            rms = std::sqrt(rms / static_cast<float>(numSamples));

            auto& sRms = (ch == 0) ? m.smoothedRmsLeft : m.smoothedRmsRight;
            auto& sPeak = (ch == 0) ? m.smoothedPeakLeft : m.smoothedPeakRight;
            sRms.setTargetValue(rms);
            sPeak.setTargetValue(peak);
            for (int i = 0; i < numSamples; ++i)
            {
                if (ch == 0) { m.rmsLeft = sRms.getNextValue(); m.peakLeft = sPeak.getNextValue(); }
                else { m.rmsRight = sRms.getNextValue(); m.peakRight = sPeak.getNextValue(); }
            }
        }
    }

    void updateTruePeak(SourceMeters& m, const float* const* data, int numCh, int numSamples)
    {
        for (int ch = 0; ch < std::min(numCh, 2); ++ch)
        {
            float& tp = (ch == 0) ? m.truePeakLeft : m.truePeakRight;
            const float* d = data[ch];
            for (int i = 0; i < numSamples; ++i)
                tp = std::max(tp, std::abs(d[i]));
        }
    }
};

} // namespace mc_wasm
