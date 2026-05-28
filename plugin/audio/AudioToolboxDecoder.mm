// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#include "AudioToolboxDecoder.h"

#ifdef __APPLE__

#import <Foundation/Foundation.h>
#import <AudioToolbox/AudioToolbox.h>
#include <vector>

namespace mc
{

bool decodeAudioToolbox(const std::string& pathUtf8, DecodedAudio& out)
{
  @autoreleasepool
  {
    NSString* ns = [NSString stringWithUTF8String:pathUtf8.c_str()];
    if (!ns) { out.error = "bad path"; return false; }
    NSURL* url = [NSURL fileURLWithPath:ns];

    ExtAudioFileRef file = nullptr;
    if (ExtAudioFileOpenURL((__bridge CFURLRef)url, &file) != noErr || !file)
    { out.error = "ExtAudioFileOpenURL failed"; return false; }

    // ファイルのネイティブフォーマット (sampleRate / channels) を取得。
    AudioStreamBasicDescription fileFmt = {};
    UInt32 sz = sizeof(fileFmt);
    if (ExtAudioFileGetProperty(file, kExtAudioFileProperty_FileDataFormat, &sz, &fileFmt) != noErr)
    { ExtAudioFileDispose(file); out.error = "get file format failed"; return false; }

    const double sr = fileFmt.mSampleRate > 0 ? fileFmt.mSampleRate : 44100.0;
    UInt32 ch = fileFmt.mChannelsPerFrame > 0 ? fileFmt.mChannelsPerFrame : 2;
    if (ch > 2) ch = 2; // L/R のみ扱う

    // クライアント (出力) フォーマット = float32 interleaved。
    AudioStreamBasicDescription cli = {};
    cli.mSampleRate = sr;
    cli.mFormatID = kAudioFormatLinearPCM;
    cli.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    cli.mBitsPerChannel = 32;
    cli.mChannelsPerFrame = ch;
    cli.mFramesPerPacket = 1;
    cli.mBytesPerFrame = sizeof(float) * ch;
    cli.mBytesPerPacket = cli.mBytesPerFrame;

    if (ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat, sizeof(cli), &cli) != noErr)
    { ExtAudioFileDispose(file); out.error = "set client format failed"; return false; }

    SInt64 totalFrames = 0;
    sz = sizeof(totalFrames);
    ExtAudioFileGetProperty(file, kExtAudioFileProperty_FileLengthFrames, &sz, &totalFrames);
    if (totalFrames > 0)
    {
      out.left.reserve(static_cast<size_t>(totalFrames));
      out.right.reserve(static_cast<size_t>(totalFrames));
    }

    constexpr UInt32 kChunk = 8192;
    std::vector<float> buf(static_cast<size_t>(kChunk) * ch);
    for (;;)
    {
      AudioBufferList abl;
      abl.mNumberBuffers = 1;
      abl.mBuffers[0].mNumberChannels = ch;
      abl.mBuffers[0].mDataByteSize = static_cast<UInt32>(buf.size() * sizeof(float));
      abl.mBuffers[0].mData = buf.data();

      UInt32 frames = kChunk;
      if (ExtAudioFileRead(file, &frames, &abl) != noErr) break;
      if (frames == 0) break;

      for (UInt32 i = 0; i < frames; ++i)
      {
        out.left.push_back(buf[i * ch + 0]);
        out.right.push_back(ch >= 2 ? buf[i * ch + 1] : buf[i * ch + 0]);
      }
    }

    ExtAudioFileDispose(file);

    out.sampleRate = sr;
    out.numFrames = static_cast<int64_t>(out.left.size());
    if (out.numFrames <= 0) { out.error = "decoded empty"; return false; }
    out.durationSec = out.numFrames / out.sampleRate;
    out.ok = true;
    return true;
  }
}

} // namespace mc

#endif // __APPLE__
