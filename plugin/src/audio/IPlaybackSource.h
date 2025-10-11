#pragma once

#include <JuceHeader.h>

namespace MixCompare
{

/**
 * IPlaybackSource
 *
 * 再生ソースを抽象化するためのリアルタイム安全なインターフェース。
 * - 目的: 短尺=メモリ全展開、長尺=ストリーミング等、実装差を吸収して
 *   `AudioEngine` から一貫した API で扱えるようにする。
 * - リアルタイム原則: `process()` 内ではメモリ確保/ロック/ログ出力を禁止。
 *   例外も使用しない（noexcept）。
 */
class IPlaybackSource
{
public:
    virtual ~IPlaybackSource() = default;

    /**
     * 再生前の準備を行う。
     * - バッファの事前確保や内部状態の初期化を行う。
     * - 呼び出しスレッドの制約はない（通常は準備スレッド/メインスレッド）。
     */
    virtual void prepare(double sampleRate, int blockSize) = 0;

    /**
     * リソースを解放する。
     * - 準備で確保したリソースを解放する。
     */
    virtual void release() = 0;

    /**
     * オーディオブロックを生成する（リアルタイムスレッド）。
     * - outBuffer の先頭から numSamples サンプル分に書き込む。
     * - 副作用としてのメモリ確保/ロック/ログ出力は禁止。
     */
    virtual void process(juce::AudioBuffer<float>& outBuffer, int numSamples) noexcept = 0;

    /**
     * 指定位置（秒）へシークする。
     * - リアルタイム安全に扱えるよう、内部では原子フラグ等で次ブロックからの反映に留める実装を推奨。
     */
    virtual void seek(double positionSeconds) noexcept = 0;

    /** ループ範囲/有効無効を設定する。秒単位。 */
    virtual void setLoop(double startSec, double endSec, bool enabled) noexcept = 0;

    /** 総尺（秒）。不明な場合は 0.0 を返す。 */
    virtual double getDurationSec() const noexcept = 0;

    /** ソースのファイルサンプルレート。未知時は準備済みの処理 SR を返してもよい。 */
    virtual double getFileSampleRate() const noexcept = 0;

    /** 現在の再生位置（秒）。未知時は 0.0 を返す。 */
    virtual double getCurrentPositionSec() const noexcept = 0;

    /**
     * ソース切替に関するフェーズ開始通知。
     * - クロスフェード等で開始フラグを立てる用途。
     */
    virtual void beginSwap() noexcept = 0;

    /** ソース切替完了通知。後始末やフラグ解除に使用。 */
    virtual void endSwap() noexcept = 0;
};

} // namespace MixCompare



