// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#include "FFmpegAACFormat.h"

#if JUCE_LINUX

//==============================================================================
// MIXCOMPARE_HAVE_FFMPEG はビルド時に FFmpeg dev ヘッダが見つかった場合のみ CMake が定義する。
// 未定義時は「常に利用不可」のスタブを提供し、ビルドを通す（実行時に AAC が無効になるだけ）。
//==============================================================================
#if ! defined (MIXCOMPARE_HAVE_FFMPEG)

namespace mc3 {

FFmpegAACFormat::FFmpegAACFormat()  : AudioFormat ("FFmpeg AAC", ".m4a .aac .mp4 .m4b") {}
FFmpegAACFormat::~FFmpegAACFormat() = default;

juce::Array<int> FFmpegAACFormat::getPossibleSampleRates() { return {}; }
juce::Array<int> FFmpegAACFormat::getPossibleBitDepths()   { return {}; }

juce::AudioFormatReader* FFmpegAACFormat::createReaderFor (juce::InputStream* s, bool deleteStreamIfOpeningFails)
{
    if (deleteStreamIfOpeningFails)
        delete s;
    return nullptr;
}

std::unique_ptr<juce::AudioFormatWriter> FFmpegAACFormat::createWriterFor (
    std::unique_ptr<juce::OutputStream>&, const juce::AudioFormatWriterOptions&)
{
    return nullptr;
}

bool FFmpegAACFormat::isFFmpegAvailable() { return false; }

} // namespace mc3

#else // MIXCOMPARE_HAVE_FFMPEG

#include <dlfcn.h>
#include <cstdint>
#include <cstdio> // SEEK_SET / SEEK_CUR / SEEK_END
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
}

namespace mc3 {
namespace {

//==============================================================================
// FFmpeg 共有ライブラリの実行時ローダー（プロセス内で一度だけ初期化）。
// ヘッダのメジャーバージョンと一致する versioned soname を dlopen し、必要シンボルを解決する。
//==============================================================================
struct FFmpegLib
{
    bool ok = false;

    // libavutil
    decltype (&::av_frame_alloc)               av_frame_alloc = nullptr;
    decltype (&::av_frame_free)                av_frame_free = nullptr;
    decltype (&::av_frame_unref)               av_frame_unref = nullptr;
    decltype (&::av_freep)                     av_freep = nullptr;
    decltype (&::av_malloc)                    av_malloc = nullptr;
    decltype (&::av_rescale_q)                 av_rescale_q = nullptr;
    decltype (&::av_channel_layout_default)    av_channel_layout_default = nullptr;
    decltype (&::av_channel_layout_uninit)     av_channel_layout_uninit = nullptr;

    // libswresample
    decltype (&::swr_alloc_set_opts2)          swr_alloc_set_opts2 = nullptr;
    decltype (&::swr_init)                     swr_init = nullptr;
    decltype (&::swr_convert)                  swr_convert = nullptr;
    decltype (&::swr_free)                     swr_free = nullptr;

    // libavcodec
    decltype (&::avcodec_find_decoder)         avcodec_find_decoder = nullptr;
    decltype (&::avcodec_alloc_context3)       avcodec_alloc_context3 = nullptr;
    decltype (&::avcodec_parameters_to_context) avcodec_parameters_to_context = nullptr;
    decltype (&::avcodec_open2)                avcodec_open2 = nullptr;
    decltype (&::avcodec_free_context)         avcodec_free_context = nullptr;
    decltype (&::avcodec_send_packet)          avcodec_send_packet = nullptr;
    decltype (&::avcodec_receive_frame)        avcodec_receive_frame = nullptr;
    decltype (&::avcodec_flush_buffers)        avcodec_flush_buffers = nullptr;
    decltype (&::av_packet_alloc)              av_packet_alloc = nullptr;
    decltype (&::av_packet_free)               av_packet_free = nullptr;
    decltype (&::av_packet_unref)              av_packet_unref = nullptr;

