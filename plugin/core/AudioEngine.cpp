// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#include "AudioEngine.h"

namespace mc
{

void AudioEngine::prepare(double sampleRate, int blockSize)
{
    mSampleRate = sampleRate > 0 ? sampleRate : 44100.0;
    // スクラッチは余裕を持って確保 (host が大きめの block を渡しても audio thread で
    // 再 alloc しないように)。
    mCapacity = std::max(blockSize, 8192);
    mHostL.assign(mCapacity, 0.0f);
    mHostR.assign(mCapacity, 0.0f);
    mPlL.assign(mCapacity, 0.0f);
    mPlR.assign(mCapacity, 0.0f);
    mFiltL.assign(mCapacity, 0.0f);
    mFiltR.assign(mCapacity, 0.0f);

    mLpfLeft.prepare(mSampleRate);
    mLpfRight.prepare(mSampleRate);
    mLpfLeft.setCutoffFrequency(mLpfFreqHz.load(std::memory_order_relaxed));
    mLpfRight.setCutoffFrequency(mLpfFreqHz.load(std::memory_order_relaxed));

    mHostGainSmoothed.reset(mSampleRate, 0.002);
    mHostGainSmoothed.setCurrentAndTargetValue(1.0f);
    mPlaylistGainSmoothed.reset(mSampleRate, 0.002);
    mPlaylistGainSmoothed.setCurrentAndTargetValue(1.0f);
    mSourceBlendSmoothed.reset(mSampleRate, 0.003);
    mSourceBlendSmoothed.setCurrentAndTargetValue(mSourceSelect.load(std::memory_order_relaxed) == 0 ? 0.0f : 1.0f);
    mLpfMixSmoothed.reset(mSampleRate, 0.003);
    mLpfMixSmoothed.setCurrentAndTargetValue(0.0f);

    mMetering.prepareToPlay(mSampleRate, mCapacity);
    mLastLpfEnabled = false;

    mMeterInterval = std::max(1, static_cast<int>(mSampleRate / 30.0)); // ~30Hz
    mMeterCounter = 0;
    for (int i = 0; i < kMeterFloats; ++i)
        mMeterSnapshot[i].store(i == 0 ? 0.0f : -70.0f, std::memory_order_relaxed);

    // Playlist トランスポート状態 (SR 変更時。ソース自体は保持)。
    mStoppedAtEnd.store(false, std::memory_order_relaxed);
    // SR が変わったら現在ソースを新 SR で再 prepare (リサンプル比率/ワーカー更新)。
    auto src = std::atomic_load(&mActiveSource);
    if (src) src->prepareToPlay(mSampleRate);
}

void AudioEngine::reset()
{
    mLpfLeft.reset();
    mLpfRight.reset();
    mMetering.reset();
    mLastLpfEnabled = false;
    mMeterCounter = 0;
}

void AudioEngine::processBlock(const float* inL, const float* inR,
                               float* outL, float* outR, int numSamples) noexcept
{
    const int n = std::min(numSamples, mCapacity);

    // --- Host = DAW 入力 ---
    if (inL && inR)
    {
        for (int i = 0; i < n; ++i) { mHostL[i] = inL[i]; mHostR[i] = inR[i]; }
    }
    else
    {
        std::fill(mHostL.begin(), mHostL.begin() + n, 0.0f);
        std::fill(mHostR.begin(), mHostR.begin() + n, 0.0f);
    }

    // --- Playlist = 現在ソース (InMemory / Streaming) から読む。再生/ループ/シーク/
    //     リサンプルはソース側が担う。engine は playing でゲートするだけ ---
    {
      std::shared_ptr<IPlaybackSource> src = std::atomic_load(&mActiveSource);
      const bool playing = mPlaying.load(std::memory_order_relaxed);
      if (src && playing)
      {
        bool reachedEnd = false;
        src->readBlock(mPlL.data(), mPlR.data(), n, reachedEnd);
        if (reachedEnd)
        {
          mPlaying.store(false, std::memory_order_relaxed);
          mStoppedAtEnd.store(true, std::memory_order_relaxed);
        }
      }
      else
      {
        std::fill(mPlL.begin(), mPlL.begin() + n, 0.0f);
        std::fill(mPlR.begin(), mPlR.begin() + n, 0.0f);
      }
    }

    // --- ゲイン (dB→linear、サンプル単位でスムージング) ---
    const float hostGainLin = std::pow(10.0f, mHostGainDb.load(std::memory_order_relaxed) / 20.0f);
    const float plGainLin   = std::pow(10.0f, mPlaylistGainDb.load(std::memory_order_relaxed) / 20.0f);
    mHostGainSmoothed.setTargetValue(hostGainLin);
    mPlaylistGainSmoothed.setTargetValue(plGainLin);
    for (int i = 0; i < n; ++i)
    {
        const float hg = mHostGainSmoothed.getNextValue();
        const float pg = mPlaylistGainSmoothed.getNextValue();
        mHostL[i] *= hg; mHostR[i] *= hg;
        mPlL[i]   *= pg; mPlR[i]   *= pg;
    }

    // --- Host / Playlist 個別メータ (ゲイン後・ブレンド前) ---
    {
        const float* hostCh[2] = { mHostL.data(), mHostR.data() };
        mMetering.processBuffer(hostCh, 2, n, mc_wasm::MeteringService::Source::Host);
        const float* plCh[2] = { mPlL.data(), mPlR.data() };
        mMetering.processBuffer(plCh, 2, n, mc_wasm::MeteringService::Source::Playlist);
    }

    // --- source blend (0=Host, 1=Playlist) ---
    mSourceBlendSmoothed.setTargetValue(mSourceSelect.load(std::memory_order_relaxed) == 0 ? 0.0f : 1.0f);
    for (int i = 0; i < n; ++i)
    {
        const float mix = mSourceBlendSmoothed.getNextValue();
        const float inv = 1.0f - mix;
        outL[i] = mHostL[i] * inv + mPlL[i] * mix;
        outR[i] = mHostR[i] * inv + mPlR[i] * mix;
    }

    // --- LPF (24dB/oct, enable 時にミックスフェード) ---
    const bool lpfEnabled = mLpfEnabled.load(std::memory_order_relaxed);
    const float lpfFreq = mLpfFreqHz.load(std::memory_order_relaxed);
    mLpfLeft.setCutoffFrequency(lpfFreq);
    mLpfRight.setCutoffFrequency(lpfFreq);
    if (lpfEnabled && !mLastLpfEnabled) { mLpfLeft.reset(); mLpfRight.reset(); }
    mLastLpfEnabled = lpfEnabled;
    mLpfMixSmoothed.setTargetValue(lpfEnabled ? 1.0f : 0.0f);

    if (lpfEnabled || mLpfMixSmoothed.isSmoothing())
    {
        for (int i = 0; i < n; ++i)
        {
            mFiltL[i] = mLpfLeft.processSample(outL[i]);
            mFiltR[i] = mLpfRight.processSample(outR[i]);
        }
        for (int i = 0; i < n; ++i)
        {
            const float mix = mLpfMixSmoothed.getNextValue();
            const float inv = 1.0f - mix;
            outL[i] = outL[i] * inv + mFiltL[i] * mix;
            outR[i] = outR[i] * inv + mFiltR[i] * mix;
        }
    }

    // --- 出力メータ ---
    {
        const float* outCh[2] = { outL, outR };
        mMetering.processBuffer(outCh, 2, n, mc_wasm::MeteringService::Source::Output);
    }

    // --- メータリセット要求 (UI からの非同期要求を audio thread 側で適用) ---
    if (mResetTruePeak.exchange(false, std::memory_order_relaxed))
        mMetering.resetTruePeakMeters();
    if (mResetMomentaryHold.exchange(false, std::memory_order_relaxed))
        mMetering.resetMomentaryHold();

    // --- UI レートで meter snapshot を更新 ---
    mMeterCounter += n;
    if (mMeterCounter >= mMeterInterval)
    {
        mMeterCounter -= mMeterInterval;
        fillMeterSnapshot();
    }
}

namespace
{
inline float gainToDb(float linear, float minDb = -70.0f)
{
    if (linear <= 0.0f) return minDb;
    float db = 20.0f * std::log10(linear);
    return db < minDb ? minDb : db;
}
} // namespace

// Web デモ dsp_get_meter_data と同一レイアウトで snapshot を埋める。
void AudioEngine::fillMeterSnapshot() noexcept
{
    using Src = mc_wasm::MeteringService::Source;
    const int mode = mMeteringMode.load(std::memory_order_relaxed);
    const bool isPeak = (mode == 0);
    const bool isMomentary = (mode == 2);

    float tmp[kMeterFloats];
    tmp[0] = static_cast<float>(mode);

    auto fillSource = [&](Src src, float* dst)
    {
        auto vals = mMetering.getMeterValues(src);
        if (isPeak)
        {
            float tpL = mMetering.getTruePeakLevelAndDecay(src, 0);
            float tpR = mMetering.getTruePeakLevelAndDecay(src, 1);
            dst[0] = gainToDb(tpL);
            dst[1] = gainToDb(tpR);
            dst[2] = gainToDb(tpL);
            dst[3] = gainToDb(tpR);
        }
        else
        {
            dst[0] = gainToDb(vals.rmsLeft);
            dst[1] = gainToDb(vals.rmsRight);
            dst[2] = gainToDb(mMetering.getTruePeakLevelAndDecay(src, 0));
            dst[3] = gainToDb(mMetering.getTruePeakLevelAndDecay(src, 1));
        }
        dst[4] = gainToDb(vals.rmsLeft);
        dst[5] = gainToDb(vals.rmsRight);
        dst[6] = isMomentary ? vals.momentaryLKFS : -70.0f;
        dst[7] = isMomentary ? vals.momentaryHoldLKFS : -70.0f;
    };

    fillSource(Src::Host, tmp + 1);     // [1..8]
    fillSource(Src::Playlist, tmp + 9); // [9..16]

    auto outVals = mMetering.getMeterValues(Src::Output);
    tmp[17] = gainToDb(outVals.rmsLeft);
    tmp[18] = gainToDb(outVals.rmsRight);
    tmp[19] = isMomentary ? outVals.momentaryLKFS : -70.0f;
    tmp[20] = isMomentary ? outVals.momentaryHoldLKFS : -70.0f;
    tmp[21] = tmp[22] = tmp[23] = 0.0f;

    for (int i = 0; i < kMeterFloats; ++i)
        mMeterSnapshot[i].store(tmp[i], std::memory_order_relaxed);
}

} // namespace mc
