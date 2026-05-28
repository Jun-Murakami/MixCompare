// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
//
// Windows Media Foundation による AAC / M4A / MP4 のフルデコード (Windows 専用)。
// JUCE 版 MediaFoundationAACFormat の MF 部分を JUCE 非依存に移植したもの。
#pragma once

#include <string>
#include "AudioDecoder.h"

namespace mc
{
#ifdef _WIN32
/// .m4a/.aac/.mp4 をメモリへ全デコードする。成功で out.ok=true。
bool decodeMediaFoundation(const std::string& pathUtf8, DecodedAudio& out);
#endif
} // namespace mc
