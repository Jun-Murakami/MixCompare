// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
//
// Monkey's Audio (.ape) のフルデコード。vendored MACLib を MemoryAPEIO 経由で使う
// (JUCE 版 MonkeyAudioFormat の MACLib 部分を JUCE 非依存に移植)。
#pragma once

#include <string>
#include "AudioDecoder.h"

namespace mc
{
/// .ape をメモリへ全デコードする。成功で out.ok=true。
bool decodeMonkeyAudio(const std::string& pathUtf8, DecodedAudio& out);
} // namespace mc