    // libavformat
    decltype (&::avformat_alloc_context)       avformat_alloc_context = nullptr;
    decltype (&::avformat_open_input)          avformat_open_input = nullptr;
    decltype (&::avformat_find_stream_info)    avformat_find_stream_info = nullptr;
    decltype (&::avformat_close_input)         avformat_close_input = nullptr;
    decltype (&::av_read_frame)                av_read_frame = nullptr;
    decltype (&::avformat_seek_file)           avformat_seek_file = nullptr;
    decltype (&::avio_alloc_context)           avio_alloc_context = nullptr;
    decltype (&::avio_context_free)            avio_context_free = nullptr;
    decltype (&::av_find_best_stream)          av_find_best_stream = nullptr;

    FFmpegLib() { load(); }

private:
    void* handles[4] = { nullptr, nullptr, nullptr, nullptr };

    static juce::String soname (const char* base, int major)
    {
        return juce::String (base) + ".so." + juce::String (major);
    }

    void load()
    {
        // 依存順に開く（avutil -> swresample -> avcodec -> avformat）。
        handles[0] = dlopen (soname ("libavutil",      LIBAVUTIL_VERSION_MAJOR).toRawUTF8(),      RTLD_NOW | RTLD_GLOBAL);
        handles[1] = dlopen (soname ("libswresample",  LIBSWRESAMPLE_VERSION_MAJOR).toRawUTF8(),  RTLD_NOW | RTLD_GLOBAL);
        handles[2] = dlopen (soname ("libavcodec",     LIBAVCODEC_VERSION_MAJOR).toRawUTF8(),     RTLD_NOW | RTLD_GLOBAL);
        handles[3] = dlopen (soname ("libavformat",    LIBAVFORMAT_VERSION_MAJOR).toRawUTF8(),    RTLD_NOW | RTLD_GLOBAL);

        if (handles[0] == nullptr || handles[1] == nullptr || handles[2] == nullptr || handles[3] == nullptr)
            return;

        auto* hUtil   = handles[0];
        auto* hSwr    = handles[1];
        auto* hCodec  = handles[2];
        auto* hFormat = handles[3];

        // dlsym して関数ポインタへ代入。1 つでも欠ければ ok=false のまま（= 利用不可）。
        #define MC_LOAD(handle, fn)                                                     \
            do {                                                                        \
                fn = reinterpret_cast<decltype (fn)> (dlsym ((handle), #fn));           \
                if (fn == nullptr) return;                                              \
            } while (0)

        MC_LOAD (hUtil, av_frame_alloc);
        MC_LOAD (hUtil, av_frame_free);
        MC_LOAD (hUtil, av_frame_unref);
        MC_LOAD (hUtil, av_freep);
        MC_LOAD (hUtil, av_malloc);
        MC_LOAD (hUtil, av_rescale_q);
        MC_LOAD (hUtil, av_channel_layout_default);
        MC_LOAD (hUtil, av_channel_layout_uninit);

        MC_LOAD (hSwr, swr_alloc_set_opts2);
        MC_LOAD (hSwr, swr_init);
        MC_LOAD (hSwr, swr_convert);
        MC_LOAD (hSwr, swr_free);

        MC_LOAD (hCodec, avcodec_find_decoder);
        MC_LOAD (hCodec, avcodec_alloc_context3);
        MC_LOAD (hCodec, avcodec_parameters_to_context);
        MC_LOAD (hCodec, avcodec_open2);
        MC_LOAD (hCodec, avcodec_free_context);
        MC_LOAD (hCodec, avcodec_send_packet);
        MC_LOAD (hCodec, avcodec_receive_frame);
        MC_LOAD (hCodec, avcodec_flush_buffers);
        MC_LOAD (hCodec, av_packet_alloc);
        MC_LOAD (hCodec, av_packet_free);
        MC_LOAD (hCodec, av_packet_unref);

        MC_LOAD (hFormat, avformat_alloc_context);
        MC_LOAD (hFormat, avformat_open_input);
        MC_LOAD (hFormat, avformat_find_stream_info);
        MC_LOAD (hFormat, avformat_close_input);
        MC_LOAD (hFormat, av_read_frame);
        MC_LOAD (hFormat, avformat_seek_file);
        MC_LOAD (hFormat, avio_alloc_context);
        MC_LOAD (hFormat, avio_context_free);
        MC_LOAD (hFormat, av_find_best_stream);

        #undef MC_LOAD

        ok = true;
    }
};

const FFmpegLib& getFFmpeg()
{
    // 関数ローカル static により初回呼び出し時に一度だけ初期化される（スレッドセーフ）。
    static const FFmpegLib lib;
    return lib;
}

} // anonymous namespace

//==============================================================================
class FFmpegAACFormat::Reader : public juce::AudioFormatReader
{
public:
    Reader (juce::InputStream* sourceStream, const juce::String& readerFormatName)
        : AudioFormatReader (sourceStream, readerFormatName)
    {
        if (! openAll())
            cleanup();
    }

