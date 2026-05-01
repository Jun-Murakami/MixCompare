// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#pragma once

#include <JuceHeader.h>
#include <atomic>

namespace mc3
{

class CrashHandler final
{
public:
    static void install();
    static juce::File getDumpDirectory();

private:
    CrashHandler() = delete;
    ~CrashHandler() = delete;
};

} // namespace mc3

