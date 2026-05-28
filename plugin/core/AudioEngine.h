// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
//
// MixCompare AudioEngine (iPlug2 移植版)。
//
// WASM 版 DspEngine (wasm/src/dsp_engine.h) と同じ信号フロー
//   Host / Playlist 2 系統 → gain → 個別メータ → source blend → LPF → 出力メータ
// だが、Host は「デモ用内部トラック」ではなく **DAW 入力バス** から供給される点が
// 相違。DSP プリミティブは plugin/dsp/ の共有実装 (mc_wasm::*) を使う。
//
// Playlist パスは Phase 2 で IPlaybackSource を接続する。Phase 1 では無音。
//
// メータ値は Web デモと同一の 24-float レイアウト (dsp_get_meter_data 互換) で
// UI レート (~30Hz) に snapshot し、std::atomic 配列で audio→UI に渡す。
#pragma once

#include <atomic>
#include <memory>
#include <vector>
#include <algorithm>
#include <cmath>

#include "../dsp/svt_filter.h"
#include "../dsp/smoothed_value.h"
#include "../dsp/metering.h"
#include "../audio/IPlaybackSource.h" // TrackAudio + IPlaybackSource

namespace mc
{

class AudioEngine
{
public:
    // Web デモと同一の 24-float メータレイアウト。shim 側のデコードと共有する。
    static constexpr int kMeterFloats = 24;

    void prepare(double sampleRate, int blockSize);
    void reset();

    // --- パラメータ setter (OnParamChange = audio thread から呼ばれる。alloc 禁止) ---
    void setHostGainDb(float db) noexcept       { mHostGainDb.store(db, std::memory_order_relaxed); }
    void setPlaylistGainDb(float db) noexcept   { mPlaylistGainDb.store(db, std::memory_order_relaxed); }
    void setSourceSelect(int src) noexcept      { mSourceSelect.store(src, std::memory_order_relaxed); }
    void setLpfEnabled(bool e) noexcept         { mLpfEnabled.store(e, std::memory_order_relaxed); }
    void setLpfFrequency(float hz) noexcept     { mLpfFreqHz.store(hz, std::memory_order_relaxed); }
    void setMeteringMode(int mode) noexcept     { mMeteringMode.store(mode, std::memory_order_relaxed); }

    // --- メイン処理 (audio thread)。Host は inputs から、出力は outputs へ。---
    void processBlock(const float* inL, const float* inR,
                      float* outL, float* outR, int numSamples) noexcept;

    // --- メータ snapshot 読み出し (UI thread / OnIdle)。out は kMeterFloats 要素。---
    void readMeterSnapshot(float* out) const noexcept
    {
        for (int i = 0; i < kMeterFloats; ++i)
            out[i] = mMeterSnapshot[i].load(std::memory_order_relaxed);
    }

    // メータリセット (UI thread)。次回 snapshot 更新時に audio thread 側で実行する。
    void requestResetTruePeak() noexcept     { mResetTruePeak.store(true, std::memory_order_relaxed); }
    void requestResetMomentaryHold() noexcept{ mResetMomentaryHold.store(true, std::memory_order_relaxed); }

    // ---- Playlist 現在ソース / トランスポート (Playlist パス) ----
    // UI/worker thread から再生ソースを差し替える。新ソースは prepareToPlay + 現在の
    // loop 設定を適用してから atomic swap する。古いソースの解放は「2 世代前」まで
    // 遅延し (mGraveyard)、audio thread が in-flight で読んでいる最中の解放/データ競合を避ける。
    void setCurrentSource(std::shared_ptr<IPlaybackSource> src) noexcept
    {
      if (src)
      {
        src->prepareToPlay(mSampleRate);
        src->setLoop(mLoopStartSec.load(std::memory_order_relaxed),
                     mLoopEndSec.load(std::memory_order_relaxed),
                     mLoopEnabled.load(std::memory_order_relaxed));
      }
      if (mGraveyard) mGraveyard->release(); // 2 世代前 = audio thread はもう参照していない
      mGraveyard = std::atomic_load(&mActiveSource);
      std::atomic_store(&mActiveSource, src);
      mStoppedAtEnd.store(false, std::memory_order_relaxed);
    }
    void clearCurrentSource() noexcept
    {
      mPlaying.store(false, std::memory_order_relaxed);
      setCurrentSource(nullptr);
    }

