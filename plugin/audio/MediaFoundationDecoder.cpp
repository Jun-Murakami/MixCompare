// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "MediaFoundationDecoder.h"

#ifdef _WIN32

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <shlwapi.h>
#include <vector>

namespace mc
{
namespace
{
template <class T> void safeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

std::vector<uint8_t> readFileBytesW(const std::string& pathUtf8)
{
  std::vector<uint8_t> bytes;
  const int wlen = MultiByteToWideChar(CP_UTF8, 0, pathUtf8.c_str(),
                                       static_cast<int>(pathUtf8.size()), nullptr, 0);
  if (wlen <= 0) return bytes;
  std::wstring wpath(static_cast<size_t>(wlen), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, pathUtf8.c_str(), static_cast<int>(pathUtf8.size()), wpath.data(), wlen);
  FILE* fp = _wfopen(wpath.c_str(), L"rb");
  if (!fp) return bytes;
  fseek(fp, 0, SEEK_END); const long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
  if (sz > 0) { bytes.resize(static_cast<size_t>(sz)); if (fread(bytes.data(), 1, bytes.size(), fp) != bytes.size()) bytes.clear(); }
  fclose(fp);
  return bytes;
}
} // namespace

bool decodeMediaFoundation(const std::string& pathUtf8, DecodedAudio& out)
{
  std::vector<uint8_t> bytes = readFileBytesW(pathUtf8);
  if (bytes.empty()) { out.error = "file read failed"; return false; }

  const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool coInited = SUCCEEDED(coHr);
  const HRESULT mfHr = MFStartup(MF_VERSION);
  const bool mfInited = SUCCEEDED(mfHr);
  bool ok = false;

  IStream* memStream = nullptr;
  IMFByteStream* byteStream = nullptr;
  IMFAttributes* attribs = nullptr;
  IMFSourceReader* reader = nullptr;
  IMFMediaType* outType = nullptr;
  IMFMediaType* actualType = nullptr;

  do
  {
    if (!mfInited) { out.error = "MFStartup failed"; break; }

    memStream = SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size()));
    if (!memStream) { out.error = "SHCreateMemStream failed"; break; }
    if (FAILED(MFCreateMFByteStreamOnStream(memStream, &byteStream))) { out.error = "byte stream"; break; }
    if (FAILED(MFCreateAttributes(&attribs, 1))) { out.error = "attribs"; break; }
    attribs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    if (FAILED(MFCreateSourceReaderFromByteStream(byteStream, attribs, &reader))) { out.error = "source reader"; break; }

    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);

    if (FAILED(MFCreateMediaType(&outType))) { out.error = "media type"; break; }
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    if (FAILED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, outType)))
    { out.error = "set float type"; break; }

    if (FAILED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &actualType)))
    { out.error = "get type"; break; }

    UINT32 sr = 0, ch = 0;
    actualType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
    actualType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
    if (sr == 0 || ch == 0) { out.error = "bad format"; break; }

    out.left.clear();
    out.right.clear();

    for (;;)
    {
      DWORD flags = 0;
      LONGLONG ts = 0;
      IMFSample* sample = nullptr;
      const HRESULT rhr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
                                             0, nullptr, &flags, &ts, &sample);
      if (FAILED(rhr)) { safeRelease(sample); break; }
      if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { safeRelease(sample); break; }
      if (!sample) continue;

      IMFMediaBuffer* buf = nullptr;
      if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && buf)
      {
        BYTE* data = nullptr; DWORD len = 0;
        if (SUCCEEDED(buf->Lock(&data, nullptr, &len)) && data)
        {
          const int frames = static_cast<int>(len / (sizeof(float) * ch));
          const float* f = reinterpret_cast<const float*>(data);
          for (int i = 0; i < frames; ++i)
          {
            out.left.push_back(f[i * ch + 0]);
            out.right.push_back(ch >= 2 ? f[i * ch + 1] : f[i * ch + 0]);
          }
          buf->Unlock();
        }
        safeRelease(buf);
      }
      safeRelease(sample);
    }

    out.sampleRate = sr;
    out.numFrames = static_cast<int64_t>(out.left.size());
    if (out.numFrames <= 0) { out.error = "decoded empty"; break; }
    out.durationSec = out.numFrames / out.sampleRate;
    out.ok = true;
    ok = true;
  } while (false);

  safeRelease(actualType);
  safeRelease(outType);
  safeRelease(reader);
  safeRelease(attribs);
  safeRelease(byteStream);
  safeRelease(memStream);
  if (mfInited) MFShutdown();
  if (coInited) CoUninitialize();
  return ok;
}

} // namespace mc

#endif // _WIN32
