#include "AudioEngine.h"
#include "FormatUtils.h" // 表示用フォーマット/量子化（周波数）

namespace MixCompare
{

AudioEngine::AudioEngine()
{
    lowPassFilterLeft.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    lowPassFilterRight.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
    
    // Snapshot 経路は廃止
}

AudioEngine::~AudioEngine() = default;

void AudioEngine::prepareToPlay(double newSampleRate, int samplesPerBlock)
{
    this->sampleRate = newSampleRate;
    this->blockSize = samplesPerBlock;
    
    
    // Initialize last processed buffers
    lastHostBuffer.setSize(2, samplesPerBlock);
    lastPlaylistBuffer.setSize(2, samplesPerBlock);
    lastHostBuffer.clear();
    lastPlaylistBuffer.clear();
    
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock);
    spec.numChannels = 1;
    
    lowPassFilterLeft.prepare(spec);
    lowPassFilterRight.prepare(spec);
    
    lowPassFilterLeft.reset();
    lowPassFilterRight.reset();
    
    // メータリングに関する初期化は MeteringService 側で実施
    
    // 初期 LPF 周波数はリアルタイム値から設定
    const float q = static_cast<float>(mc3::format::quantizeFrequencyForProcessing(static_cast<double>(rtLpfFrequencyHz.load())));
    lowPassFilterLeft.setCutoffFrequency(q);
    lowPassFilterRight.setCutoffFrequency(q);

    // 固定ワークバッファ確保（割当回避）
    workHostBuffer.setSize(2, samplesPerBlock);
    workPlaylistBuffer.setSize(2, samplesPerBlock);
    workNextPlaylistBuffer.setSize(2, samplesPerBlock);
    workHostBuffer.clear();
    workPlaylistBuffer.clear();
    workNextPlaylistBuffer.clear();
    workFilteredBuffer.setSize(2, samplesPerBlock);
    workFilteredBuffer.clear();

    const double gainSmoothingTimeSec = 0.002;
    const double blendSmoothingTimeSec = 0.003;
    const double lpfSmoothingTimeSec = 0.003;

    hostGainSmoothed.reset(sampleRate, gainSmoothingTimeSec);
    playlistGainSmoothed.reset(sampleRate, gainSmoothingTimeSec);
    sourceBlendSmoothed.reset(sampleRate, blendSmoothingTimeSec);
    lpfMixSmoothed.reset(sampleRate, lpfSmoothingTimeSec);

    const float initialHostGain = juce::Decibels::decibelsToGain(rtHostGainDb.load(std::memory_order_acquire));
    const float initialPlaylistGain = juce::Decibels::decibelsToGain(rtPlaylistGainDb.load(std::memory_order_acquire));
    const float initialSourceBlend = rtCurrentSourceIndex.load(std::memory_order_acquire) == 0 ? 0.0f : 1.0f;
    const bool initialLpfEnabled = rtLpfEnabled.load(std::memory_order_acquire);

    hostGainSmoothed.setCurrentAndTargetValue(initialHostGain);
    playlistGainSmoothed.setCurrentAndTargetValue(initialPlaylistGain);
    sourceBlendSmoothed.setCurrentAndTargetValue(initialSourceBlend);
    lpfMixSmoothed.setCurrentAndTargetValue(initialLpfEnabled ? 1.0f : 0.0f);
    lastLpfEnabled = initialLpfEnabled;

    // クロスフェード長（~5ms）
    crossfadeSamples = static_cast<int>(std::max(1.0, sampleRate * 0.005));
    remainingCrossfadeSamples.store(0, std::memory_order_release);

    // 既存ソースがあれば新しい SR/BS で再準備（起動直後のSR未確定→後確定のミスマッチを解消）
    if (auto current = std::atomic_load_explicit(&currentSource, std::memory_order_acquire))
        current->prepare(sampleRate, blockSize);

    if (auto next = std::atomic_load_explicit(&nextSource, std::memory_order_acquire))
        next->prepare(sampleRate, blockSize);
}

