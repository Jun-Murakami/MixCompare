// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#pragma once

#include <JuceHeader.h>

#if JUCE_LINUX

namespace mc3 {

/**
 * Linux 向け AAC / M4A / MP4 デコーダー。
 *
 * 方針:
 * - システムの `ffmpeg` 実行ファイルをサブプロセスで起動し、入力を WAV(float32) へ
 *   デコードして受け取る（実装詳細は FFmpegAACFormat.cpp 冒頭のコメント参照）。
 *   libav* を dlopen / link しないため、FFmpeg のメジャーバージョン差による構造体 ABI
 *   非互換の影響を受けない。ビルド時の dev ヘッダ依存も無い。
 * - `ffmpeg` が PATH 等に見つからない環境ではフォーマット登録をスキップし、AAC のみが
 *   無効になる（Windows の MediaFoundationAACFormat と同じ「利用可能なら登録」運用）。
 *
 * ライセンス: 本プロジェクトは AGPL-3.0-or-later。FFmpeg（LGPL/GPL）は同梱せず、ディストリ
 * 提供の実行ファイルを参照するだけなので、配布物にコーデック本体を含めない。
 */
class FFmpegAACFormat : public juce::AudioFormat
{
public:
    FFmpegAACFormat();
    ~FFmpegAACFormat() override;

    juce::Array<int> getPossibleSampleRates() override;
    juce::Array<int> getPossibleBitDepths() override;
    bool canDoStereo() override { return true; }
    bool canDoMono() override { return true; }
    bool isCompressed() override { return true; }
    bool isChannelLayoutSupported (const juce::AudioChannelSet&) override { return true; }

    juce::AudioFormatReader* createReaderFor (juce::InputStream* sourceStream,
                                              bool deleteStreamIfOpeningFails) override;

    std::unique_ptr<juce::AudioFormatWriter> createWriterFor (
                                              std::unique_ptr<juce::OutputStream>& streamToWriteTo,
                                              const juce::AudioFormatWriterOptions& options) override;

    juce::StringArray getQualityOptions() override { return {}; }

    /** 実行時に `ffmpeg` 実行ファイルが利用可能か。フォーマット登録可否の判定に用いる。 */
    static bool isFFmpegAvailable();

private:
    class Reader;

    // 開封失敗時の後始末（deleteStreamIfOpeningFails に応じてストリームの所有権を扱う）。
    static juce::AudioFormatReader* failReader (std::unique_ptr<juce::InputStream>& streamOwner,
                                                bool deleteStreamIfOpeningFails);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FFmpegAACFormat)
};

} // namespace mc3

#endif // JUCE_LINUX