    ~Reader() override
    {
        cleanup();
    }

    bool isValid() const noexcept
    {
        return dec != nullptr && sampleRate > 0.0 && numChannels > 0;
    }

    // createReaderFor 側で deleteStreamIfOpeningFails==false のとき、基底クラスに
    // ストリームを破棄させないためデタッチする（所有権を呼び出し元へ戻す）。
    void detachStream() noexcept { input = nullptr; }

    bool readSamples (int* const* destSamples, int numDestChannels, int startOffsetInDestBuffer,
                      juce::int64 startSampleInFile, int numSamples) override
    {
        const juce::ScopedLock sl (lock);

        if (dec == nullptr)
            return false;

        if (startSampleInFile != currentReadSample)
        {
            doSeek (startSampleInFile);
            currentReadSample = startSampleInFile;
        }

        int produced = 0;

        while (produced < numSamples && ! endOfStream)
        {
            if (pendingReadPos >= pendingNumSamples)
            {
                if (! decodeNextChunk())
                    break;

                // シーク直後: 目標サンプルまで読み飛ばす。
                if (pendingStartSample < currentReadSample)
                {
                    const juce::int64 skip = juce::jmin ((juce::int64) (pendingNumSamples - pendingReadPos),
                                                         currentReadSample - pendingStartSample);
                    pendingReadPos     += (int) skip;
                    pendingStartSample += skip;
                }
                continue;
            }

            // デコード結果が要求位置より先（前方ギャップ）なら無音で埋める。
            if (pendingStartSample > currentReadSample)
            {
                const int gap = (int) juce::jmin ((juce::int64) (numSamples - produced),
                                                  pendingStartSample - currentReadSample);
                writeSilence (destSamples, numDestChannels, startOffsetInDestBuffer + produced, gap);
                produced          += gap;
                currentReadSample += gap;
                continue;
            }

            const int avail  = pendingNumSamples - pendingReadPos;
            const int toCopy = juce::jmin (avail, numSamples - produced);

            for (int ch = 0; ch < numDestChannels; ++ch)
            {
                if (destSamples[ch] == nullptr)
                    continue;

                float* dest = reinterpret_cast<float*> (destSamples[ch]) + startOffsetInDestBuffer + produced;

                if (ch < (int) numChannels)
                {
                    const float* src = pending.getReadPointer (ch) + pendingReadPos;
                    std::memcpy (dest, src, sizeof (float) * (size_t) toCopy);
                }
                else
                {
                    std::memset (dest, 0, sizeof (float) * (size_t) toCopy);
                }
            }

            pendingReadPos     += toCopy;
            pendingStartSample += toCopy;
            produced           += toCopy;
            currentReadSample  += toCopy;
        }

        // 末尾（EOF など）は無音で埋める。
        if (produced < numSamples)
        {
            writeSilence (destSamples, numDestChannels, startOffsetInDestBuffer + produced, numSamples - produced);
            currentReadSample += (numSamples - produced);
        }

        return true;
    }

private:
    //==============================================================================
    // メモリ上の入力データに対する AVIO コールバック
    static int readPacket (void* opaque, uint8_t* buf, int bufSize)
    {
        auto* self = static_cast<Reader*> (opaque);
        const size_t total = self->data.getSize();

        if (self->dataPos >= total)
            return AVERROR_EOF;

        const size_t remaining = total - self->dataPos;
        const int n = (int) juce::jmin ((size_t) bufSize, remaining);
        std::memcpy (buf, static_cast<const uint8_t*> (self->data.getData()) + self->dataPos, (size_t) n);
        self->dataPos += (size_t) n;
        return n;
    }