void AudioEngine::releaseResources()
{
    lowPassFilterLeft.reset();
    lowPassFilterRight.reset();
    lpfMixSmoothed.setCurrentAndTargetValue(0.0f);
    lastLpfEnabled = false;
}

void AudioEngine::setPlaybackSource(std::shared_ptr<IPlaybackSource> source)
{
    
    // 新しい再生ソースを適用する。
    // - 現在ソースが無ければ即時適用（準備の上、クロスフェードなし）
    // - 存在する場合は nextSource として保持し、ブロック内でクロスフェードして安全にスワップ
    if (!source)
    {
        
        std::atomic_store_explicit(&currentSource, std::shared_ptr<IPlaybackSource>{}, std::memory_order_release);
        std::atomic_store_explicit(&nextSource, std::shared_ptr<IPlaybackSource>{}, std::memory_order_release);
        remainingCrossfadeSamples.store(0, std::memory_order_release);
        return;
    }

    // ソースは AudioEngine の SR/BS で準備させる（RT外）
    source->prepare(sampleRate, blockSize);
    

    auto current = std::atomic_load_explicit(&currentSource, std::memory_order_acquire);
    if (!current)
    {
        // 初回適用: そのまま現在ソースに設定
        std::atomic_store_explicit(&currentSource, source, std::memory_order_release);
        remainingCrossfadeSamples.store(0, std::memory_order_release);
        currentPlaybackPosition = 0.0;
        previousPosition = 0.0;
    }
    else
    {
        // 以降はクロスフェードで切替
        std::atomic_store_explicit(&nextSource, source, std::memory_order_release);
        current->beginSwap();
        source->beginSwap();
        remainingCrossfadeSamples.store(crossfadeSamples, std::memory_order_release);
        previousPosition = 0.0;
    }
}

void AudioEngine::setPlaybackSourcePrepared(std::shared_ptr<IPlaybackSource> source)
{
    // 既に prepare 済み前提。ここではクロスフェード切替のみに限定。
    if (!source)
    {
        std::atomic_store_explicit(&currentSource, std::shared_ptr<IPlaybackSource>{}, std::memory_order_release);
        std::atomic_store_explicit(&nextSource, std::shared_ptr<IPlaybackSource>{}, std::memory_order_release);
        remainingCrossfadeSamples.store(0, std::memory_order_release);
        return;
    }

    // AudioEngine の現在の SR/BS に合わせて最終的に整合（Streaming は冪等）
    source->prepare(sampleRate, blockSize);
    

    auto current = std::atomic_load_explicit(&currentSource, std::memory_order_acquire);
    if (!current)
    {
        std::atomic_store_explicit(&currentSource, source, std::memory_order_release);
        remainingCrossfadeSamples.store(0, std::memory_order_release);
        
    }
    else
    {
        std::atomic_store_explicit(&nextSource, source, std::memory_order_release);
        current->beginSwap();
        source->beginSwap();
        remainingCrossfadeSamples.store(crossfadeSamples, std::memory_order_release);
        
    }
}

void AudioEngine::getLastProcessedBuffers(juce::AudioBuffer<float>& hostBuffer, 
                                          juce::AudioBuffer<float>& playlistBuffer) const
{
    hostBuffer.makeCopyOf(lastHostBuffer);
    playlistBuffer.makeCopyOf(lastPlaylistBuffer);
}

// Snapshot 経路は廃止