    void setPlaying(bool p) noexcept              { mPlaying.store(p, std::memory_order_relaxed); }
    bool isPlaying() const noexcept               { return mPlaying.load(std::memory_order_relaxed); }
    void seekSeconds(double sec) noexcept
    {
      auto s = std::atomic_load(&mActiveSource);
      if (s) s->seekSeconds(sec);
    }
    void setLoopEnabled(bool e) noexcept          { mLoopEnabled.store(e, std::memory_order_relaxed); forwardLoopToSource(); }
    void setLoopRangeSec(double s, double e) noexcept
    {
      mLoopStartSec.store(s, std::memory_order_relaxed);
      mLoopEndSec.store(e, std::memory_order_relaxed);
      forwardLoopToSource();
    }
    double getPositionSec() const noexcept
    {
      auto s = std::atomic_load(&mActiveSource);
      return s ? s->getPositionSec() : 0.0;
    }
    double getActiveDurationSec() const noexcept
    {
      auto s = std::atomic_load(&mActiveSource);
      return s ? s->getDurationSec() : 0.0;
    }
    // 再生終端到達を 1 回だけ消費する (UI が Play ボタンを Off に戻す用)。
    bool consumeStoppedAtEnd() noexcept           { return mStoppedAtEnd.exchange(false, std::memory_order_relaxed); }

private:
    void forwardLoopToSource() noexcept
    {
      auto s = std::atomic_load(&mActiveSource);
      if (s) s->setLoop(mLoopStartSec.load(std::memory_order_relaxed),
                        mLoopEndSec.load(std::memory_order_relaxed),
                        mLoopEnabled.load(std::memory_order_relaxed));
    }

    void fillMeterSnapshot() noexcept;

    double mSampleRate = 44100.0;
    int    mCapacity   = 0;

    // パラメータ (UI/automation から書かれ audio で読まれる)
    std::atomic<float> mHostGainDb { 0.0f };
    std::atomic<float> mPlaylistGainDb { 0.0f };
    std::atomic<int>   mSourceSelect { 0 };   // 0=Host, 1=Playlist (APVTS 既定=Host)
    std::atomic<bool>  mLpfEnabled { false };
    std::atomic<float> mLpfFreqHz { 120.0f };
    std::atomic<int>   mMeteringMode { 0 };

    std::atomic<bool>  mResetTruePeak { false };
    std::atomic<bool>  mResetMomentaryHold { false };

    // 共有 DSP プリミティブ
    mc_wasm::StateVariableTPTFilter mLpfLeft;
    mc_wasm::StateVariableTPTFilter mLpfRight;
    mc_wasm::SmoothedValue mHostGainSmoothed { 1.0f };
    mc_wasm::SmoothedValue mPlaylistGainSmoothed { 1.0f };
    mc_wasm::SmoothedValue mSourceBlendSmoothed { 0.0f }; // 0=Host, 1=Playlist
    mc_wasm::SmoothedValue mLpfMixSmoothed { 0.0f };
    mc_wasm::MeteringService mMetering;
    bool mLastLpfEnabled = false;

    // 処理用スクラッチ (prepare で確保。audio thread では alloc しない)
    std::vector<float> mHostL, mHostR, mPlL, mPlR, mFiltL, mFiltR;

    // メータ snapshot (audio→UI)。UI レートで更新。
    std::atomic<float> mMeterSnapshot[kMeterFloats];
    int    mMeterCounter  = 0;
    int    mMeterInterval = 1470; // ~30Hz @44.1k (prepare で再計算)

    // ---- Playlist 現在ソース / トランスポート ----
    std::shared_ptr<IPlaybackSource> mActiveSource; // atomic_load/store で audio⇄UI
    std::shared_ptr<IPlaybackSource> mGraveyard;    // 旧ソース保持 (UI thread のみ。解放遅延)
    std::atomic<bool>   mPlaying { false };
    std::atomic<bool>   mLoopEnabled { false };
    std::atomic<double> mLoopStartSec { 0.0 };
    std::atomic<double> mLoopEndSec { 0.0 };        // 0 = ソース終端まで
    std::atomic<bool>   mStoppedAtEnd { false };
};

} // namespace mc
