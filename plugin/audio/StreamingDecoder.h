// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
//
// 増分デコーダ (ストリーミング再生用)。ファイルを開いたまま read/seek する。
// dr_libs の実装は AudioDecoder.cpp 側に 1 か所だけ存在し、ここは宣言のみ参照する。
// WAV/FLAC/MP3 = dr_libs (Unicode は _w 初期化)、OGG = _wfopen + stb_vorbis_open_file。
//
// open/read/seek/close は同一の所有スレッド (StreamingSource の worker) からのみ呼ぶ。
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace mc
{

class StreamingDecoder
{
public:
  ~StreamingDecoder();

  bool open(const std::string& pathUtf8); // 失敗 / 非対応なら false
  void close();

  // frames フレーム分を deinterleaved L/R に読む (mono は両 ch に複製)。実読フレーム数を返す。
  int read(float* outL, float* outR, int frames);
  void seek(int64_t frame);

  bool    ok() const noexcept          { return mOk; }
  double  sampleRate() const noexcept  { return mSampleRate; }
  int64_t totalFrames() const noexcept { return mTotalFrames; }

private:
  enum class Fmt { None, Wav, Flac, Mp3, Ogg };
  Fmt   mFmt = Fmt::None;
  void* mHandle = nullptr; // drwav* / drflac* / drmp3* / stb_vorbis*
  unsigned int mChannels = 2;
  double  mSampleRate = 44100.0;
  int64_t mTotalFrames = 0;
  bool    mOk = false;
  std::vector<float> mInterleaved; // worker 専用の read 一時バッファ
};

} // namespace mc
