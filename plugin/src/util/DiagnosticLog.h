// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#pragma once

#include <JuceHeader.h>

namespace mc3
{

// 常時オンの軽量診断ログ。
// ユーザー環境で WebView UI が表示されない問題（真っ白/灰色）を現地調査するため、
//   - 起動時の環境情報（OS / ホスト / セッション種別 / 関連環境変数）
//   - Linux: WebKitGTK 系ライブラリの dlopen 可否と dlerror
//   - WebView のライフサイクル（最初のリソース要求 / JS からの最初のネイティブ呼び出し）
// をテキストファイルへ記録する。ログの場所:
//   Linux:   ~/.config/MixCompare/Logs/
//   Windows: %APPDATA%/MixCompare/Logs/
//   macOS:   ~/Library/Logs/MixCompare/
// 個人情報やオーディオデータは記録しない。サイズは FileLogger により自動で刈り込む。
class DiagnosticLog final
{
public:
    // 冪等。プロセス内で最初の呼び出しがログを開設し、環境スナップショットを記録する。
    // PluginProcessor のコンストラクタから呼ぶ（エディタ生成＝WebView 起動より前）。
    static void install();

    // タイムスタンプ付きで1行追記（スレッドセーフ）。install() 前の呼び出しは無視される。
    static void log(const juce::String& message);

    static juce::File getLogDirectory();
    static juce::File getLogFile();

    // WebView 子プロセス（--juce-gtkwebkitfork-child）の stdout/stderr の書き出し先。
    // juce-webview-linux-childlog.patch が環境変数 JUCE_WEBVIEW_CHILD_LOG を介して使用する。
    static juce::File getWebViewChildLogFile();

private:
    DiagnosticLog() = delete;
    ~DiagnosticLog() = delete;
};

} // namespace mc3
