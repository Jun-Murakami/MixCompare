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