void AudioEngine::setRealtimeParams(const RealtimeParams& params) noexcept
{
    // 原子的に各値を入れ替える（依存関係のないスカラーのみ）
    rtHostGainDb.store(params.hostGainDb, std::memory_order_release);
    rtPlaylistGainDb.store(params.playlistGainDb, std::memory_order_release);
    rtLpfEnabled.store(params.lpfEnabled, std::memory_order_release);
    rtLpfFrequencyHz.store(params.lpfFrequencyHz, std::memory_order_release);
    rtMeteringMode.store(params.meteringMode, std::memory_order_release);
    rtTransportPlaying.store(params.transportPlaying, std::memory_order_release);
    rtCurrentSourceIndex.store(params.currentSourceIndex, std::memory_order_release);
    rtLoopEnabled.store(params.loopEnabled, std::memory_order_release);
    rtLoopStartSamples.store(params.loopStartSamples, std::memory_order_release);
    rtLoopEndSamples.store(params.loopEndSamples, std::memory_order_release);

    // LPF cutoff の即時更新（安全のため量子化）
    const float q = static_cast<float>(mc3::format::quantizeFrequencyForProcessing(static_cast<double>(params.lpfFrequencyHz)));
    lowPassFilterLeft.setCutoffFrequency(q);
    lowPassFilterRight.setCutoffFrequency(q);
}

