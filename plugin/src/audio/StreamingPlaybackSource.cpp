// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#include "StreamingPlaybackSource.h"
#include "../core/ErrorManager.h"

namespace MixCompare
{

StreamingPlaybackSource::StreamingPlaybackSource(const juce::File& audioFile,
                                                 juce::AudioFormatManager& formatManager)
    : file(audioFile)
{
    std::unique_ptr<juce::AudioFormatReader> r(formatManager.createReaderFor(file));
    if (r)
    {
        fileSampleRate = r->sampleRate;
        durationSec = static_cast<double>(r->lengthInSamples) / std::max(1.0, fileSampleRate);
        reader.reset(r.release());
    }
    else
    {
        ErrorManager::getInstance().reportError(ErrorCode::FileFormatNotSupported,
            "Unsupported audio format", "", file.getFullPathName());
    }
}

StreamingPlaybackSource::StreamingPlaybackSource(std::unique_ptr<juce::AudioFormatReader> prebuiltReader)
{
    if (prebuiltReader)
    {
        fileSampleRate = prebuiltReader->sampleRate;
        durationSec = static_cast<double>(prebuiltReader->lengthInSamples) / std::max(1.0, fileSampleRate);
        reader = std::move(prebuiltReader);
    }
    else
    {
        // 追加情報（拡張子, 存在, サイズ）を詳細に付与
        const bool exists = file.existsAsFile();
        const auto size = exists ? (juce::String) juce::String(file.getSize()) + " bytes" : juce::String("n/a");
        juce::String details;
        details << "extension=" << file.getFileExtension() << ", exists=" << (exists ? "true" : "false")
                << ", size=" << size;
        ErrorManager::getInstance().reportError(ErrorCode::AudioFormatError,
            "Audio reader could not be initialised", details, file.getFullPathName());
    }
}

StreamingPlaybackSource::~StreamingPlaybackSource()
{
    release();
}

void StreamingPlaybackSource::prepare(double sampleRate, int blockSize)
{
    hostSampleRate = sampleRate;
    hostBlockSize = blockSize;
    loopFadeInActive = false;
    loopFadeInProgress = 0;

    // Reuse the existing transport if we are already prepared
    if (transport)
    {
        transport->prepareToPlay(hostBlockSize, hostSampleRate);
        return;
    }

    fadeSamples = static_cast<int>(std::max(1.0, hostSampleRate * 0.005));
    fadeInRemain = 0;
    fadeOutRemain = 0;
    loopFadeSamples = static_cast<int>(std::max(1.0, hostSampleRate * 0.01));
    loopFadeInActive = false;
    loopFadeInProgress = 0;

    if (reader)
    {
        readAheadThread = std::make_unique<juce::TimeSliceThread>("streaming read-ahead");
        // ユーザー環境の方針に合わせ通常優先度を使用
        readAheadThread->startThread(juce::Thread::Priority::normal);

        readerSource = std::make_unique<juce::AudioFormatReaderSource>(reader.get(), false);

        transport = std::make_unique<juce::AudioTransportSource>();
        // 読み出し先行バッファを拡大してドロップアウトを防ぐ（約 131072 サンプル）
        // Media Foundation 経由の AAC/m4a は内部で 48k に変換される環境があり、
        // ソースSRを誤指定すると時間進行が不連続になることがあるため、
        // 拡張子が .m4a/.aac の場合は sourceSampleRate を 0.0（= リサンプル無効）で渡す。
        double sourceSR = fileSampleRate;
#if JUCE_WINDOWS
        {
            const juce::String ext = file.getFileExtension().toLowerCase();
            if (ext == ".m4a" || ext == ".aac")
                sourceSR = 0.0; // デコード側のPCMレートをそのまま使用
        }
#endif
        transport->setSource(readerSource.get(), 131072, readAheadThread.get(), sourceSR);
        transport->prepareToPlay(hostBlockSize, hostSampleRate);
        transport->setGain(1.0f);
        transport->setLooping(false);
        // start は再生要求時（初回 process 呼び出し時）に遅延開始する
    }
    else
    {
        ErrorManager::getInstance().reportError(ErrorCode::AudioFormatError,
            "Streaming source is not initialised",
            "reader is null (unsupported format or codec not available)", file.getFullPathName());
    }
}

void StreamingPlaybackSource::release()
{
    if (transport)
    {
        transport->stop();
        transport->releaseResources();
        transport->setSource(nullptr);
    }

    readerSource.reset();
    transport.reset();

    if (readAheadThread)
    {
        readAheadThread->stopThread(1000);
        readAheadThread.reset();
    }

    loopFadeInActive = false;
    loopFadeInProgress = 0;
}

void StreamingPlaybackSource::seek(double positionSeconds) noexcept
{
    if (transport)
        transport->setPosition(positionSeconds);
    pendingSeekSec.store(positionSeconds);
    fadeInRemain = fadeSamples;
    loopFadeInActive = false;
    loopFadeInProgress = 0;
    // シーク直後の1ブロックはループチェックをスキップして範囲外シークを許可
    skipLoopCheckOnce.store(true, std::memory_order_release);
    // 未来側へシークした可能性があるので、wrap を一時的に無効化
    loopDisarmedUntilBelowEnd.store(true, std::memory_order_release);
}

void StreamingPlaybackSource::setLoop(double startSec, double endSec, bool enabled) noexcept
{
    loopEnabled.store(enabled);
    loopStartSec.store(std::min(startSec, endSec));
    loopEndSec.store(std::max(startSec, endSec));
    if (!enabled)
    {
        loopFadeInActive = false;
        loopFadeInProgress = 0;
    }
}

void StreamingPlaybackSource::beginSwap() noexcept
{
    swapRequested.store(true);
    fadeOutRemain = fadeSamples;
}

void StreamingPlaybackSource::endSwap() noexcept
{
    swapRequested.store(false);
    fadeOutRemain = 0;
}

void StreamingPlaybackSource::applyFades(juce::AudioBuffer<float>& buffer, int numSamples) noexcept
{
    if (fadeInRemain <= 0 && fadeOutRemain <= 0)
        return;

    const int channels = buffer.getNumChannels();
    for (int i = 0; i < numSamples; ++i)
    {
        float gain = 1.0f;
        if (fadeInRemain > 0)
        {
            const int progressed = fadeSamples - fadeInRemain;
            const float t = static_cast<float>(std::max(0, progressed)) / static_cast<float>(std::max(1, fadeSamples));
            gain *= t;
            --fadeInRemain;
        }
        if (fadeOutRemain > 0)
        {
            const float t = static_cast<float>(fadeOutRemain) / static_cast<float>(std::max(1, fadeSamples));
            gain *= t;
            --fadeOutRemain;
        }

        if (gain != 1.0f)
        {
            for (int ch = 0; ch < channels; ++ch)
            {
                float* d = buffer.getWritePointer(ch);
                d[i] *= gain;
            }
        }
    }
}

void StreamingPlaybackSource::process(juce::AudioBuffer<float>& outBuffer, int numSamples) noexcept
{
    outBuffer.clear();
    if (!transport)
        return;

    // 遅延開始: 初回の process 呼び出しで開始（prepare 完了後）
    if (!transport->isPlaying())
        transport->start();

    const double seekSec = pendingSeekSec.exchange(-1.0);
    if (seekSec >= 0.0)
        transport->setPosition(seekSec);

    juce::AudioSourceChannelInfo info(&outBuffer, 0, numSamples);
    transport->getNextAudioBlock(info);

    if (loopFadeInActive)
        applyLoopFadeIn(outBuffer, numSamples);

    // シーク直後の1ブロックはループチェックをスキップ（範囲外シークを許可）
    if (skipLoopCheckOnce.exchange(false, std::memory_order_acq_rel))
    {
        // このブロックではループチェックをスキップ
    }
    else if (loopEnabled.load())
    {
        const double pos = transport->getCurrentPosition();
        const double ls = loopStartSec.load();
        const double le = loopEndSec.load();
        // 未来側シーク後は、一度 pos が le 未満に戻るまで wrap を無効化
        if (loopDisarmedUntilBelowEnd.load(std::memory_order_acquire))
        {
            if (pos < le)
            {
                // 一度範囲内に戻ったので再武装
                loopDisarmedUntilBelowEnd.store(false, std::memory_order_release);
            }
            // 再武装までは wrap しない
        }
        else if (le > ls && pos >= le)
        {
            applyLoopFadeOut(outBuffer, numSamples);
            loopFadeInActive = true;
            loopFadeInProgress = 0;
            transport->setPosition(ls);
        }
    }

    applyFades(outBuffer, numSamples);
}

void StreamingPlaybackSource::applyLoopFadeOut(juce::AudioBuffer<float>& buffer, int numSamples) noexcept
{
    if (loopFadeSamples <= 0)
        return;

    const int channels = buffer.getNumChannels();
    if (channels <= 0)
        return;

    const int fadeLength = std::min(loopFadeSamples, numSamples);
    if (fadeLength <= 0)
        return;

    const int startIndex = std::max(0, numSamples - fadeLength);
    const int denom = std::max(1, fadeLength - 1);

    for (int ch = 0; ch < channels; ++ch)
    {
        float* data = buffer.getWritePointer(ch);
        for (int i = 0; i < fadeLength; ++i)
        {
            const int idx = startIndex + i;
            if (idx < 0 || idx >= numSamples)
                continue;
            const float t = fadeLength == 1 ? 1.0f : static_cast<float>(i) / static_cast<float>(denom);
            data[idx] *= (1.0f - t);
        }
    }
}

void StreamingPlaybackSource::applyLoopFadeIn(juce::AudioBuffer<float>& buffer, int numSamples) noexcept
{
    if (loopFadeSamples <= 0 || !loopFadeInActive)
        return;

    const int channels = buffer.getNumChannels();
    if (channels <= 0)
        return;

    const int denom = std::max(1, loopFadeSamples - 1);
    const int remaining = loopFadeSamples - loopFadeInProgress;
    if (remaining <= 0)
    {
        loopFadeInActive = false;
        return;
    }

    const int samplesToApply = std::min(remaining, numSamples);
    if (samplesToApply <= 0)
        return;

    for (int ch = 0; ch < channels; ++ch)
    {
        float* data = buffer.getWritePointer(ch);
        for (int i = 0; i < samplesToApply; ++i)
        {
            const int idx = i;
            if (idx < 0 || idx >= numSamples)
                continue;
            const int progress = loopFadeInProgress + i;
            const float t = (loopFadeSamples == 1)
                                ? 1.0f
                                : static_cast<float>(progress) / static_cast<float>(denom);
            data[idx] *= juce::jlimit(0.0f, 1.0f, t);
        }
    }

    loopFadeInProgress += samplesToApply;
    if (loopFadeInProgress >= loopFadeSamples)
        loopFadeInActive = false;
}

} // namespace MixCompare
