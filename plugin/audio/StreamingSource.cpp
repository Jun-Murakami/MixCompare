// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#include "StreamingSource.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace mc
{

void StreamingSource::prepareToPlay(double hostSampleRate)
{
  const double hs = hostSampleRate > 0.0 ? hostSampleRate : 44100.0;
  if (mPrepared && std::abs(hs - mHostSR) < 0.5) return; // 同 SR なら再準備不要
  release();

  mHostSR = hs;
  if (!mDec.open(mPath))
  {
    mPrepared = false;
    mDurationSec = 0.0;
    return;
  }
  mFileSR = mDec.sampleRate();
  mTotalFrames = mDec.totalFrames();
  mDurationSec = (mFileSR > 0.0) ? static_cast<double>(mTotalFrames) / mFileSR : 0.0;
  mRate = (mHostSR > 0.0) ? mFileSR / mHostSR : 1.0;

  // chunk pool + queues
  mChunks.clear();
  mChunks.resize(kNumChunks);
  for (auto& c : mChunks) { c.L.assign(kChunkFrames, 0.0f); c.R.assign(kChunkFrames, 0.0f); c.frames = 0; }
  uint32_t cap = 1; while (cap < static_cast<uint32_t>(kNumChunks) + 1) cap <<= 1;
  mFreeQ.init(cap);
  mReadyQ.init(cap);
  for (uint32_t i = 0; i < static_cast<uint32_t>(kNumChunks); ++i) mFreeQ.push(i);

  // producer state
  mProducerEpoch = 0;
  mResampleSrcPos = 0.0;
  mPfL.assign(static_cast<size_t>(kFileReadBlk) * 4, 0.0f);
  mPfR.assign(static_cast<size_t>(kFileReadBlk) * 4, 0.0f);
  mPfBaseFrame = 0;
  mPfCount = 0;
  mProducerStreamEof = false;
  mProducerEof.store(false, std::memory_order_relaxed);

  // consumer state
  mReadEpoch = 0;
  mCurChunk = -1;
  mCurChunkPos = 0;
  mSeekBaseFrame = 0.0;
  mHostConsumed = 0;

  mSeekEpoch.store(0, std::memory_order_relaxed);
  mSeekFrame.store(0, std::memory_order_relaxed);
  mReportPosSec.store(0.0, std::memory_order_relaxed);

  mStop.store(false, std::memory_order_relaxed);
  mPrepared = true;
  mWorker = std::thread([this]() { workerLoop(); });
}

void StreamingSource::release()
{
  mStop.store(true, std::memory_order_relaxed);
  if (mWorker.joinable()) mWorker.join();
  mDec.close();
  mPrepared = false;
}

// ---- producer (worker thread) ----
void StreamingSource::workerLoop()
{
  while (!mStop.load(std::memory_order_relaxed))
  {
    // seek 反映 (consumer が epoch を進めたら再シーク)
    const uint64_t se = mSeekEpoch.load(std::memory_order_acquire);
    if (se != mProducerEpoch)
    {
      const int64_t sf = mSeekFrame.load(std::memory_order_relaxed);
      mDec.seek(sf);
      mProducerEpoch = se;
      mResampleSrcPos = static_cast<double>(sf);
      mPfBaseFrame = sf;
      mPfCount = 0;
      mProducerStreamEof = false;
      mProducerEof.store(false, std::memory_order_relaxed);
    }

    bool didWork = false;
    if (!mProducerStreamEof)
    {
      uint32_t idx;
      if (mFreeQ.pop(idx))
      {
        produceChunk(mChunks[idx]);
        mReadyQ.push(idx);
        didWork = true;
      }
    }
    if (!didWork)
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

bool StreamingSource::refillFileBuffer(int64_t needFrame)
{
  // 先頭の消費済みフレームを捨てて (sliding window)、needFrame を含むまでデコードする。
  const int64_t i0 = static_cast<int64_t>(std::floor(mResampleSrcPos));
  const int64_t dropTo = i0 - mPfBaseFrame;
  if (dropTo > 0 && dropTo <= mPfCount)
  {
    const int remaining = mPfCount - static_cast<int>(dropTo);
    if (remaining > 0)
    {
      std::memmove(mPfL.data(), mPfL.data() + dropTo, sizeof(float) * static_cast<size_t>(remaining));
      std::memmove(mPfR.data(), mPfR.data() + dropTo, sizeof(float) * static_cast<size_t>(remaining));
    }
    mPfBaseFrame += dropTo;
    mPfCount = (remaining > 0) ? remaining : 0;
  }

  while (mPfBaseFrame + mPfCount <= needFrame)
  {
    if (static_cast<size_t>(mPfCount + kFileReadBlk) > mPfL.size())
    {
      mPfL.resize(static_cast<size_t>(mPfCount + kFileReadBlk));
      mPfR.resize(static_cast<size_t>(mPfCount + kFileReadBlk));
    }
    const int got = mDec.read(mPfL.data() + mPfCount, mPfR.data() + mPfCount, kFileReadBlk);
    if (got <= 0)
      return (mPfBaseFrame + mPfCount) > needFrame; // eof
    mPfCount += got;
  }
  return true;
}

void StreamingSource::produceChunk(Chunk& c)
{
  c.epoch = mProducerEpoch;
  c.isLast = false;
  int k = 0;
  for (; k < kChunkFrames; ++k)
  {
    const int64_t i0 = static_cast<int64_t>(std::floor(mResampleSrcPos));
    if (mTotalFrames > 0 && i0 >= mTotalFrames)
    {
      c.isLast = true; mProducerStreamEof = true; mProducerEof.store(true, std::memory_order_relaxed);
      break;
    }
    if (!refillFileBuffer(i0 + 1))
    {
      // デコーダ eof: 利用可能分まで出して終端。
      if (i0 - mPfBaseFrame >= mPfCount) { c.isLast = true; mProducerStreamEof = true; mProducerEof.store(true, std::memory_order_relaxed); break; }
    }
    const int rel = static_cast<int>(i0 - mPfBaseFrame);
    if (rel < 0 || rel >= mPfCount) { c.isLast = true; mProducerStreamEof = true; mProducerEof.store(true, std::memory_order_relaxed); break; }
    const float frac = static_cast<float>(mResampleSrcPos - static_cast<double>(i0));
    const float l0 = mPfL[rel];
    const float r0 = mPfR[rel];
    const float l1 = (rel + 1 < mPfCount) ? mPfL[rel + 1] : l0;
    const float r1 = (rel + 1 < mPfCount) ? mPfR[rel + 1] : r0;
    c.L[k] = l0 + (l1 - l0) * frac;
    c.R[k] = r0 + (r1 - r0) * frac;
    mResampleSrcPos += mRate;
  }
  c.frames = k;
}

// ---- consumer (audio thread) ----
void StreamingSource::readBlock(float* outL, float* outR, int n, bool& reachedEnd) noexcept
{
  reachedEnd = false;

  if (!mPrepared || mDurationSec <= 0.0)
  {
    std::memset(outL, 0, sizeof(float) * static_cast<size_t>(n));
    std::memset(outR, 0, sizeof(float) * static_cast<size_t>(n));
    reachedEnd = true; // 失敗ソースは即終了扱い
    return;
  }

  // seek epoch 変化を反映 (外部 seekSeconds / ループ seek 共通)。
  const uint64_t se = mSeekEpoch.load(std::memory_order_acquire);
  if (se != mReadEpoch)
  {
    mReadEpoch = se;
    mSeekBaseFrame = static_cast<double>(mSeekFrame.load(std::memory_order_relaxed));
    mHostConsumed = 0;
    if (mCurChunk >= 0) { mFreeQ.push(static_cast<uint32_t>(mCurChunk)); mCurChunk = -1; }
  }

  const bool loop = mLoopOn.load(std::memory_order_relaxed);
  const double loopStartF = mLoopStartSec.load(std::memory_order_relaxed) * mFileSR;
  double loopEndF = mLoopEndSec.load(std::memory_order_relaxed) * mFileSR;
  if (loopEndF <= 0.0 || loopEndF > static_cast<double>(mTotalFrames))
    loopEndF = static_cast<double>(mTotalFrames);

  for (int i = 0; i < n; ++i)
  {
    const double fileFrame = mSeekBaseFrame + static_cast<double>(mHostConsumed) * mRate;

    // ループ: 終端到達で loopStart へ seek (次ブロックから新データ)。残りは無音。
    if (loop && loopEndF > loopStartF && fileFrame >= loopEndF)
    {
      seekSeconds(mLoopStartSec.load(std::memory_order_relaxed));
      for (int j = i; j < n; ++j) { outL[j] = 0.0f; outR[j] = 0.0f; }
      return;
    }

    // 現在 chunk が無ければ ready から取得 (古い epoch は破棄)。
    if (mCurChunk < 0)
    {
      uint32_t idx;
      bool got = false;
      while (mReadyQ.pop(idx))
      {
        if (mChunks[idx].epoch == mReadEpoch && mChunks[idx].frames > 0)
        {
          mCurChunk = static_cast<int>(idx);
          mCurChunkPos = 0;
          got = true;
          break;
        }
        mFreeQ.push(idx); // stale or empty
      }
      if (!got)
      {
        // データ無し: 終端 (eof+drain) か underrun。
        if (!loop && mProducerEof.load(std::memory_order_relaxed))
        {
          reachedEnd = true;
          for (int j = i; j < n; ++j) { outL[j] = 0.0f; outR[j] = 0.0f; }
          mReportPosSec.store(fileFrame / mFileSR, std::memory_order_relaxed);
          return;
        }
        // underrun: 無音 (位置は進めない)。
        outL[i] = 0.0f; outR[i] = 0.0f;
        continue;
      }
    }

    const Chunk& c = mChunks[mCurChunk];
    outL[i] = c.L[mCurChunkPos];
    outR[i] = c.R[mCurChunkPos];
    ++mCurChunkPos;
    ++mHostConsumed;
    if (mCurChunkPos >= c.frames)
    {
      mFreeQ.push(static_cast<uint32_t>(mCurChunk));
      mCurChunk = -1;
    }
  }

  const double endFrame = mSeekBaseFrame + static_cast<double>(mHostConsumed) * mRate;
  mReportPosSec.store(endFrame / mFileSR, std::memory_order_relaxed);
}

void StreamingSource::seekSeconds(double sec) noexcept
{
  if (sec < 0.0) sec = 0.0;
  mSeekFrame.store(static_cast<int64_t>(sec * mFileSR), std::memory_order_relaxed);
  mSeekEpoch.fetch_add(1, std::memory_order_release);
  mReportPosSec.store(sec, std::memory_order_relaxed);
}

void StreamingSource::setLoop(double startSec, double endSec, bool enabled) noexcept
{
  mLoopStartSec.store(startSec, std::memory_order_relaxed);
  mLoopEndSec.store(endSec, std::memory_order_relaxed);
  mLoopOn.store(enabled, std::memory_order_relaxed);
}

} // namespace mc
