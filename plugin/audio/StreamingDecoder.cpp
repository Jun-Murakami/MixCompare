// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "StreamingDecoder.h"

#include <algorithm>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#endif

// dr_libs は宣言のみ (実装は AudioDecoder.cpp)。struct 定義はここで必要。
#include "../lib/decoders/dr_wav.h"
#include "../lib/decoders/dr_flac.h"
#include "../lib/decoders/dr_mp3.h"

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
  std::string e = path.substr(dot + 1);
  std::transform(e.begin(), e.end(), e.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return e;
}
#ifdef _WIN32
std::wstring toWide(const std::string& s)
{
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 0) return {};
  std::wstring w(static_cast<size_t>(n - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}
#endif
} // namespace

StreamingDecoder::~StreamingDecoder() { close(); }

bool StreamingDecoder::open(const std::string& pathUtf8)
{
  close();
  const std::string ext = lowerExt(pathUtf8);
#ifdef _WIN32
  const std::wstring wpath = toWide(pathUtf8);
#endif

  if (ext == "wav")
  {
    auto* w = new drwav();
#ifdef _WIN32
    const bool initialized = drwav_init_file_w(w, wpath.c_str(), nullptr) != 0;
#else
    const bool initialized = drwav_init_file(w, pathUtf8.c_str(), nullptr) != 0;
#endif
    if (!initialized) { delete w; return false; }
    mChannels = w->channels; mSampleRate = w->sampleRate;
    mTotalFrames = static_cast<int64_t>(w->totalPCMFrameCount);
    mHandle = w; mFmt = Fmt::Wav;
  }
  else if (ext == "flac")
  {
#ifdef _WIN32
    drflac* f = drflac_open_file_w(wpath.c_str(), nullptr);
#else
    drflac* f = drflac_open_file(pathUtf8.c_str(), nullptr);
#endif
    if (!f) return false;
    mChannels = f->channels; mSampleRate = f->sampleRate;
    mTotalFrames = static_cast<int64_t>(f->totalPCMFrameCount);
    mHandle = f; mFmt = Fmt::Flac;
  }
  else if (ext == "mp3")
  {
    auto* m = new drmp3();
#ifdef _WIN32
    const bool initialized = drmp3_init_file_w(m, wpath.c_str(), nullptr) != 0;
#else
    const bool initialized = drmp3_init_file(m, pathUtf8.c_str(), nullptr) != 0;
#endif
    if (!initialized) { delete m; return false; }
    mChannels = m->channels; mSampleRate = m->sampleRate;
    mTotalFrames = static_cast<int64_t>(drmp3_get_pcm_frame_count(m));
    mHandle = m; mFmt = Fmt::Mp3;
  }
  else if (ext == "ogg")
  {
#ifdef _WIN32
    FILE* fp = _wfopen(wpath.c_str(), L"rb");
#else
    FILE* fp = std::fopen(pathUtf8.c_str(), "rb");
#endif
    if (!fp) return false;
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_file(fp, 1 /*close_on_free*/, &err, nullptr);
    if (!v) { std::fclose(fp); return false; }
    const stb_vorbis_info info = stb_vorbis_get_info(v);
    mChannels = static_cast<unsigned int>(info.channels);
    mSampleRate = info.sample_rate;
    mTotalFrames = static_cast<int64_t>(stb_vorbis_stream_length_in_samples(v));
    mHandle = v; mFmt = Fmt::Ogg;
  }
  else
  {
    return false;
  }

  if (mChannels == 0) mChannels = 1;
  mOk = (mSampleRate > 0.0 && mTotalFrames > 0);
  return mOk;
}

void StreamingDecoder::close()
{
  if (mHandle)
  {
    switch (mFmt)
    {
      case Fmt::Wav:  drwav_uninit(static_cast<drwav*>(mHandle));  delete static_cast<drwav*>(mHandle);  break;
      case Fmt::Flac: drflac_close(static_cast<drflac*>(mHandle)); break;
      case Fmt::Mp3:  drmp3_uninit(static_cast<drmp3*>(mHandle));  delete static_cast<drmp3*>(mHandle);  break;
      case Fmt::Ogg:  stb_vorbis_close(static_cast<stb_vorbis*>(mHandle)); break;
      default: break;
    }
  }
  mHandle = nullptr;
  mFmt = Fmt::None;
  mOk = false;
}

int StreamingDecoder::read(float* outL, float* outR, int frames)
{
  if (!mHandle || frames <= 0) return 0;
  const size_t need = static_cast<size_t>(frames) * mChannels;
  if (mInterleaved.size() < need) mInterleaved.resize(need);
  float* buf = mInterleaved.data();

  int got = 0;
  switch (mFmt)
  {
    case Fmt::Wav:
      got = static_cast<int>(drwav_read_pcm_frames_f32(static_cast<drwav*>(mHandle),
                                                       static_cast<drwav_uint64>(frames), buf));
      break;
    case Fmt::Flac:
      got = static_cast<int>(drflac_read_pcm_frames_f32(static_cast<drflac*>(mHandle),
                                                        static_cast<drflac_uint64>(frames), buf));
      break;
    case Fmt::Mp3:
      got = static_cast<int>(drmp3_read_pcm_frames_f32(static_cast<drmp3*>(mHandle),
                                                       static_cast<drmp3_uint64>(frames), buf));
      break;
    case Fmt::Ogg:
      got = stb_vorbis_get_samples_float_interleaved(static_cast<stb_vorbis*>(mHandle),
                                                     static_cast<int>(mChannels), buf,
                                                     frames * static_cast<int>(mChannels));
      break;
    default: return 0;
  }
  if (got <= 0) return 0;

  if (mChannels >= 2)
  {
    for (int i = 0; i < got; ++i)
    {
      outL[i] = buf[i * mChannels + 0];
      outR[i] = buf[i * mChannels + 1];
    }
  }
  else
  {
    for (int i = 0; i < got; ++i)
      outL[i] = outR[i] = buf[i];
  }
  return got;
}

void StreamingDecoder::seek(int64_t frame)
{
  if (!mHandle) return;
  if (frame < 0) frame = 0;
  if (mTotalFrames > 0 && frame > mTotalFrames) frame = mTotalFrames;
  switch (mFmt)
  {
    case Fmt::Wav:  drwav_seek_to_pcm_frame(static_cast<drwav*>(mHandle), static_cast<drwav_uint64>(frame)); break;
    case Fmt::Flac: drflac_seek_to_pcm_frame(static_cast<drflac*>(mHandle), static_cast<drflac_uint64>(frame)); break;
    case Fmt::Mp3:  drmp3_seek_to_pcm_frame(static_cast<drmp3*>(mHandle), static_cast<drmp3_uint64>(frame)); break;
    case Fmt::Ogg:  stb_vorbis_seek(static_cast<stb_vorbis*>(mHandle), static_cast<unsigned int>(frame)); break;
    default: break;
  }
}

} // namespace mc