    static int64_t seekIO (void* opaque, int64_t offset, int whence)
    {
        auto* self = static_cast<Reader*> (opaque);
        const int64_t total = (int64_t) self->data.getSize();

        if (whence == AVSEEK_SIZE)
            return total;

        int64_t newPos;
        switch (whence & ~AVSEEK_FORCE)
        {
            case SEEK_SET: newPos = offset; break;
            case SEEK_CUR: newPos = (int64_t) self->dataPos + offset; break;
            case SEEK_END: newPos = total + offset; break;
            default:       return -1;
        }

        newPos = juce::jlimit ((int64_t) 0, total, newPos);
        self->dataPos = (size_t) newPos;
        return newPos;
    }

    //==============================================================================
    bool openAll()
    {
        const auto& ff = getFFmpeg();
        if (! ff.ok)
            return false;

        // 入力ストリーム全体をメモリへ読み込み、メモリ AVIO で扱う。
        input->setPosition (0);
        input->readIntoMemoryBlock (data);
        if (data.getSize() == 0)
            return false;
        dataPos = 0;

        const int ioBufSize = 32768;
        auto* ioBuf = static_cast<unsigned char*> (ff.av_malloc (ioBufSize));
        if (ioBuf == nullptr)
            return false;

        avio = ff.avio_alloc_context (ioBuf, ioBufSize, 0, this, &readPacket, nullptr, &seekIO);
        if (avio == nullptr)
        {
            ff.av_freep (&ioBuf);
            return false;
        }

        fmt = ff.avformat_alloc_context();
        if (fmt == nullptr)
            return false;

        fmt->pb     = avio;
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

        AVFormatContext* openCtx = fmt;
        if (ff.avformat_open_input (&openCtx, nullptr, nullptr, nullptr) < 0)
        {
            // open_input 失敗時は fmt は解放済み。AVIO は CUSTOM_IO なので自前で解放する。
            fmt = nullptr;
            return false;
        }
        fmt = openCtx;

        if (ff.avformat_find_stream_info (fmt, nullptr) < 0)
            return false;

        const AVCodec* codec = nullptr;
        audioStreamIndex = ff.av_find_best_stream (fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
        if (audioStreamIndex < 0 || codec == nullptr)
            return false;

        AVStream* st = fmt->streams[audioStreamIndex];
        streamTimeBase = st->time_base;

        dec = ff.avcodec_alloc_context3 (codec);
        if (dec == nullptr)
            return false;

        if (ff.avcodec_parameters_to_context (dec, st->codecpar) < 0)
            return false;

        if (ff.avcodec_open2 (dec, codec, nullptr) < 0)
            return false;

        const int chans = dec->ch_layout.nb_channels;
        if (dec->sample_rate <= 0 || chans <= 0)
            return false;

        sampleRate          = (double) dec->sample_rate;
        numChannels         = (unsigned int) chans;
        bitsPerSample       = 32;
        usesFloatingPointData = true;

        // 長さ（サンプル数）を算出。stream->duration を優先し、無ければコンテナ全体長から。
        if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
            lengthInSamples = ff.av_rescale_q (st->duration, streamTimeBase, AVRational { 1, dec->sample_rate });
        else if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0)
            lengthInSamples = (juce::int64) (((double) fmt->duration / (double) AV_TIME_BASE) * sampleRate);
        else
            lengthInSamples = 0;

        // 出力は「ファイル本来のレート・チャンネル数のまま、planar float へフォーマット変換のみ」。
        // ホストレートへのリサンプルは上位（InMemory/Streaming 側）が行う。
        AVChannelLayout outLayout {};
        ff.av_channel_layout_default (&outLayout, chans);

        SwrContext* swrTmp = nullptr;
        const int swrRet = ff.swr_alloc_set_opts2 (&swrTmp,
                                                   &outLayout,        AV_SAMPLE_FMT_FLTP, dec->sample_rate,
                                                   &dec->ch_layout,   dec->sample_fmt,    dec->sample_rate,
                                                   0, nullptr);
        ff.av_channel_layout_uninit (&outLayout);

        if (swrRet < 0 || swrTmp == nullptr)
            return false;
        swr = swrTmp;

        if (ff.swr_init (swr) < 0)
            return false;

        pkt   = ff.av_packet_alloc();
        frame = ff.av_frame_alloc();
        if (pkt == nullptr || frame == nullptr)
            return false;

        pending.setSize (chans, 0);
        decodedSamplePos = 0;
        currentReadSample = 0;
        return true;
    }

