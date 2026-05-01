// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#pragma once
#include <cmath>

namespace mc_wasm
{

/// juce::LinearSmoothedValue 互換の線形補間スムーザー
class SmoothedValue
{
public:
    SmoothedValue(float initialValue = 0.0f)
        : currentValue(initialValue), targetValue(initialValue) {}

    void reset(double sampleRate, double rampLengthInSeconds)
    {
        stepsToTarget = static_cast<int>(sampleRate * rampLengthInSeconds);
        if (stepsToTarget <= 0) stepsToTarget = 1;
        countdown = 0;
        step = 0.0f;
    }

    void setCurrentAndTargetValue(float newValue)
    {
        currentValue = newValue;
        targetValue = newValue;
        countdown = 0;
        step = 0.0f;
    }

    void setTargetValue(float newTarget)
    {
        if (std::abs(newTarget - targetValue) < 1e-9f && countdown == 0)
            return;
        targetValue = newTarget;
        countdown = stepsToTarget;
        step = (targetValue - currentValue) / static_cast<float>(countdown);
    }

    float getNextValue()
    {
        if (countdown <= 0)
            return currentValue;
        currentValue += step;
        --countdown;
        if (countdown == 0)
            currentValue = targetValue;
        return currentValue;
    }

    float getCurrentValue() const { return currentValue; }
    float getTargetValue() const { return targetValue; }
    bool isSmoothing() const { return countdown > 0; }

private:
    float currentValue;
    float targetValue;
    float step = 0.0f;
    int stepsToTarget = 1;
    int countdown = 0;
};

} // namespace mc_wasm