void AudioEngine::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused(midiMessages);
    juce::ScopedNoDenormals noDenormals;
    
    auto totalNumInputChannels = buffer.getNumChannels();
    auto totalNumOutputChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    
    // Ensure stereo
    if (totalNumOutputChannels < 2)
    {
        buffer.clear();
        return;
    }
    
    // Use fixed work buffers (no allocation on RT)
    auto& hostBuffer = workHostBuffer;
    auto& playlistBuffer = workPlaylistBuffer;
    hostBuffer.clear();
    playlistBuffer.clear();
    
    // Copy host input
    for (int channel = 0; channel < juce::jmin(2, totalNumInputChannels); ++channel)
    {
        hostBuffer.copyFrom(channel, 0, buffer, channel, 0, numSamples);
    }
    
    // If mono input, duplicate to stereo
    if (totalNumInputChannels == 1)
    {
        hostBuffer.copyFrom(1, 0, hostBuffer, 0, 0, numSamples);
    }
    
    // ホスト同期かどうか
    const bool hostSync = rtHostSyncEnabled.load(std::memory_order_acquire);
    const bool isTransportPlaying = rtTransportPlaying.load(std::memory_order_acquire);

    auto current = std::atomic_load_explicit(&currentSource, std::memory_order_acquire);

    // Process playlist audio via playback source（currentSource があるなら停止時も位置を維持）
    if (current)
    {
        bool shouldProcessPlaylist = false;
        bool loopEnabledNow = false;
        if (hostSync)
        {
            // 同期時: ループ無効化し、必要な場合のみシーク
            current->setLoop(0.0, 0.0, false);
            loopEnabledNow = false;
            const double hostPosSec = rtHostPositionSec.load(std::memory_order_acquire);
            const double durSec = current->getDurationSec();
            const bool inRange = !(hostPosSec < 0.0 || (durSec > 0.0 && hostPosSec >= durSec));
            const bool hostIsPlaying = rtHostIsPlaying.load(std::memory_order_acquire);
            if (inRange && hostIsPlaying)
            {
                const double sourcePosSec = current->getCurrentPositionSec();
                const double divergence = std::abs(hostPosSec - sourcePosSec);
                const double thresholdSec = 0.02; // 20ms
                if (divergence > thresholdSec)
                {
                    current->seek(hostPosSec);
                }
                shouldProcessPlaylist = true;
            }
            else
            {
                // 範囲外、または停止中は無音
                playlistBuffer.clear();
            }
        }
        else
        {
            const bool loopOn = rtLoopEnabled.load(std::memory_order_acquire);
            const double lsSamples = rtLoopStartSamples.load(std::memory_order_acquire);
            const double leSamples = rtLoopEndSamples.load(std::memory_order_acquire);
            const double fsr = current->getFileSampleRate();
            const double lsSec = fsr > 0.0 ? (lsSamples / fsr) : 0.0;
            const double leSec = fsr > 0.0 ? (leSamples / fsr) : 0.0;
            current->setLoop(lsSec, leSec, loopOn);
            loopEnabledNow = loopOn;
            shouldProcessPlaylist = isTransportPlaying;
            // 再生要求が来ており、TransportSource が未開始なら開始して滑らかに同期
            if (shouldProcessPlaylist)
            {
                // IPlaybackSource 実装側で必要に応じて start を行うため、
                // process の最初の呼び出しで実開始されるようにする。
            }
        }

        // 実際の出力生成（停止時・範囲外は playlistBuffer を空のままにする）
        if (shouldProcessPlaylist)
        {
            // フラグに依存せず、ブロック内での境界通過を厳密判定
            const double dur = current->getDurationSec();
            const double posBefore = current->getCurrentPositionSec();

            current->process(playlistBuffer, numSamples);

            if (!loopEnabledNow && dur > 0.0)
            {
                const double posAfter = current->getCurrentPositionSec();
                if (posBefore < dur && posAfter >= dur)
                {
                    if (onPlaybackStopped)
                        onPlaybackStopped();
                }
            }
        }

        // スワップ要求があれば nextSource も生成してクロスフェード適用
        auto next = std::atomic_load_explicit(&nextSource, std::memory_order_acquire);
        if (next)
        {
            workNextPlaylistBuffer.clear();
            next->process(workNextPlaylistBuffer, numSamples);

            int remaining = remainingCrossfadeSamples.load(std::memory_order_acquire);
            if (remaining <= 0)
                remaining = crossfadeSamples;

            for (int i = 0; i < numSamples; ++i)
            {
                const int cf = std::max(0, remaining - i);
                const float t = static_cast<float>(cf) / static_cast<float>(std::max(1, crossfadeSamples));
                const float gOld = t;
                const float gNew = 1.0f - t;
                for (int ch = 0; ch < 2; ++ch)
                {
                    float* dst = playlistBuffer.getWritePointer(ch);
                    const float* nsrc = workNextPlaylistBuffer.getReadPointer(ch);
                    dst[i] = dst[i] * gOld + nsrc[i] * gNew;
                }
            }
            remaining = std::max(0, remaining - numSamples);
            remainingCrossfadeSamples.store(remaining, std::memory_order_release);
            if (remaining <= 0)
            {
                if (current)
                    current->endSwap();

                auto old = current;
                std::atomic_store_explicit(&retiredSourceOwned, old, std::memory_order_release);
                std::atomic_store_explicit(&currentSource, next, std::memory_order_release);
                std::atomic_store_explicit(&nextSource, std::shared_ptr<IPlaybackSource>{}, std::memory_order_release);
                remainingCrossfadeSamples.store(0, std::memory_order_release);
                current = std::move(next);
                old.reset();

                triggerAsyncUpdate();
            }
        }
    }
    
    // Apply gains from realtime params (APVTS直読み最短経路)
    const int currentSourceIdx = rtCurrentSourceIndex.load(std::memory_order_acquire);
    const float hostGainDb = rtHostGainDb.load(std::memory_order_acquire);
    const float playlistGainDb = rtPlaylistGainDb.load(std::memory_order_acquire);
    const bool lpfEnabled = rtLpfEnabled.load(std::memory_order_acquire);
    
    hostGainSmoothed.setTargetValue(juce::Decibels::decibelsToGain(hostGainDb));
    playlistGainSmoothed.setTargetValue(juce::Decibels::decibelsToGain(playlistGainDb));
    sourceBlendSmoothed.setTargetValue(currentSourceIdx == 0 ? 0.0f : 1.0f);
    if (lpfEnabled && !lastLpfEnabled)
    {
        lowPassFilterLeft.reset();
        lowPassFilterRight.reset();
    }
    lastLpfEnabled = lpfEnabled;
    lpfMixSmoothed.setTargetValue(lpfEnabled ? 1.0f : 0.0f);

    const int hostChannels = hostBuffer.getNumChannels();
    const int playlistChannels = playlistBuffer.getNumChannels();
    auto* hostLeft = hostChannels > 0 ? hostBuffer.getWritePointer(0) : nullptr;
    auto* hostRight = hostChannels > 1 ? hostBuffer.getWritePointer(1) : hostLeft;
    auto* playlistLeft = playlistChannels > 0 ? playlistBuffer.getWritePointer(0) : nullptr;
    auto* playlistRight = playlistChannels > 1 ? playlistBuffer.getWritePointer(1) : playlistLeft;

    for (int i = 0; i < numSamples; ++i)
    {
        const float hg = hostGainSmoothed.getNextValue();
        const float pg = playlistGainSmoothed.getNextValue();

        if (hostLeft)
        {
            hostLeft[i] *= hg;
            if (hostRight && hostRight != hostLeft)
                hostRight[i] *= hg;
        }
        if (playlistLeft)
        {
            playlistLeft[i] *= pg;
            if (playlistRight && playlistRight != playlistLeft)
                playlistRight[i] *= pg;
        }
    }

    // Store buffers for metering access (copy into fixed-size last buffers)
    for (int ch = 0; ch < 2; ++ch)
    {
        lastHostBuffer.copyFrom(ch, 0, hostBuffer, ch, 0, numSamples);
        lastPlaylistBuffer.copyFrom(ch, 0, playlistBuffer, ch, 0, numSamples);
    }

    // Host/Playlist クロスブレンド。sourceBlendSmoothed により1〜3msで補間。
    buffer.clear();
    auto* outLeft = buffer.getWritePointer(0);
    auto* outRight = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : outLeft;
    auto* hostReadLeft = hostChannels > 0 ? hostBuffer.getReadPointer(0) : nullptr;
    auto* hostReadRight = hostChannels > 1 ? hostBuffer.getReadPointer(1) : hostReadLeft;
    auto* playlistReadLeft = playlistChannels > 0 ? playlistBuffer.getReadPointer(0) : nullptr;
    auto* playlistReadRight = playlistChannels > 1 ? playlistBuffer.getReadPointer(1) : playlistReadLeft;

    for (int i = 0; i < numSamples; ++i)
    {
        const float mix = sourceBlendSmoothed.getNextValue();
        const float inv = 1.0f - mix;

        const float hostL = hostReadLeft ? hostReadLeft[i] : 0.0f;
        const float hostR = hostReadRight ? hostReadRight[i] : hostL;
        const float playlistL = playlistReadLeft ? playlistReadLeft[i] : 0.0f;
        const float playlistR = playlistReadRight ? playlistReadRight[i] : playlistL;

        if (outLeft)
            outLeft[i] = hostL * inv + playlistL * mix;
        if (outRight && outRight != outLeft)
            outRight[i] = hostR * inv + playlistR * mix;
    }

    const bool lpfActive = lpfEnabled || lpfMixSmoothed.isSmoothing();
    if (lpfActive)
    {
        const int filterChannels = juce::jmin(2, buffer.getNumChannels());
        for (int ch = 0; ch < filterChannels; ++ch)
            workFilteredBuffer.copyFrom(ch, 0, buffer, ch, 0, numSamples);

        applyLowPassFilter(workFilteredBuffer, numSamples);

        auto* filteredLeft = workFilteredBuffer.getReadPointer(0);
        auto* filteredRight = workFilteredBuffer.getNumChannels() > 1 ? workFilteredBuffer.getReadPointer(1) : filteredLeft;

        for (int i = 0; i < numSamples; ++i)
        {
            const float mix = lpfMixSmoothed.getNextValue();
            const float inv = 1.0f - mix;
            if (outLeft && filteredLeft)
                outLeft[i] = outLeft[i] * inv + filteredLeft[i] * mix;
            if (outRight && outRight != outLeft && filteredRight)
                outRight[i] = outRight[i] * inv + filteredRight[i] * mix;
        }
    }

    // メータリング更新は MeteringService に委譲
    
    // 再生位置のUI通知（約20Hz）
    if (onPositionChanged)
    {
        static thread_local double lastNotifiedPosition = -1.0;
        static thread_local int positionUpdateCounter = 0;
        const int updateInterval = std::max(1, (int) std::round(sampleRate / (numSamples * 20.0)));

        // 強制ゼロ通知（次の1回だけ）
        if (forceZeroPositionNotify.exchange(false, std::memory_order_acq_rel))
        {
            lastNotifiedPosition = 0.0;
            onPositionChanged(0.0);
            positionUpdateCounter = 0; // 直後の通常通知は間引く
        }
        else if (current)
        {
            if (suppressPositionNotify.load(std::memory_order_acquire))
            {
                // 抑止中は position 通知を行わず、lastNotifiedPosition を 0 に仮固定
                lastNotifiedPosition = 0.0;
            }
            else
            {
            if (++positionUpdateCounter >= updateInterval)
            {
                positionUpdateCounter = 0;
                const double posSec = current->getCurrentPositionSec();
                if (std::abs(posSec - lastNotifiedPosition) > 0.01)
                {
                    lastNotifiedPosition = posSec;
                    onPositionChanged(posSec);
                }
            }
            }
        }
    }

    // Clear any extra channels
    for (auto i = 2; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, numSamples);
}
void AudioEngine::setHostSyncState(bool enabled, bool isPlaying, double positionSec) noexcept
{
    rtHostSyncEnabled.store(enabled, std::memory_order_release);
    rtHostIsPlaying.store(isPlaying, std::memory_order_release);
    rtHostPositionSec.store(positionSec, std::memory_order_release);
}

