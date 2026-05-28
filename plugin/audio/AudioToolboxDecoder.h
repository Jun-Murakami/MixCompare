// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
//
// macOS AudioToolbox (ExtAudioFile) による AAC / M4A / MP4 のフルデコード (mac 専用)。
// Windows は MediaFoundationDecoder を使う。
#pragma once

#include <string>
#include "AudioDecoder.h"

namespace mc
{
#ifdef __APPLE__
/// .m4a/.aac/.mp4 をメモリへ全デコードする (mac)。成功で out.ok=true。
bool decodeAudioToolbox(const std::string& pathUtf8, DecodedAudio& out);
#endif
} // namespace mc
