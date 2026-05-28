// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#pragma once
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <cmath>

namespace mc_wasm
{

/// Vadim Zavalishin の Topology-Preserving Transform (TPT) State Variable Filter
/// juce::dsp::StateVariableTPTFilter 互換のローパスフィルタ
class StateVariableTPTFilter
{
public:
    void prepare(double sampleRate)
    {
        this->sampleRate = sampleRate;
        updateCoefficients();
        reset();
    }

    void reset()
    {
        s1 = 0.0f;
        s2 = 0.0f;
    }

    void setCutoffFrequency(float freqHz)
    {
        if (std::abs(freqHz - cutoffFreq) < 0.01f)
            return;
        cutoffFreq = freqHz;
        updateCoefficients();
    }

    /// ローパス出力を返す
    float processSample(float input)
    {
        // TPT SVF: g = tan(pi * fc / fs)
        // v1 = (input - 2*s2 - s1 * (1 + g)) * 1 / (1 + g*(g + 1/Q))
        // ただし Q=1/sqrt(2) (Butterworth) → 1/Q = sqrt(2)
        const float hp = (input - (oneOverQ + g) * s1 - s2) * h;
        const float bp = g * hp + s1;
        const float lp = g * bp + s2;

        s1 = g * hp + bp;
        s2 = g * bp + lp;

        return lp;
    }

private:
    void updateCoefficients()
    {
        g = static_cast<float>(std::tan(M_PI * static_cast<double>(cutoffFreq) / sampleRate));
        // Butterworth Q = 1/sqrt(2) → 1/Q = sqrt(2) ≈ 1.4142
        oneOverQ = 1.4142135623730951f;
        h = 1.0f / (1.0f + oneOverQ * g + g * g);
    }

    double sampleRate = 44100.0;
    float cutoffFreq = 20000.0f;
    float g = 0.0f;
    float oneOverQ = 1.4142135623730951f;
    float h = 0.0f;

    // フィルタ状態
    float s1 = 0.0f;
    float s2 = 0.0f;
};

} // namespace mc_wasm
