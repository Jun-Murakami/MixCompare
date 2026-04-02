#pragma once

#include <JuceHeader.h>

namespace mc3 {

/**
 * Monkey's Audio (.ape) デコーダー
 * MAC SDK をラップして JUCE の AudioFormat として提供
 * Windows / macOS 両対応
 */
class MonkeyAudioFormat : public juce::AudioFormat
{
public:
    MonkeyAudioFormat();
    ~MonkeyAudioFormat() override = default;

    juce::Array<int> getPossibleSampleRates() override;
    juce::Array<int> getPossibleBitDepths() override;
    bool canDoStereo() override { return true; }
    bool canDoMono() override   { return true; }
    bool isCompressed() override { return true; }

    juce::AudioFormatReader* createReaderFor (juce::InputStream* sourceStream,
                                              bool deleteStreamIfOpeningFails) override;

    std::unique_ptr<juce::AudioFormatWriter> createWriterFor (
                                              std::unique_ptr<juce::OutputStream>& streamToWriteTo,
                                              const juce::AudioFormatWriterOptions& options) override;

    juce::StringArray getQualityOptions() override { return {}; }

private:
    class MonkeyAudioReader;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MonkeyAudioFormat)
};

} // namespace mc3