    void cleanup()
    {
        const auto& ff = getFFmpeg();
        if (! ff.ok)
            return;

        if (frame != nullptr) ff.av_frame_free (&frame);
        if (pkt   != nullptr) ff.av_packet_free (&pkt);
        if (swr   != nullptr) ff.swr_free (&swr);
        if (dec   != nullptr) ff.avcodec_free_context (&dec);
        if (fmt   != nullptr) ff.avformat_close_input (&fmt); // CUSTOM_IO のため avio は解放されない
        if (avio  != nullptr)
        {
            ff.av_freep (&avio->buffer); // 現行の内部バッファを解放（途中で再確保され得る）
            ff.avio_context_free (&avio);
        }
    }

    //==============================================================================
    void doSeek (juce::int64 targetSample)
    {
        const auto& ff = getFFmpeg();
        const int64_t ts = ff.av_rescale_q (targetSample, AVRational { 1, (int) sampleRate }, streamTimeBase);

        ff.avformat_seek_file (fmt, audioStreamIndex, INT64_MIN, ts, ts, 0);
        ff.avcodec_flush_buffers (dec);

        pendingReadPos   = 0;
        pendingNumSamples = 0;
        seekPending      = true;
        draining         = false;
        endOfStream      = false;
        decodedSamplePos = targetSample; // 最初のフレームの pts で補正する
    }

    // 次のフレームをデコードして pending バッファへ詰める。EOF なら false。
    bool decodeNextChunk()
    {
        const auto& ff = getFFmpeg();

        for (;;)
        {
            const int ret = ff.avcodec_receive_frame (dec, frame);

            if (ret == 0)
            {
                if (seekPending)
                {
                    int64_t pts = frame->best_effort_timestamp;
                    if (pts == AV_NOPTS_VALUE)
                        pts = frame->pts;
                    if (pts != AV_NOPTS_VALUE)
                        decodedSamplePos = ff.av_rescale_q (pts, streamTimeBase, AVRational { 1, (int) sampleRate });
                    seekPending = false;
                }
                return convertFrameToPending();
            }

            if (ret == AVERROR_EOF)
            {
                endOfStream = true;
                return false;
            }

            if (ret != AVERROR (EAGAIN))
            {
                endOfStream = true;
                return false;
            }

            // EAGAIN: デコーダへ入力を供給する。
            if (draining)
            {
                endOfStream = true;
                return false;
            }

            const int rret = ff.av_read_frame (fmt, pkt);
            if (rret < 0)
            {
                // 入力 EOF: null パケットでデコーダをフラッシュ。
                ff.avcodec_send_packet (dec, nullptr);
                draining = true;
                continue;
            }

            if (pkt->stream_index != audioStreamIndex)
            {
                ff.av_packet_unref (pkt);
                continue;
            }

            ff.avcodec_send_packet (dec, pkt);
            ff.av_packet_unref (pkt);
        }
    }

