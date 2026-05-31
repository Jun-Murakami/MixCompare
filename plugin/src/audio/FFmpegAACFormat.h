// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#pragma once

#include <JuceHeader.h>

#if JUCE_LINUX

namespace mc3 {

/**
 * Linux 向け FFmpeg ベースの AAC / M4A / MP4 デコーダー。
 *
 * 方針:
 * - libavformat / libavcodec / libavutil / libswresample を「実行時に dlopen」で読み込む。
 *   ビルド時はヘッダ（*-dev）のみ必要で、リンクはしない。これにより FFmpeg 実行用
 *   共有ライブラリが無い環境でもプラグイン自体は問題なくロードでき、AAC のみが無効になる
 *   （Windows の MediaFoundationAACFormat と同じ「利用可能なら登録」運用）。
 * - dlopen するのはヘッダのメジャーバージョンに一致する versioned soname
 *   （例: libavcodec.so.NN）。ABI 不一致のライブラリを掴むことを避ける。
 *
 * ライセンス: 本プロジェクトは AGPL-3.0-or-later。FFmpeg（LGPL/GPL）はディストリ提供の
 * 共有ライブラリを参照するだけで同梱しないため、配布物にコーデック本体を含めない。
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

    /** 実行時に FFmpeg 共有ライブラリ群が利用可能か。フォーマット登録可否の判定に用いる。 */
    static bool isFFmpegAvailable();

private:
    class Reader;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FFmpegAACFormat)
};

} // namespace mc3

#endif // JUCE_LINUX
