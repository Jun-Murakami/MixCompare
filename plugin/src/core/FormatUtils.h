// 共通ユーティリティ: 数値フォーマット
// フロントエンドの表示仕様に合わせて、周波数(Hz)の表示を整形する。
// 仕様:
// - 1000Hz 未満: 少数切り捨て (整数表示)
// - 1000Hz 以上: kHz 表示で小数点1桁 (例: 1.2k)

#pragma once

#include <juce_core/juce_core.h>

namespace mc3::format
{
    // フロントエンド準拠の量子化: 実数Hzを表示仕様に沿って丸める
    // - 1000Hz 未満: 小数切り捨て（整数Hz）
    // - 1000Hz 以上: 0.1kHz 単位に丸め（四捨五入） => 実際のHzへ戻す
    inline double quantizeFrequencyForProcessing(double hz) noexcept
    {
        if (!std::isfinite(hz) || hz < 0.0)
            return 0.0;

        if (hz >= 1000.0)
        {
            // 0.1kHz=100Hz 単位に丸め（四捨五入）
            const double khz = hz / 1000.0;
            const double roundedKhz = std::round(khz * 10.0) / 10.0; // 0.1kHz step
            return roundedKhz * 1000.0;
        }

        // 999Hz 以下は小数切り捨て（floor）
        return std::floor(hz);
    }

    // フロントエンド準拠の周波数フォーマットを返す
    // 例) 999.9 -> "999 Hz", 1000 -> "1.0 kHz", 12045 -> "12.0 kHz"
    inline juce::String frequencyHumanReadable(double hz) noexcept
    {
        // ガード: 非数/負値を考慮し0とみなす
        if (!std::isfinite(hz) || hz < 0.0)
            hz = 0.0;

        if (hz >= 1000.0)
        {
            // kHz 単位で小数1桁固定
            const double khz = hz / 1000.0;
            return juce::String(khz, 1) + " kHz";
        }

        // 999Hz 以下は小数切り捨て
        const int truncated = static_cast<int>(hz);
        return juce::String(truncated) + " Hz";
    }
}


