// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#include "MonkeyAudioFormat.h"

// Monkey's Audio SDK
#include "All.h"
#include "MACLib.h"
#include "IAPEIO.h"

namespace mc3 {

//==============================================================================
// JUCE InputStream を Monkey's Audio SDK の IAPEIO にアダプトする
//==============================================================================
class JuceStreamAPEIO : public APE::IAPEIO
{
public:
    explicit JuceStreamAPEIO (juce::InputStream* stream)
        : inputStream (stream),
          streamSize (stream != nullptr ? stream->getTotalLength() : 0)
    {
    }

    ~JuceStreamAPEIO() override = default;

    // Open / Close — ストリームは既に開いているため何もしない
    int Open (const APE::str_utfn*, bool) override { return ERROR_SUCCESS; }
    int Close() override { return ERROR_SUCCESS; }

    int Read (void* pBuffer, unsigned int nBytesToRead, unsigned int* pBytesRead) override
    {
        if (inputStream == nullptr)
            return ERROR_IO_READ;

        const int bytesRead = inputStream->read (pBuffer, static_cast<int> (nBytesToRead));
        if (pBytesRead != nullptr)
            *pBytesRead = static_cast<unsigned int> (bytesRead);

        return ERROR_SUCCESS;
    }

    int Write (const void*, unsigned int, unsigned int*) override { return ERROR_IO_WRITE; }

    int Seek (APE::int64 nPosition, APE::SeekMethod nMethod) override
    {
        if (inputStream == nullptr)
            return ERROR_IO_READ;

        APE::int64 targetPos = 0;
        switch (nMethod)
        {
            case APE::SeekFileBegin:   targetPos = nPosition; break;
            case APE::SeekFileCurrent: targetPos = inputStream->getPosition() + nPosition; break;
            case APE::SeekFileEnd:     targetPos = streamSize + nPosition; break;
            default: return ERROR_IO_READ;
        }

        if (inputStream->setPosition (targetPos))
            return ERROR_SUCCESS;

        return ERROR_IO_READ;
    }

    int Create (const APE::str_utfn*) override { return ERROR_IO_WRITE; }
    int Delete() override                      { return ERROR_IO_WRITE; }
    int SetEOF() override                      { return ERROR_IO_WRITE; }

    unsigned char* GetBuffer (int*) override { return nullptr; }

    APE::int64 GetPosition() override
    {
        return inputStream != nullptr ? inputStream->getPosition() : 0;
    }

    APE::int64 GetSize() override
    {
        return streamSize;
    }

private:
    juce::InputStream* inputStream;
    APE::int64 streamSize;
};

//==============================================================================
// AudioFormatReader 実装
//==============================================================================
class MonkeyAudioFormat::MonkeyAudioReader : public juce::AudioFormatReader
{
public:
    MonkeyAudioReader (juce::InputStream* stream, const juce::String& formatName)
        : AudioFormatReader (stream, formatName)
    {
        apeIO = std::make_unique<JuceStreamAPEIO> (stream);

        int errorCode = 0;
        decompressor.reset (CreateIAPEDecompressEx (apeIO.get(), &errorCode));

        if (decompressor == nullptr || errorCode != ERROR_SUCCESS)
        {
            decompressor.reset();
            return;
        }

        // ファイル情報を取得
        sampleRate    = static_cast<double> (decompressor->GetInfo (APE::IAPEDecompress::APE_INFO_SAMPLE_RATE));
        bitsPerSample = static_cast<unsigned int> (decompressor->GetInfo (APE::IAPEDecompress::APE_INFO_BITS_PER_SAMPLE));
        numChannels   = static_cast<unsigned int> (decompressor->GetInfo (APE::IAPEDecompress::APE_INFO_CHANNELS));
        lengthInSamples = decompressor->GetInfo (APE::IAPEDecompress::APE_DECOMPRESS_TOTAL_BLOCKS);
        usesFloatingPointData = false;

        bytesPerSample = static_cast<int> (decompressor->GetInfo (APE::IAPEDecompress::APE_INFO_BYTES_PER_SAMPLE));
        blockAlign     = static_cast<int> (decompressor->GetInfo (APE::IAPEDecompress::APE_INFO_BLOCK_ALIGN));
    }

    ~MonkeyAudioReader() override
    {
        // decompressor を先に解放（apeIO を参照しているため）
        decompressor.reset();
        apeIO.reset();
    }

