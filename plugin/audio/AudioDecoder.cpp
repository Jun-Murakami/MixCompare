// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS  // _wfopen 等の deprecation 警告を抑制
#endif
#include "AudioDecoder.h"
#include "StreamingDecoder.h"
#include "MediaFoundationDecoder.h"
#include "AudioToolboxDecoder.h"
#include "MonkeyAudioDecoder.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#endif

// dr_libs の実装はこの C++ TU に埋め込む (単一ヘッダライブラリ)。
#define DR_WAV_IMPLEMENTATION
#include "../lib/decoders/dr_wav.h"
#define DR_FLAC_IMPLEMENTATION
#include "../lib/decoders/dr_flac.h"
#define DR_MP3_IMPLEMENTATION
#include "../lib/decoders/dr_mp3.h"

// stb_vorbis: 実装は stb_vorbis.c を C ソースとして別途コンパイルする
// (CMake で plugin/lib/decoders/stb_vorbis.c を追加)。ここでは宣言のみ取り込む。
// stb_vorbis.c は内部に extern "C" ガードを持つので C++ からそのまま参照できる。
#define STB_VORBIS_HEADER_ONLY
#include "../lib/decoders/stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY

namespace mc
{
namespace
{

std::string lowerExt(const std::string& path)
{
  const auto dot = path.find_last_of('.');
  if (dot == std::string::npos) return {};
  std::string ext = path.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext;
}

// UTF-8 パスを Unicode 安全に読み込む (Windows の日本語/OneDrive パス対応)。
std::vector<uint8_t> readFileBytes(const std::string& pathUtf8)
{
  std::vector<uint8_t> bytes;
  FILE* fp = nullptr;
#ifdef _WIN32
  const int wlen = MultiByteToWideChar(CP_UTF8, 0, pathUtf8.c_str(),
                                       static_cast<int>(pathUtf8.size()), nullptr, 0);
  if (wlen > 0)
  {
    std::wstring wpath(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, pathUtf8.c_str(), static_cast<int>(pathUtf8.size()),
                        wpath.data(), wlen);
    fp = _wfopen(wpath.c_str(), L"rb");
  }
#else
  fp = std::fopen(pathUtf8.c_str(), "rb");
#endif
  if (!fp) return bytes;

  std::fseek(fp, 0, SEEK_END);
  const long size = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);
  if (size > 0)
  {
    bytes.resize(static_cast<size_t>(size));
    const size_t got = std::fread(bytes.data(), 1, bytes.size(), fp);
    if (got != bytes.size()) bytes.resize(got);
  }
  std::fclose(fp);
  return bytes;
}

void deinterleaveF32(const float* inter, unsigned int channels, int64_t frames, DecodedAudio& out)
{
  out.left.resize(static_cast<size_t>(frames));
  out.right.resize(static_cast<size_t>(frames));
  if (channels >= 2)
  {
    for (int64_t i = 0; i < frames; ++i)
    {
      out.left[i]  = inter[i * channels + 0];
      out.right[i] = inter[i * channels + 1];
    }
  }
  else // mono → 両チャンネルに複製
  {
    for (int64_t i = 0; i < frames; ++i)
      out.left[i] = out.right[i] = inter[i];
  }
}

} // namespace

bool AudioDecoder::isSupported(const std::string& pathUtf8)
{
  const std::string ext = lowerExt(pathUtf8);
  return ext == "wav" || ext == "flac" || ext == "mp3" || ext == "ogg"
      || ext == "m4a" || ext == "aac" || ext == "mp4" || ext == "ape";
}

bool AudioDecoder::isStreamable(const std::string& pathUtf8)
{
  const std::string ext = lowerExt(pathUtf8);
  return ext == "wav" || ext == "flac" || ext == "mp3" || ext == "ogg";
}

