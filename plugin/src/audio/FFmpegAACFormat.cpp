// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#include "FFmpegAACFormat.h"

#if JUCE_LINUX

//==============================================================================
// Linux 向け AAC / M4A / MP4 デコード。
//
// 実装方針（重要）:
//   システムの `ffmpeg` 実行ファイルをサブプロセスで起動し、入力を WAV(float32 LE) の
//   一時ファイルへ丸ごとデコードする。その一時ファイルを JUCE の WavAudioFormat で読み、
//   読み取りは内部 WAV リーダーへ委譲する。
//
//   なぜ libav* を直接 dlopen/link しないのか:
//     - 以前は libav* を dlopen し、AVCodecContext 等の構造体フィールドへ直接アクセス
//       していた。しかし FFmpeg はメジャーバージョン間で構造体 ABI（フィールドのオフ
//       セットや sizeof）を保証しない。ビルド環境(例: Docker の FFmpeg6)と実行環境
//       (例: ホストの FFmpeg8)で AVCodecContext のレイアウトが食い違い、ch_layout 等
//       を別オフセットから読んでしまい "Input channel layout \"\"" 等で失敗していた。
//     - `ffmpeg` CLI の入出力仕様（pcm_f32le / WAV 出力）はメジャーをまたいで安定して
//       いるため、CLI 経由なら FFmpeg のバージョンに依存せず動作する。
//     - ライセンス方針（AGPL）にも合致: FFmpeg を同梱せず、システム提供のバイナリを
//       実行時に参照するだけ。dev ヘッダへのビルド時依存も不要になった。
//
// 利用形態の前提:
//   呼び出し側（PluginProcessor / InMemoryPlaybackSource::readAllToBuffer）は
//   reader.lengthInSamples と reader.read() で全サンプルを一括取得する。よってここでは
//   「先頭から全体をデコードして WavAudioFormat リーダーを返す」だけでよく、サンプル単位の
//   ランダムシークを自前実装する必要はない（シークが要っても WAV リーダーが面倒を見る）。
//==============================================================================

#include <memory>

namespace mc3 {
namespace {

//==============================================================================
// `ffmpeg` 実行ファイルを探す。PATH 上と一般的な絶対パスを順に確認する。
juce::File findFFmpegBinary()
{
    // 1) よくある絶対パス（PATH に頼らず確実に見つける）
    const char* candidates[] = {
        "/usr/bin/ffmpeg",
        "/usr/local/bin/ffmpeg",
        "/bin/ffmpeg",
        "/snap/bin/ffmpeg",
    };
    for (auto* c : candidates)
    {
        juce::File f (c);
        if (f.existsAsFile())
            return f;
    }

    // 2) PATH を走査
    const juce::String pathEnv = juce::SystemStats::getEnvironmentVariable ("PATH", {});
    for (const auto& dir : juce::StringArray::fromTokens (pathEnv, ":", ""))
    {
        if (dir.isEmpty())
            continue;
        juce::File f = juce::File (dir).getChildFile ("ffmpeg");
        if (f.existsAsFile())
            return f;
    }

    return {};
}

// 一度だけ ffmpeg を探してキャッシュする（スレッドセーフ）。
const juce::File& cachedFFmpegBinary()
{
    static const juce::File ff = findFFmpegBinary();
    return ff;
}

//==============================================================================
// 入力ファイルを ffmpeg で WAV(float32 LE) の一時ファイルへデコードする。
// 成功時、出力先の一時ファイルを返す（呼び出し側が使用後に削除する）。失敗時は無効 File。
//
// 重要: stdout(pipe:1) へ WAV を書かせてはいけない。ffmpeg はパイプ出力をシークできず、
// WAV の RIFF/data チャンクサイズに本当の値を書けないためプレースホルダ 0xFFFFFFFF を
// 書く。すると JUCE の WavAudioFormat が長さを「約 536,870,911 サンプル(=44.1k/2ch で
// 約 202 分)」と誤読し、再生位置も破綻する。一時「ファイル」へ書けば ffmpeg がシークして
// 正しいサイズを書き込むため、この問題が起きない。
juce::File decodeToWavFile (const juce::File& input)
{
    const juce::File& ff = cachedFFmpegBinary();
    if (! ff.existsAsFile() || ! input.existsAsFile())
        return {};

    const juce::File outFile = juce::File::createTempFile ("mixcompare_aac_decode.wav");

    juce::StringArray args;
    args.add (ff.getFullPathName());
    args.add ("-v");           args.add ("error");      // 余計なログを抑制
    args.add ("-nostdin");                               // 標準入力を待たない
    args.add ("-y");                                     // 出力（temp）を上書き
    args.add ("-i");           args.add (input.getFullPathName());
    args.add ("-map");         args.add ("0:a:0");       // 最初の音声ストリームのみ
    args.add ("-vn");                                    // 映像/アルバムアート無視
    args.add ("-c:a");         args.add ("pcm_f32le");   // float32 PCM
    args.add ("-f");           args.add ("wav");         // WAV コンテナ
    args.add (outFile.getFullPathName());                // 一時ファイルへ

    // wantStdErr も指定する: 指定しないと ffmpeg のログ出力先パイプが詰まり、
    // エラー時にプロセスがブロックしうる（出力自体は読まず破棄でよい）。
    juce::ChildProcess proc;
    if (! proc.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        outFile.deleteFile();
        return {};
    }

    // 終了まで待つ（最大 60 秒。通常は数秒）。
    if (! proc.waitForProcessToFinish (60000))
    {
        proc.kill();
        outFile.deleteFile();
        return {};
    }

    if (! outFile.existsAsFile() || outFile.getSize() <= 44) // WAV ヘッダより大きいこと
    {
        outFile.deleteFile();
        return {};
    }

    return outFile;
}

} // anonymous namespace

//==============================================================================
// reader は「デコード済み WAV 一時ファイルを JUCE の WavAudioFormat で読む」ラッパー。
// 入力ストリームから元ファイルパスを得られないため、createReaderFor 側で先に
// デコードを済ませ、出来上がった WAV リーダーをこのクラスが所有・委譲する。
// 一時ファイルは Reader の生存期間中保持し、破棄時に削除する。
//==============================================================================
class FFmpegAACFormat::Reader : public juce::AudioFormatReader
{
public:
    Reader (juce::AudioFormatReader* wavReaderToOwn, const juce::File& tempToOwn)
        : AudioFormatReader (nullptr, "FFmpeg AAC"),
          inner (wavReaderToOwn),
          tempFile (tempToOwn)
    {
        // 基底のメタ情報を内部 WAV リーダーから引き写す。
        sampleRate            = inner->sampleRate;
        bitsPerSample         = inner->bitsPerSample;
        lengthInSamples       = inner->lengthInSamples;
        numChannels           = inner->numChannels;
        usesFloatingPointData = inner->usesFloatingPointData;
        metadataValues        = inner->metadataValues;
    }