void AudioEngine::handleAsyncUpdate()
{
    // 退役ソースを取得し、BGスレッドで release/reset を行ってUIをブロックしない
    if (auto toCleanup = std::atomic_exchange_explicit(&retiredSourceOwned,
                                                       std::shared_ptr<IPlaybackSource>{},
                                                       std::memory_order_acq_rel))
    {
        
        std::thread([ret = std::move(toCleanup)]() mutable
        {
            // 非RT/BGで安全に停止・解放
            ret->release();
            ret.reset();
        }).detach();
        
    }
}


// メータリング関連のロジックは削除（MeteringService に集約）

void AudioEngine::applyLowPassFilter(juce::AudioBuffer<float>& buffer, int numSamples)
{
    if (buffer.getNumChannels() >= 2)
    {
        auto* leftData = buffer.getWritePointer(0);
        auto* rightData = buffer.getWritePointer(1);
        
        for (int i = 0; i < numSamples; ++i)
        {
            leftData[i] = lowPassFilterLeft.processSample(0, leftData[i]);
            rightData[i] = lowPassFilterRight.processSample(0, rightData[i]);
        }
    }
}

void AudioEngine::setPlaybackPosition(double position)
{
    // Set the playback position in samples
    currentPlaybackPosition = position;
    previousPosition = position;
    // 直近で外部シークが発生したことをマーク。
    // 次の processPlaylistAudio() で 1 回だけ参照し、
    // そのブロックでのループ・ラップを抑止する。
    externalSeekPending.store(true, std::memory_order_release);
}

void AudioEngine::seekSeconds(double positionSeconds)
{
    // 新ルート: IPlaybackSource 経由で秒指定のシークを行う
    if (auto current = std::atomic_load_explicit(&currentSource, std::memory_order_acquire))
        current->seek(positionSeconds);
    // UI/Processor 側の互換挙動: サンプル基準の内部位置も概算で更新
    const double fsr = getFileSampleRate();
    setPlaybackPosition(positionSeconds * fsr);
}

double AudioEngine::getDuration() const
{
    if (auto current = std::atomic_load_explicit(&currentSource, std::memory_order_acquire))
        return current->getDurationSec();
    return 0.0;
}

double AudioEngine::getFileSampleRate() const
{
    if (auto current = std::atomic_load_explicit(&currentSource, std::memory_order_acquire))
        return current->getFileSampleRate();
    return sampleRate;
}

bool AudioEngine::hasCurrentSource() const noexcept
{
    return std::atomic_load_explicit(&currentSource, std::memory_order_acquire) != nullptr;
}

} // namespace MixCompare