DecodedAudio AudioDecoder::decodeFull(const std::string& pathUtf8)
{
  DecodedAudio out;
  const std::string ext = lowerExt(pathUtf8);

  std::vector<uint8_t> bytes = readFileBytes(pathUtf8);
  if (bytes.empty())
  {
    out.error = "file not found or empty";
    return out;
  }

  if (ext == "wav")
  {
    unsigned int ch = 0, sr = 0;
    drwav_uint64 frames = 0;
    float* pcm = drwav_open_memory_and_read_pcm_frames_f32(
        bytes.data(), bytes.size(), &ch, &sr, &frames, nullptr);
    if (!pcm) { out.error = "wav decode failed"; return out; }
    deinterleaveF32(pcm, ch, static_cast<int64_t>(frames), out);
    drwav_free(pcm, nullptr);
    out.sampleRate = sr;
    out.numFrames = static_cast<int64_t>(frames);
  }
  else if (ext == "flac")
  {
    unsigned int ch = 0, sr = 0;
    drflac_uint64 frames = 0;
    float* pcm = drflac_open_memory_and_read_pcm_frames_f32(
        bytes.data(), bytes.size(), &ch, &sr, &frames, nullptr);
    if (!pcm) { out.error = "flac decode failed"; return out; }
    deinterleaveF32(pcm, ch, static_cast<int64_t>(frames), out);
    drflac_free(pcm, nullptr);
    out.sampleRate = sr;
    out.numFrames = static_cast<int64_t>(frames);
  }
  else if (ext == "mp3")
  {
    drmp3_config cfg{};
    drmp3_uint64 frames = 0;
    float* pcm = drmp3_open_memory_and_read_pcm_frames_f32(
        bytes.data(), bytes.size(), &cfg, &frames, nullptr);
    if (!pcm) { out.error = "mp3 decode failed"; return out; }
    deinterleaveF32(pcm, cfg.channels, static_cast<int64_t>(frames), out);
    drmp3_free(pcm, nullptr);
    out.sampleRate = cfg.sampleRate;
    out.numFrames = static_cast<int64_t>(frames);
  }
  else if (ext == "ogg")
  {
    int ch = 0, sr = 0;
    short* pcm = nullptr;
    const int frames = stb_vorbis_decode_memory(
        bytes.data(), static_cast<int>(bytes.size()), &ch, &sr, &pcm);
    if (frames < 0 || !pcm) { out.error = "ogg decode failed"; return out; }
    // short interleaved → float deinterleaved。
    out.left.resize(static_cast<size_t>(frames));
    out.right.resize(static_cast<size_t>(frames));
    constexpr float kInv = 1.0f / 32768.0f;
    if (ch >= 2)
    {
      for (int i = 0; i < frames; ++i)
      {
        out.left[i]  = pcm[i * ch + 0] * kInv;
        out.right[i] = pcm[i * ch + 1] * kInv;
      }
    }
    else
    {
      for (int i = 0; i < frames; ++i)
        out.left[i] = out.right[i] = pcm[i] * kInv;
    }
    free(pcm);
    out.sampleRate = sr;
    out.numFrames = frames;
  }
  else if (ext == "m4a" || ext == "aac" || ext == "mp4")
  {
#ifdef _WIN32
    decodeMediaFoundation(pathUtf8, out); // out を完全に埋める (失敗時 ok=false)
#elif defined(__APPLE__)
    decodeAudioToolbox(pathUtf8, out);
#else
    out.error = "aac/m4a decode not supported on this platform";
#endif
    return out;
  }
  else if (ext == "ape")
  {
    decodeMonkeyAudio(pathUtf8, out); // out を完全に埋める
    return out;
  }
  else
  {
    out.error = "unsupported format: " + ext;
    return out;
  }

  if (out.sampleRate <= 0.0 || out.numFrames <= 0)
  {
    out.error = "decoded but empty/invalid";
    return out;
  }
  out.durationSec = static_cast<double>(out.numFrames) / out.sampleRate;
  out.ok = true;
  return out;
}

AudioInfo AudioDecoder::probe(const std::string& pathUtf8)
{
  AudioInfo info;
  StreamingDecoder dec;
  if (!dec.open(pathUtf8)) return info;
  info.sampleRate = dec.sampleRate();
  info.numFrames = dec.totalFrames();
  info.durationSec = (info.sampleRate > 0.0) ? static_cast<double>(info.numFrames) / info.sampleRate : 0.0;
  info.ok = (info.sampleRate > 0.0 && info.numFrames > 0);
  dec.close();
  return info;
}

int64_t AudioDecoder::fileSizeBytes(const std::string& pathUtf8)
{
  FILE* fp = nullptr;
#ifdef _WIN32
  const int wlen = MultiByteToWideChar(CP_UTF8, 0, pathUtf8.c_str(),
                                       static_cast<int>(pathUtf8.size()), nullptr, 0);
  if (wlen > 0)
  {
    std::wstring wpath(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, pathUtf8.c_str(), static_cast<int>(pathUtf8.size()),
                        wpath.data(), wlen);
    fp = _wfopen(wpath.c_str(), L"rb");
  }
#else
  fp = std::fopen(pathUtf8.c_str(), "rb");
#endif
  if (!fp) return -1;
  std::fseek(fp, 0, SEEK_END);
  const long sz = std::ftell(fp);
  std::fclose(fp);
  return static_cast<int64_t>(sz);
}

} // namespace mc
