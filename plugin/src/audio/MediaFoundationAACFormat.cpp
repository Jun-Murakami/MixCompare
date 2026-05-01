// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#include "MediaFoundationAACFormat.h"

#if JUCE_WINDOWS

namespace mc3 {

//==============================================================================
class MediaFoundationAACFormat::MediaFoundationAACReader : public juce::AudioFormatReader
{
public:
    MediaFoundationAACReader (juce::InputStream* stream, const juce::String& formatName)
        : AudioFormatReader (stream, formatName), sourceStream (stream)
    {
        if (!initializeReader())
        {
            // 初期化失敗
            lengthInSamples = 0;
        }
    }
    
    ~MediaFoundationAACReader() override
    {
        if (sourceReader != nullptr)
        {
            sourceReader->Release();
            sourceReader = nullptr;
        }
    }
    
    bool readSamples (int* const* destSamples, int numDestChannels, int startOffsetInDestBuffer,
                     juce::int64 startSampleInFile, int numSamples) override
    {
        if (sourceReader == nullptr)
            return false;
        
        const juce::ScopedLock sl (readLock);
        
        // シーク処理
        if (startSampleInFile != lastReadPosition)
        {
            PROPVARIANT varPosition;
            PropVariantInit (&varPosition);
            varPosition.vt = VT_I8;
            varPosition.hVal.QuadPart = static_cast<LONGLONG>((startSampleInFile * 10000000LL) / sampleRate); // 100ナノ秒単位
            
            HRESULT hr = sourceReader->SetCurrentPosition (GUID_NULL, varPosition);
            PropVariantClear (&varPosition);
            
            if (FAILED (hr))
                return false;
            
            lastReadPosition = startSampleInFile;
        }
        
        // サンプル読み込み
        int samplesRead = 0;
        while (samplesRead < numSamples)
        {
            CComPtr<IMFSample> sample;
            DWORD streamFlags = 0;
            LONGLONG timestamp = 0;
            
            HRESULT hr = sourceReader->ReadSample (
                (DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                0,
                nullptr,
                &streamFlags,
                &timestamp,
                &sample
            );
            
            if (FAILED (hr) || (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM))
                break;
            
            if (sample == nullptr)
                continue;
            
            // バッファーからオーディオデータを取得
            CComPtr<IMFMediaBuffer> buffer;
            hr = sample->ConvertToContiguousBuffer (&buffer);
            if (FAILED (hr))
                break;
            
            BYTE* audioData = nullptr;
            DWORD bufferLength = 0;
            hr = buffer->Lock (&audioData, nullptr, &bufferLength);
            if (FAILED (hr))
                break;
            
            // float データをコピー
            const int samplesInBuffer = static_cast<int>(bufferLength / (sizeof(float) * numChannels));
            const int samplesToCopy = juce::jmin (samplesInBuffer, numSamples - samplesRead);
            const float* sourceData = reinterpret_cast<const float*> (audioData);
            
            for (int ch = 0; ch < numDestChannels; ++ch)
            {
                if (ch < static_cast<int>(numChannels))
                {
                    float* dest = reinterpret_cast<float*> (destSamples[ch]) + startOffsetInDestBuffer + samplesRead;
                    for (int i = 0; i < samplesToCopy; ++i)
                    {
                        dest[i] = sourceData[i * numChannels + ch];
                    }
                }
            }
            
            buffer->Unlock();
            samplesRead += samplesToCopy;
            lastReadPosition += samplesToCopy;
        }
        
        // 残りをゼロで埋める
        if (samplesRead < numSamples)
        {
            for (int ch = 0; ch < numDestChannels; ++ch)
            {
                float* dest = reinterpret_cast<float*> (destSamples[ch]) + startOffsetInDestBuffer + samplesRead;
                for (int i = 0; i < numSamples - samplesRead; ++i)
                {
                    dest[i] = 0.0f;
                }
            }
        }
        
        return samplesRead > 0;
    }
    
private:
    bool initializeReader()
    {
        // InputStreamをIStreamに変換
        juce::MemoryBlock mb;
        sourceStream->readIntoMemoryBlock (mb);
        
        if (mb.isEmpty())
            return false;
        
        // IStreamを作成
        CComPtr<IStream> stream = SHCreateMemStream (
            static_cast<const BYTE*> (mb.getData()),
            static_cast<UINT> (mb.getSize())
        );
        
        if (stream == nullptr)
            return false;
        
        // SourceReaderを作成
        CComPtr<IMFByteStream> byteStream;
        HRESULT hr = MFCreateMFByteStreamOnStream (stream, &byteStream);
        if (FAILED (hr))
            return false;
        
        // 属性を設定
        CComPtr<IMFAttributes> attributes;
        hr = MFCreateAttributes (&attributes, 1);
        if (FAILED (hr))
            return false;
        
        hr = attributes->SetUINT32 (MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        if (FAILED (hr))
            return false;
        
        // SourceReaderを作成
        hr = MFCreateSourceReaderFromByteStream (
            byteStream,
            attributes,
            &sourceReader
        );
        
        if (FAILED (hr))
            return false;
        
        // オーディオストリームを選択
        hr = sourceReader->SetStreamSelection ((DWORD) MF_SOURCE_READER_ALL_STREAMS, FALSE);
        if (FAILED (hr))
            return false;
        
        hr = sourceReader->SetStreamSelection ((DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
        if (FAILED (hr))
            return false;
        
        // 出力形式をfloat PCMに設定
        CComPtr<IMFMediaType> outputType;
        hr = MFCreateMediaType (&outputType);
        if (FAILED (hr))
            return false;
        
        hr = outputType->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (FAILED (hr))
            return false;
        
        hr = outputType->SetGUID (MF_MT_SUBTYPE, MFAudioFormat_Float);
        if (FAILED (hr))
            return false;
        
        hr = sourceReader->SetCurrentMediaType ((DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, outputType);
        if (FAILED (hr))
            return false;
        
        // 実際のメディアタイプを取得
        CComPtr<IMFMediaType> actualType;
        hr = sourceReader->GetCurrentMediaType ((DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actualType);
        if (FAILED (hr))
            return false;
        
        // サンプルレートを取得
        UINT32 sampleRateInt = 0;
        hr = actualType->GetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRateInt);
        if (FAILED (hr))
            return false;
        
        sampleRate = static_cast<double> (sampleRateInt);
        
        // チャンネル数を取得
        UINT32 channelCount = 0;
        hr = actualType->GetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, &channelCount);
        if (FAILED (hr))
            return false;
        
        numChannels = static_cast<unsigned int> (channelCount);
        
        // ビット深度（floatなので32ビット）
        bitsPerSample = 32;
        usesFloatingPointData = true;
        
        // 長さを取得
        PROPVARIANT var;
        PropVariantInit (&var);
        hr = sourceReader->GetPresentationAttribute ((DWORD) MF_SOURCE_READER_MEDIASOURCE,
                                                     MF_PD_DURATION, &var);
        
        if (SUCCEEDED (hr) && var.vt == VT_UI8)
        {
            // 100ナノ秒単位からサンプル数に変換
            lengthInSamples = static_cast<juce::int64>((static_cast<double>(var.uhVal.QuadPart) * sampleRate) / 10000000.0);
        }
        else
        {
            lengthInSamples = 0;
        }
        
        PropVariantClear (&var);
        
        return true;
    }
    
    juce::InputStream* sourceStream;
    IMFSourceReader* sourceReader = nullptr;
    juce::int64 lastReadPosition = 0;
    juce::CriticalSection readLock;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MediaFoundationAACReader)
};

//==============================================================================
MediaFoundationAACFormat::MediaFoundationAACFormat()
    : AudioFormat ("Media Foundation AAC", ".m4a .aac .mp4")
{
    // Media Foundation を初期化
    HRESULT hr = MFStartup (MF_VERSION);
    mediaFoundationInitialized = SUCCEEDED (hr);
}

MediaFoundationAACFormat::~MediaFoundationAACFormat()
{
    if (mediaFoundationInitialized)
    {
        MFShutdown();
    }
}

juce::Array<int> MediaFoundationAACFormat::getPossibleSampleRates()
{
    return { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000 };
}

juce::Array<int> MediaFoundationAACFormat::getPossibleBitDepths()
{
    return { 16, 24, 32 };
}

juce::AudioFormatReader* MediaFoundationAACFormat::createReaderFor (juce::InputStream* sourceStream,
                                                                    bool deleteStreamIfOpeningFails)
{
    if (!mediaFoundationInitialized)
    {
        if (deleteStreamIfOpeningFails)
            delete sourceStream;
        return nullptr;
    }
    
    std::unique_ptr<MediaFoundationAACReader> reader (new MediaFoundationAACReader (sourceStream, getFormatName()));
    
    if (reader->lengthInSamples > 0)
        return reader.release();
    
    if (deleteStreamIfOpeningFails)
        delete sourceStream;
    
    return nullptr;
}

std::unique_ptr<juce::AudioFormatWriter> MediaFoundationAACFormat::createWriterFor (
                                                                    std::unique_ptr<juce::OutputStream>& /*streamToWriteTo*/,
                                                                    const juce::AudioFormatWriterOptions& /*options*/)
{
    // 書き込みは現在サポートしていません
    return nullptr;
}

bool MediaFoundationAACFormat::isMediaFoundationAvailable()
{
    HRESULT hr = MFStartup (MF_VERSION);
    if (SUCCEEDED (hr))
    {
        MFShutdown();
        return true;
    }
    return false;
}

} // namespace mc3

#endif // JUCE_WINDOWS