    bool readSamples (int* const* destSamples, int numDestChannels, int startOffsetInDestBuffer,
                      juce::int64 startSampleInFile, int numSamples) override
    {
        if (decompressor == nullptr)
            return false;

        const juce::ScopedLock sl (readLock);

        // シーク
        const auto currentBlock = decompressor->GetInfo (APE::IAPEDecompress::APE_DECOMPRESS_CURRENT_BLOCK);
        if (currentBlock != startSampleInFile)
        {
            if (decompressor->Seek (startSampleInFile) != ERROR_SUCCESS)
                return false;
        }

        // デコード用一時バッファ（インターリーブ形式）
        const int bytesPerBlock = blockAlign; // 全チャンネル × bytesPerSample
        decodeBuffer.resize (static_cast<size_t> (numSamples) * static_cast<size_t> (bytesPerBlock));

        APE::int64 blocksRetrieved = 0;
        const int result = decompressor->GetData (
            decodeBuffer.data(),
            static_cast<APE::int64> (numSamples),
            &blocksRetrieved,
            nullptr  // デフォルト処理
        );

        if (result != ERROR_SUCCESS && blocksRetrieved == 0)
            return false;

        // デインターリーブして JUCE の 32-bit int（左詰め）形式に変換
        const int nCh = static_cast<int> (numChannels);
        const int retrieved = static_cast<int> (blocksRetrieved);

        for (int ch = 0; ch < numDestChannels; ++ch)
        {
            if (destSamples[ch] == nullptr)
                continue;

            int* dest = destSamples[ch] + startOffsetInDestBuffer;
            const int srcCh = (ch < nCh) ? ch : (nCh - 1); // モノラル→ステレオ拡張

            if (bitsPerSample == 16)
            {
                const auto* src = reinterpret_cast<const int16_t*> (decodeBuffer.data());
                for (int i = 0; i < retrieved; ++i)
                    dest[i] = static_cast<int> (src[i * nCh + srcCh]) << 16;
            }
            else if (bitsPerSample == 24)
            {
                const auto* src = decodeBuffer.data();
                for (int i = 0; i < retrieved; ++i)
                {
                    const int byteOffset = (i * nCh + srcCh) * 3;
                    const int val = (static_cast<int> (static_cast<int8_t> (src[byteOffset + 2])) << 16)
                                  | (static_cast<int> (src[byteOffset + 1]) << 8)
                                  | (static_cast<int> (src[byteOffset]));
                    dest[i] = val << 8;
                }
            }
            else if (bitsPerSample == 8)
            {
                const auto* src = decodeBuffer.data();
                for (int i = 0; i < retrieved; ++i)
                    dest[i] = (static_cast<int> (src[i * nCh + srcCh]) - 128) << 24;
            }
            else if (bitsPerSample == 32)
            {
                const auto* src = reinterpret_cast<const int32_t*> (decodeBuffer.data());
                for (int i = 0; i < retrieved; ++i)
                    dest[i] = src[i * nCh + srcCh];
            }

            // 残りをゼロで埋める
            for (int i = retrieved; i < numSamples; ++i)
                dest[i] = 0;
        }

        return retrieved > 0;
    }

private:
    struct APEDecompressorDeleter
    {
        void operator() (APE::IAPEDecompress* p) const { delete p; }
    };

    std::unique_ptr<JuceStreamAPEIO> apeIO;
    std::unique_ptr<APE::IAPEDecompress, APEDecompressorDeleter> decompressor;
    std::vector<unsigned char> decodeBuffer;
    int bytesPerSample = 0;
    int blockAlign = 0;
    juce::CriticalSection readLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MonkeyAudioReader)
};

//==============================================================================
// MonkeyAudioFormat 実装
//==============================================================================
MonkeyAudioFormat::MonkeyAudioFormat()
    : AudioFormat ("Monkey's Audio", ".ape")
{
}

juce::Array<int> MonkeyAudioFormat::getPossibleSampleRates()
{
    return { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000, 176400, 192000 };
}

juce::Array<int> MonkeyAudioFormat::getPossibleBitDepths()
{
    return { 8, 16, 24, 32 };
}

juce::AudioFormatReader* MonkeyAudioFormat::createReaderFor (juce::InputStream* sourceStream,
                                                              bool deleteStreamIfOpeningFails)
{
    auto reader = std::make_unique<MonkeyAudioReader> (sourceStream, getFormatName());

    if (reader->lengthInSamples > 0)
        return reader.release();

    // 失敗時、ストリームの所有権を返す（JUCE の規約に従う）
    if (! deleteStreamIfOpeningFails)
        reader->input = nullptr;

    return nullptr;
}

std::unique_ptr<juce::AudioFormatWriter> MonkeyAudioFormat::createWriterFor (
                                                              std::unique_ptr<juce::OutputStream>&,
                                                              const juce::AudioFormatWriterOptions&)
{
    // エンコードは現在サポートしない
    return nullptr;
}

} // namespace mc3