    bool convertFrameToPending()
    {
        const auto& ff = getFFmpeg();

        const int inSamples = frame->nb_samples;
        if (inSamples <= 0)
        {
            ff.av_frame_unref (frame);
            return decodeNextChunk();
        }

        const int chans = pending.getNumChannels();
        const int outCap = inSamples + 32; // リサンプル無しでも swr の遅延に余裕を持たせる

        pending.setSize (chans, outCap, false, false, true);

        // planar float 出力: 各チャンネルプレーンへ直接書き込む。
        std::vector<uint8_t*> outPlanes ((size_t) chans, nullptr);
        for (int c = 0; c < chans; ++c)
            outPlanes[(size_t) c] = reinterpret_cast<uint8_t*> (pending.getWritePointer (c));

        const int out = ff.swr_convert (swr,
                                        outPlanes.data(), outCap,
                                        const_cast<const uint8_t**> (frame->extended_data), inSamples);

        ff.av_frame_unref (frame);

        if (out < 0)
        {
            endOfStream = true;
            return false;
        }

        pendingNumSamples = out;
        pendingReadPos    = 0;
        pendingStartSample = decodedSamplePos;
        decodedSamplePos  += out;

        if (out == 0)
            return decodeNextChunk();

        return true;
    }

    void writeSilence (int* const* destSamples, int numDestChannels, int startOffset, int num)
    {
        if (num <= 0)
            return;
        for (int ch = 0; ch < numDestChannels; ++ch)
        {
            if (destSamples[ch] == nullptr)
                continue;
            float* dest = reinterpret_cast<float*> (destSamples[ch]) + startOffset;
            std::memset (dest, 0, sizeof (float) * (size_t) num);
        }
    }

    //==============================================================================
    juce::MemoryBlock data;
    size_t dataPos = 0;

    AVFormatContext* fmt = nullptr;
    AVIOContext*     avio = nullptr;
    AVCodecContext*  dec = nullptr;
    SwrContext*      swr = nullptr;
    AVPacket*        pkt = nullptr;
    AVFrame*         frame = nullptr;

    int       audioStreamIndex = -1;
    AVRational streamTimeBase { 0, 1 };

    juce::AudioBuffer<float> pending;
    int          pendingReadPos = 0;
    int          pendingNumSamples = 0;
    juce::int64  pendingStartSample = 0; // pending[pendingReadPos] の絶対サンプル位置
    juce::int64  decodedSamplePos = 0;   // 次にデコードされるフレーム先頭の絶対サンプル位置
    juce::int64  currentReadSample = 0;  // 次に readSamples が期待する絶対サンプル位置
    bool         seekPending = false;
    bool         draining = false;
    bool         endOfStream = false;

    juce::CriticalSection lock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Reader)
};

//==============================================================================
FFmpegAACFormat::FFmpegAACFormat()
    : AudioFormat ("FFmpeg AAC", ".m4a .aac .mp4 .m4b")
{
}

FFmpegAACFormat::~FFmpegAACFormat() = default;

juce::Array<int> FFmpegAACFormat::getPossibleSampleRates()
{
    return { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000 };
}

juce::Array<int> FFmpegAACFormat::getPossibleBitDepths()
{
    return { 16, 24, 32 };
}

juce::AudioFormatReader* FFmpegAACFormat::createReaderFor (juce::InputStream* sourceStream,
                                                           bool deleteStreamIfOpeningFails)
{
    if (! isFFmpegAvailable())
    {
        if (deleteStreamIfOpeningFails)
            delete sourceStream;
        return nullptr;
    }

    auto reader = std::make_unique<Reader> (sourceStream, getFormatName());

    if (reader->isValid())
        return reader.release();

    // 失敗時: deleteStreamIfOpeningFails==false なら所有権を呼び出し元へ戻す。
    if (! deleteStreamIfOpeningFails)
        reader->detachStream();

    return nullptr;
}

std::unique_ptr<juce::AudioFormatWriter> FFmpegAACFormat::createWriterFor (
    std::unique_ptr<juce::OutputStream>&, const juce::AudioFormatWriterOptions&)
{
    return nullptr; // 書き込みは非対応
}

bool FFmpegAACFormat::isFFmpegAvailable()
{
    return getFFmpeg().ok;
}

} // namespace mc3

#endif // MIXCOMPARE_HAVE_FFMPEG

#endif // JUCE_LINUX
