// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
//
// OS ネイティブファイルダイアログ (WebView UI には組み込みダイアログが無いため自前実装)。
// UI スレッド (OnMessage) から呼ぶこと。返り値の path は UTF-8。
#pragma once

#include <string>
#include <vector>

namespace mc
{

/// 複数のオーディオファイルを選択させる。キャンセル時は空。
std::vector<std::string> PromptForAudioFiles(void* nativeParent);

/// プレイリスト (.m3u8) の保存先を選ばせる。成功で outPath に UTF-8 パス。
bool PromptForSavePlaylist(void* nativeParent, std::string& outPathUtf8);

/// プレイリスト (.m3u8) を開く。成功で outPath に UTF-8 パス。
bool PromptForOpenPlaylist(void* nativeParent, std::string& outPathUtf8);

} // namespace mc
