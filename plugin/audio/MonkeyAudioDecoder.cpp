// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "MonkeyAudioDecoder.h"

#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "All.h"
#include "MACLib.h"
#include "IAPEIO.h"

namespace mc
{
namespace
{
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
    MultiByteToWideChar(CP_UTF8, 0, pathUtf8.c_str(), static_cast<int>(pathUtf8.size()), wpath.data(), wlen);
    fp = _wfopen(wpath.c_str(), L"rb");
  }
#else
  fp = std::fopen(pathUtf8.c_str(), "rb");
#endif
  if (!fp) return bytes;
  std::fseek(fp, 0, SEEK_END); const long sz = std::ftell(fp); std::fseek(fp, 0, SEEK_SET);
  if (sz > 0) { bytes.resize(static_cast<size_t>(sz)); if (std::fread(bytes.data(), 1, bytes.size(), fp) != bytes.size()) bytes.clear(); }
  std::fclose(fp);
  return bytes;
}

// メモリ buffer 上で動く IAPEIO 実装 (Unicode パスは呼び出し側で解決済み)。
class MemoryAPEIO : public APE::IAPEIO
{
public:
  explicit MemoryAPEIO(const std::vector<uint8_t>& data) : mData(data) {}
  int Open(const APE::str_utfn*, bool) override { return ERROR_SUCCESS; }
  int Close() override { return ERROR_SUCCESS; }
  int Read(void* pBuffer, unsigned int nBytesToRead, unsigned int* pBytesRead) override
  {
    const size_t remain = (mPos < mData.size()) ? (mData.size() - mPos) : 0;
    const size_t n = (nBytesToRead < remain) ? nBytesToRead : remain;
    if (n > 0) std::memcpy(pBuffer, mData.data() + mPos, n);
    mPos += n;
    if (pBytesRead) *pBytesRead = static_cast<unsigned int>(n);
    return ERROR_SUCCESS;
  }
  int Write(const void*, unsigned int, unsigned int*) override { return ERROR_IO_WRITE; }
  int Seek(APE::int64 pos, APE::SeekMethod method) override
  {
    APE::int64 target = 0;
    switch (method)
    {
      case APE::SeekFileBegin:   target = pos; break;
      case APE::SeekFileCurrent: target = static_cast<APE::int64>(mPos) + pos; break;
      case APE::SeekFileEnd:     target = static_cast<APE::int64>(mData.size()) + pos; break;
      default: return ERROR_IO_READ;
    }
    if (target < 0) target = 0;
    if (target > static_cast<APE::int64>(mData.size())) target = static_cast<APE::int64>(mData.size());
    mPos = static_cast<size_t>(target);
    return ERROR_SUCCESS;
  }
  int Create(const APE::str_utfn*) override { return ERROR_IO_WRITE; }
  int Delete() override { return ERROR_IO_WRITE; }
  int SetEOF() override { return ERROR_IO_WRITE; }
  unsigned char* GetBuffer(int*) override { return nullptr; }
  APE::int64 GetPosition() override { return static_cast<APE::int64>(mPos); }
  APE::int64 GetSize() override { return static_cast<APE::int64>(mData.size()); }
private:
  const std::vector<uint8_t>& mData;
  size_t mPos = 0;
};
} // namespace

bool decodeMonkeyAudio(const std::string& pathUtf8, DecodedAudio& out)
{
  std::vector<uint8_t> bytes = readFileBytes(pathUtf8);
  if (bytes.empty()) { out.error = "ape file read failed"; return false; }

  MemoryAPEIO io(bytes);
  int err = 0;
  APE::IAPEDecompress* dec = CreateIAPEDecompressEx(&io, &err);
  if (!dec || err != ERROR_SUCCESS) { delete dec; out.error = "ape open failed"; return false; }

  const double sr   = static_cast<double>(dec->GetInfo(APE::IAPEDecompress::APE_INFO_SAMPLE_RATE));
  const int    bits = static_cast<int>(dec->GetInfo(APE::IAPEDecompress::APE_INFO_BITS_PER_SAMPLE));
  const int    ch   = static_cast<int>(dec->GetInfo(APE::IAPEDecompress::APE_INFO_CHANNELS));
  const APE::int64 total = dec->GetInfo(APE::IAPEDecompress::APE_DECOMPRESS_TOTAL_BLOCKS);
  const int blockAlign = static_cast<int>(dec->GetInfo(APE::IAPEDecompress::APE_INFO_BLOCK_ALIGN));

  if (sr <= 0 || ch <= 0 || total <= 0 || blockAlign <= 0) { delete dec; out.error = "ape bad info"; return false; }

  out.left.reserve(static_cast<size_t>(total));
  out.right.reserve(static_cast<size_t>(total));

  constexpr int kChunk = 4096;
  std::vector<unsigned char> buf(static_cast<size_t>(kChunk) * blockAlign);
  const float inv8  = 1.0f / 128.0f;
  const float inv16 = 1.0f / 32768.0f;
  const float inv24 = 1.0f / 8388608.0f;
  const double inv32 = 1.0 / 2147483648.0;

  for (;;)
  {
    APE::int64 retrieved = 0;
    const int r = dec->GetData(buf.data(), kChunk, &retrieved, nullptr);
    if (r != ERROR_SUCCESS && retrieved == 0) break;
    if (retrieved == 0) break;

    const int n = static_cast<int>(retrieved);
    for (int i = 0; i < n; ++i)
    {
      float l = 0.0f, rr = 0.0f;
      for (int c = 0; c < ch && c < 2; ++c)
      {
        float v = 0.0f;
        if (bits == 16) { const int16_t* s = reinterpret_cast<const int16_t*>(buf.data()); v = s[i * ch + c] * inv16; }
        else if (bits == 24) { const unsigned char* s = buf.data(); const int off = (i * ch + c) * 3;
          int val = (static_cast<int>(static_cast<int8_t>(s[off + 2])) << 16) | (static_cast<int>(s[off + 1]) << 8) | static_cast<int>(s[off]);
          v = val * inv24; }
        else if (bits == 32) { const int32_t* s = reinterpret_cast<const int32_t*>(buf.data()); v = static_cast<float>(s[i * ch + c] * inv32); }
        else if (bits == 8)  { const unsigned char* s = buf.data(); v = (static_cast<int>(s[i * ch + c]) - 128) * inv8; }
        if (c == 0) l = v;
        if (c == 1) rr = v;
      }
      if (ch < 2) rr = l; // mono → 複製
      out.left.push_back(l);
      out.right.push_back(rr);
    }
  }

  delete dec;

  out.sampleRate = sr;
  out.numFrames = static_cast<int64_t>(out.left.size());
  if (out.numFrames <= 0) { out.error = "ape decoded empty"; return false; }
  out.durationSec = out.numFrames / out.sampleRate;
  out.ok = true;
  return true;
}

} // namespace mc
