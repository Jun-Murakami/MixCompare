// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
//
// ネイティブのオーディオファイルデコーダ (JUCE 非依存)。
//   WAV/FLAC/MP3 = dr_libs、OGG = stb_vorbis (いずれもパブリックドメイン)。
//   AAC/M4A は Phase 4 で OS ネイティブ (Media Foundation / AudioToolbox)、
//   APE は vendored MACLib を追加予定。
//
// Phase 2 では「追加時に全 PCM をデコードしてメモリ保持」する方針 (WASM 版 Track と
// 同じ in-memory モデル)。長尺のストリーミングは Phase 3。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mc
{

struct DecodedAudio
{
  std::vector<float> left;   // deinterleaved L
  std::vector<float> right;  // deinterleaved R (mono は L を複製)
  double  sampleRate = 0.0;
  int64_t numFrames  = 0;
  double  durationSec = 0.0;
  bool    ok = false;
  std::string error;
};

/// ヘッダのみ読む軽量な情報取得 (ストリーミングトラックの duration 用)。
struct AudioInfo
{
  double  sampleRate = 0.0;
  int64_t numFrames  = 0;
  double  durationSec = 0.0;
  bool    ok = false;
};

namespace AudioDecoder
{
  /// 拡張子が対応フォーマットか (wav/flac/mp3/ogg/m4a/aac/mp4/ape)。
  bool isSupported(const std::string& pathUtf8);

  /// StreamingSource (逐次再生) に対応する拡張子か (wav/flac/mp3/ogg)。
  /// m4a/aac/ape は full decode (in-memory) のみ。
  bool isStreamable(const std::string& pathUtf8);

  /// ファイル全体を float L/R にデコードする。失敗時は ok=false + error。
  /// 呼び出しスレッド制約なし (ワーカースレッドから呼ぶ想定。audio thread からは呼ばない)。
  DecodedAudio decodeFull(const std::string& pathUtf8);

  /// sampleRate / numFrames / duration だけを取得 (全 PCM は展開しない)。
  AudioInfo probe(const std::string& pathUtf8);

  /// ファイルサイズ (bytes)。見つからなければ -1。in-memory/streaming の判定に使う。
  int64_t fileSizeBytes(const std::string& pathUtf8);
}

} // namespace mc
