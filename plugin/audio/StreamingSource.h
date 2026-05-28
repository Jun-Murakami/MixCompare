// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
//
// ストリーミング再生ソース (長尺向け、低メモリ)。
//   - worker スレッドが StreamingDecoder で増分デコードし、**プロデューサ側で
//     ホスト SR にリサンプル**した host-SR フレームを epoch タグ付き Chunk として
//     SPSC キューで consumer(readBlock) に渡す。
//   - シーク/ループは epoch を進めて producer に再シークさせ、consumer は旧 epoch の
//     Chunk を破棄する (遷移時は短い無音を許容)。
//   - 位置はシーク起点 + 消費 host フレーム × rate で算出 (exact)。
//
// 同時アクセス前提:
//   - readBlock / seekSeconds は所有スレッド(オーディオ)からのみ。setLoop は任意スレッド(atomic)。
//   - prepareToPlay / release は UI/worker スレッド(select 時)。
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "IPlaybackSource.h"
#include "StreamingDecoder.h"

namespace mc
{

class StreamingSource final : public IPlaybackSource
{
public:
  explicit StreamingSource(std::string pathUtf8) : mPath(std::move(pathUtf8)) {}
  ~StreamingSource() override { release(); }

  void prepareToPlay(double hostSampleRate) override;
  void release() override;
  void readBlock(float* outL, float* outR, int n, bool& reachedEnd) noexcept override;
  void seekSeconds(double sec) noexcept override;
  void setLoop(double startSec, double endSec, bool enabled) noexcept override;
  double getPositionSec() const noexcept override { return mReportPosSec.load(std::memory_order_relaxed); }
  double getDurationSec() const noexcept override { return mDurationSec; }

private:
  static constexpr int kChunkFrames = 8192;  // host-SR frames / chunk
  static constexpr int kNumChunks   = 24;    // ~4s @48k buffer
  static constexpr int kFileReadBlk = 8192;  // file frames / decoder read

  struct Chunk
  {
    std::vector<float> L, R;
    int frames = 0;
    uint64_t epoch = 0;
    bool isLast = false;
  };

  // 単一生産者・単一消費者のロックフリー index キュー (容量は 2 冪)。
  class SpscQueue
  {
  public:
    void init(uint32_t cap) { mCap = cap; mMask = cap - 1; mBuf.assign(cap, 0); mHead.store(0); mTail.store(0); }
    bool push(uint32_t v) noexcept
    {
      const uint32_t t = mTail.load(std::memory_order_relaxed);
      const uint32_t h = mHead.load(std::memory_order_acquire);
      if (t - h >= mCap) return false; // full
      mBuf[t & mMask] = v;
      mTail.store(t + 1, std::memory_order_release);
      return true;
    }
    bool pop(uint32_t& out) noexcept
    {
      const uint32_t h = mHead.load(std::memory_order_relaxed);
      const uint32_t t = mTail.load(std::memory_order_acquire);
      if (h == t) return false; // empty
      out = mBuf[h & mMask];
      mHead.store(h + 1, std::memory_order_release);
      return true;
    }
  private:
    std::vector<uint32_t> mBuf;
    uint32_t mCap = 0, mMask = 0;
    std::atomic<uint32_t> mHead{0}, mTail{0};
  };

  void workerLoop();
  void produceChunk(Chunk& c);          // worker: 1 chunk 分を resample 生成
  bool refillFileBuffer(int64_t needFrame); // worker: file-frame sliding window を埋める

  std::string mPath;
  double mHostSR = 44100.0;
  double mFileSR = 44100.0;
  int64_t mTotalFrames = 0;
  double mDurationSec = 0.0;
  double mRate = 1.0; // fileSR / hostSR

  StreamingDecoder mDec;
  std::thread mWorker;
  std::atomic<bool> mStop{false};
  bool mPrepared = false;

  // chunk pool + queues
  std::vector<Chunk> mChunks;
  SpscQueue mFreeQ;   // consumer→producer (空き)
  SpscQueue mReadyQ;  // producer→consumer (充填済み)

  // seek/epoch (atomic)
  std::atomic<uint64_t> mSeekEpoch{0};
  std::atomic<int64_t>  mSeekFrame{0};
  std::atomic<bool>     mProducerEof{false};

  // loop (atomic)
  std::atomic<bool>   mLoopOn{false};
  std::atomic<double> mLoopStartSec{0.0};
  std::atomic<double> mLoopEndSec{0.0};

  std::atomic<double> mReportPosSec{0.0};

  // ---- consumer (readBlock / seekSeconds = audio thread) のみ ----
  uint64_t mReadEpoch = 0;
  int      mCurChunk = -1;     // 現在消費中の chunk index (-1 = なし)
  int      mCurChunkPos = 0;
  double   mSeekBaseFrame = 0.0; // 現区間のシーク起点 (file frame)
  int64_t  mHostConsumed = 0;    // シーク起点からの host フレーム消費数

  // ---- producer (worker) のみ ----
  uint64_t mProducerEpoch = 0;
  double   mResampleSrcPos = 0.0; // fractional file frame
  std::vector<float> mPfL, mPfR;  // file-frame sliding window
  int64_t  mPfBaseFrame = 0;      // mPfL[0] の file frame
  int      mPfCount = 0;
  bool     mProducerStreamEof = false;
};

} // namespace mc