    ~Reader() override
    {
        inner.reset();             // WAV ファイルへのハンドルを先に閉じる
        tempFile.deleteFile();     // その後に一時ファイルを削除
    }

    bool readSamples (int* const* destSamples, int numDestChannels, int startOffsetInDestBuffer,
                      juce::int64 startSampleInFile, int numSamples) override
    {
        return inner->readSamples (destSamples, numDestChannels, startOffsetInDestBuffer,
                                   startSampleInFile, numSamples);
    }

private:
    std::unique_ptr<juce::AudioFormatReader> inner;
    juce::File tempFile;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Reader)
};

//==============================================================================
FFmpegAACFormat::FFmpegAACFormat()
    : AudioFormat ("FFmpeg AAC", ".m4a .aac .mp4 .m4b")
{
}

FFmpegAACFormat::~FFmpegAACFormat() = default;

juce::Array<int> FFmpegAACFormat::getPossibleSampleRates()
{
    return { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000 };
}

juce::Array<int> FFmpegAACFormat::getPossibleBitDepths()
{
    // 実際のデコード出力は常に 32bit float（ffmpeg の pcm_f32le）。
    return { 32 };
}

juce::AudioFormatReader* FFmpegAACFormat::createReaderFor (juce::InputStream* sourceStream,
                                                           bool deleteStreamIfOpeningFails)
{
    // この AudioFormat は「ファイル」を ffmpeg に渡す必要があるため、入力ストリームが
    // ファイルに紐づく FileInputStream であることを前提とする（本プラグインの
    // createReaderFor(File) 経由の利用ではこれが満たされる）。
    std::unique_ptr<juce::InputStream> streamOwner (sourceStream);

    if (! isFFmpegAvailable())
        return failReader (streamOwner, deleteStreamIfOpeningFails);

    auto* fileStream = dynamic_cast<juce::FileInputStream*> (sourceStream);
    if (fileStream == nullptr)
        return failReader (streamOwner, deleteStreamIfOpeningFails);

    const juce::File inputFile = fileStream->getFile();

    const juce::File wavTemp = decodeToWavFile (inputFile);
    if (! wavTemp.existsAsFile())
        return failReader (streamOwner, deleteStreamIfOpeningFails);

    // デコード済み WAV 一時ファイルを JUCE の WavAudioFormat で開く。
    auto wavInput = std::make_unique<juce::FileInputStream> (wavTemp);
    if (! wavInput->openedOk())
    {
        wavTemp.deleteFile();
        return failReader (streamOwner, deleteStreamIfOpeningFails);
    }

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> wavReader (
        wav.createReaderFor (wavInput.get(), true));
    if (wavReader == nullptr)
    {
        wavTemp.deleteFile();
        return failReader (streamOwner, deleteStreamIfOpeningFails);
    }

    wavInput.release(); // 所有権は wavReader へ渡った

    // 元の入力ストリームはもう不要（ffmpeg は File から直接読んだ）。破棄してよい。
    streamOwner.reset();

    return new Reader (wavReader.release(), wavTemp);
}

juce::AudioFormatReader* FFmpegAACFormat::failReader (std::unique_ptr<juce::InputStream>& streamOwner,
                                                      bool deleteStreamIfOpeningFails)
{
    // deleteStreamIfOpeningFails==false のとき、呼び出し元へ所有権を戻す（破棄しない）。
    if (! deleteStreamIfOpeningFails)
        streamOwner.release();
    return nullptr;
}

std::unique_ptr<juce::AudioFormatWriter> FFmpegAACFormat::createWriterFor (
    std::unique_ptr<juce::OutputStream>&, const juce::AudioFormatWriterOptions&)
{
    return nullptr; // 書き込みは非対応
}

bool FFmpegAACFormat::isFFmpegAvailable()
{
    return cachedFFmpegBinary().existsAsFile();
}

} // namespace mc3

#endif // JUCE_LINUX
