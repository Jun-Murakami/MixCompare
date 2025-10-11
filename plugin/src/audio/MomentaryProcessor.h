#pragma once

#include <JuceHeader.h>
#include <vector>
#include <cstddef>
#include <atomic>
#include <array>

namespace MixCompare
{

/**
 * ITU-R BS.1770-4 準拠のMomentary Loudness処理
 * K-weightingフィルタと400msスライディングウィンドウでLKFS値を算出
 */
class MomentaryProcessor
{
public:
    MomentaryProcessor();
    ~MomentaryProcessor() = default;

    /** 処理の初期化 */
    void prepareToPlay(double sampleRate, int maximumBlockSize);

    /** リセット */
    void reset();

    /** オーディオバッファを処理してMomentary値を計算 */
    void processBlock(const juce::AudioBuffer<float>& buffer);

    /** 現在のMomentary LKFS値を取得 */
    float getMomentaryLKFS() const noexcept;

    /** ホールドされたピーク値を取得 */
    float getPeakHoldLKFS() const noexcept;

    /** ホールド値をリセット */
    void resetPeakHold();

private:
    // K-weighting フィルタ係数 (ITU-R BS.1770-4)
    struct KWeightingCoefficients
    {
        // Stage 1: Pre-filter (high shelf)
        double b0_pre = 1.53512485958697;
        double b1_pre = -2.69169618940638;
        double b2_pre = 1.19839281085285;
        double a1_pre = -1.69065929318241;
        double a2_pre = 0.73248077421585;

        // Stage 2: RLB-weighting (high-pass)
        double b0_rlb = 1.0;
        double b1_rlb = -2.0;
        double b2_rlb = 1.0;
        double a1_rlb = -1.99004745483398;
        double a2_rlb = 0.99007225036621;

        void updateForSampleRate(double sampleRate);
    };

    // フィルタ状態（各チャンネル用）
    struct FilterState
    {
        // Pre-filter state
        double x1_pre = 0.0;
        double x2_pre = 0.0;
        double y1_pre = 0.0;
        double y2_pre = 0.0;

        // RLB-filter state
        double x1_rlb = 0.0;
        double x2_rlb = 0.0;
        double y1_rlb = 0.0;
        double y2_rlb = 0.0;

        void reset();
    };

    // K-weighting フィルタ処理
    float processKWeighting(float input, FilterState& state);

    // Mean square 計算用のリングバッファ
    class MeanSquareBuffer
    {
    public:
        void setSizeInSamples(int numSamples);
        void reset();
        void pushSample(float weightedSample);
        float getMeanSquare() const;

    private:
        std::vector<float> buffer;
        std::size_t writeIndex = 0;
        std::size_t bufferSize = 0;
        float sum = 0.0f;
        bool isFull = false;
    };

    // Member variables
    double currentSampleRate = 48000.0;
    int samplesPerBlock = 512;

    KWeightingCoefficients coefficients;
    std::array<FilterState, 2> filterStates; // 2チャンネル分

    // 400ms window用のバッファ（チャンネル毎）
    std::array<MeanSquareBuffer, 2> meanSquareBuffers;

    // 出力値
    std::atomic<float> momentaryLKFS{-100.0f};
    std::atomic<float> peakHoldLKFS{-100.0f};

    // 定数
    static constexpr float WINDOW_SIZE_MS = 400.0f; // ITU-R BS.1770-4規定
    static constexpr float MIN_LKFS = -70.0f; // 最小値（無音時）
    static constexpr float CHANNEL_WEIGHT = 1.0f; // ステレオチャンネルの重み

    // LKFS計算
    float calculateLKFS(float meanSquareLeft, float meanSquareRight) const;
};

} // namespace MixCompare
