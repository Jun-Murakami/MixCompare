// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
//
// 再生ソースの抽象 (JUCE 非依存)。短尺=全展開(InMemorySource)、長尺=ストリーミング
// (StreamingSource) の差を吸収し、AudioEngine から一貫した API で扱う。
//
// readBlock はオーディオスレッドから呼ばれる (RT 安全: alloc/lock/log 禁止)。
// prepareToPlay / release は select 時に UI/worker スレッドから呼ぶ。
// seek/loop はオーディオ or UI スレッドから呼ばれうるため atomic で受ける。
// 出力は **ホスト SR** にリサンプル済みのフレームを返す。
#pragma once

#include <cstdint>
#include <vector>

namespace mc
{

/// デコード済みフルトラック (InMemorySource が保持)。worker で生成し immutable に扱う。
struct TrackAudio
{
  std::vector<float> left;
  std::vector<float> right;
  double  sampleRate = 44100.0;
  int64_t numFrames  = 0;
  double  durationSec = 0.0;
};

class IPlaybackSource
{
public:
  virtual ~IPlaybackSource() = default;

  /// 再生開始前の準備 (デコーダ open / worker 起動など)。ホスト SR を渡す。
  virtual void prepareToPlay(double hostSampleRate) = 0;
  /// リソース解放 (worker join など)。dtor からも呼べるよう冪等にする。
  virtual void release() = 0;

  /// n フレーム分をホスト SR で outL/outR に書き込む (RT 安全)。
  /// 非ループで終端に達したら reachedEnd=true。
  virtual void readBlock(float* outL, float* outR, int n, bool& reachedEnd) noexcept = 0;

  virtual void seekSeconds(double sec) noexcept = 0;
  virtual void setLoop(double startSec, double endSec, bool enabled) noexcept = 0;

  virtual double getPositionSec() const noexcept = 0;
  virtual double getDurationSec() const noexcept = 0;
};

} // namespace mc
