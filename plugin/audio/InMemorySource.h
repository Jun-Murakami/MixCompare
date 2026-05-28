// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
//
// 全展開済みトラックを再生する IPlaybackSource (短尺向け)。
// readBlock 内で線形補間リサンプル + ループ + シークを行う (RT 安全、スレッドなし)。
#pragma once

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "IPlaybackSource.h"

namespace mc
{

class InMemorySource final : public IPlaybackSource
{
public:
  explicit InMemorySource(std::shared_ptr<TrackAudio> track) : mTrack(std::move(track)) {}

  void prepareToPlay(double hostSampleRate) override
  {
    mHostSR = hostSampleRate > 0.0 ? hostSampleRate : 44100.0;
  }
  void release() override {}

  void readBlock(float* outL, float* outR, int n, bool& reachedEnd) noexcept override
  {
    reachedEnd = false;

    const double seek = mSeekReq.exchange(-1.0, std::memory_order_relaxed);
    if (mTrack && seek >= 0.0)
      mPos = seek * mTrack->sampleRate;

    if (!mTrack || mTrack->numFrames <= 0)
    {
      std::memset(outL, 0, sizeof(float) * static_cast<size_t>(n));
      std::memset(outR, 0, sizeof(float) * static_cast<size_t>(n));
      return;
    }

    const int64_t nF = mTrack->numFrames;
    const double rate = (mHostSR > 0.0) ? mTrack->sampleRate / mHostSR : 1.0;
    const bool loop = mLoopOn.load(std::memory_order_relaxed);
    const double loopStartF = mLoopStartSec.load(std::memory_order_relaxed) * mTrack->sampleRate;
    double loopEndF = mLoopEndSec.load(std::memory_order_relaxed) * mTrack->sampleRate;
    if (loopEndF <= 0.0 || loopEndF > static_cast<double>(nF))
      loopEndF = static_cast<double>(nF);

    const float* L = mTrack->left.data();
    const float* R = mTrack->right.data();

    for (int i = 0; i < n; ++i)
    {
      if (loop && loopEndF > loopStartF && mPos >= loopEndF)
      {
        const double len = loopEndF - loopStartF;
        mPos = loopStartF + std::fmod(mPos - loopStartF, len);
      }
      else if (!loop && mPos >= static_cast<double>(nF))
      {
        reachedEnd = true;
        for (int j = i; j < n; ++j) { outL[j] = 0.0f; outR[j] = 0.0f; }
        break;
      }
      int64_t i0 = static_cast<int64_t>(mPos);
      if (i0 < 0) i0 = 0;
      if (i0 >= nF) i0 = nF - 1;
      const int64_t i1 = (i0 + 1 < nF) ? i0 + 1 : i0;
      const float frac = static_cast<float>(mPos - static_cast<double>(i0));
      outL[i] = L[i0] + (L[i1] - L[i0]) * frac;
      outR[i] = R[i0] + (R[i1] - R[i0]) * frac;
      mPos += rate;
    }

    mReportPos.store(mPos / mTrack->sampleRate, std::memory_order_relaxed);
  }

  void seekSeconds(double sec) noexcept override
  {
    mSeekReq.store(sec, std::memory_order_relaxed);
    mReportPos.store(sec, std::memory_order_relaxed); // pause 中シークでも UI 即反映
  }
  void setLoop(double startSec, double endSec, bool enabled) noexcept override
  {
    mLoopStartSec.store(startSec, std::memory_order_relaxed);
    mLoopEndSec.store(endSec, std::memory_order_relaxed);
    mLoopOn.store(enabled, std::memory_order_relaxed);
  }
  double getPositionSec() const noexcept override { return mReportPos.load(std::memory_order_relaxed); }
  double getDurationSec() const noexcept override { return mTrack ? mTrack->durationSec : 0.0; }

private:
  std::shared_ptr<TrackAudio> mTrack;
  double mHostSR = 44100.0;
  double mPos = 0.0; // audio thread のみ (ファイルフレーム)
  std::atomic<double> mSeekReq { -1.0 };
  std::atomic<bool>   mLoopOn { false };
  std::atomic<double> mLoopStartSec { 0.0 };
  std::atomic<double> mLoopEndSec { 0.0 };
  std::atomic<double> mReportPos { 0.0 };
};

} // namespace mc
