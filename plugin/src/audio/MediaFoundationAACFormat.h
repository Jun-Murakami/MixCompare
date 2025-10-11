#pragma once

#include <JuceHeader.h>

#if JUCE_WINDOWS
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <comdef.h>
#include <atlbase.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")

namespace mc3 {

/**
 * Windows Media Foundation AAC/M4A デコーダー
 * JUCEのAudioFormatを拡張してM4A/AACファイルの読み込みをサポート
 */
class MediaFoundationAACFormat : public juce::AudioFormat
{
public:
    MediaFoundationAACFormat();
    ~MediaFoundationAACFormat() override;
    
    juce::Array<int> getPossibleSampleRates() override;
    juce::Array<int> getPossibleBitDepths() override;
    bool canDoStereo() override { return true; }
    bool canDoMono() override { return true; }
    bool isCompressed() override { return true; }
    bool isChannelLayoutSupported (const juce::AudioChannelSet&) override { return true; }
    
    juce::AudioFormatReader* createReaderFor (juce::InputStream* sourceStream,
                                              bool deleteStreamIfOpeningFails) override;
    
    std::unique_ptr<juce::AudioFormatWriter> createWriterFor (
                                              std::unique_ptr<juce::OutputStream>& streamToWriteTo,
                                              const juce::AudioFormatWriterOptions& options) override;
    
    juce::StringArray getQualityOptions() override { return {}; }
    
    static bool isMediaFoundationAvailable();
    
private:
    class MediaFoundationAACReader;
    bool mediaFoundationInitialized = false;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MediaFoundationAACFormat)
};

} // namespace mc3

#endif // JUCE_WINDOWS