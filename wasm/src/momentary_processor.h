#pragma once
#include <cmath>
#include <vector>
#include <array>
#include <cstddef>
#include <algorithm>

namespace mc_wasm
{

/// ITU-R BS.1770-4 準拠の Momentary Loudness プロセッサ
/// K-weighting フィルタ + 400ms スライディングウィンドウで LKFS を算出
class MomentaryProcessor
{
public:
    MomentaryProcessor() { reset(); }

    void prepareToPlay(double sampleRate, int /*maximumBlockSize*/)
    {
        currentSampleRate = sampleRate;
        coefficients.updateForSampleRate(sampleRate);

        const int windowSamples = static_cast<int>(sampleRate * WINDOW_SIZE_MS / 1000.0);
        for (int i = 0; i < 2; ++i)
            meanSquareBuffers[i].setSizeInSamples(windowSamples);

        reset();
    }

    void reset()
    {
        for (auto& state : filterStates) state.reset();
        for (auto& buf : meanSquareBuffers) buf.reset();
        momentaryLKFS = MIN_LKFS;
        peakHoldLKFS = MIN_LKFS;
    }

    /// ステレオバッファを処理（interleaved ではなく、チャンネルごとのポインタ）
    void processBlock(const float* const* channelData, int numChannels, int numSamples)
    {
        if (numChannels == 0 || numSamples == 0) return;

        for (int channel = 0; channel < std::min(numChannels, 2); ++channel)
        {
            const float* data = channelData[channel];
            auto& state = filterStates[channel];
            auto& msBuffer = meanSquareBuffers[channel];

            for (int i = 0; i < numSamples; ++i)
            {
                float weighted = processKWeighting(data[i], state);
                msBuffer.pushSample(weighted);
            }
        }

        float msLeft = meanSquareBuffers[0].getMeanSquare();
        float msRight = numChannels >= 2 ? meanSquareBuffers[1].getMeanSquare() : msLeft;
        float currentLKFS = calculateLKFS(msLeft, msRight);

        momentaryLKFS = currentLKFS;
        if (currentLKFS > peakHoldLKFS)
            peakHoldLKFS = currentLKFS;
    }

    float getMomentaryLKFS() const { return momentaryLKFS; }
    float getPeakHoldLKFS() const { return peakHoldLKFS; }
    void resetPeakHold() { peakHoldLKFS = momentaryLKFS; }

private:
    static constexpr float WINDOW_SIZE_MS = 400.0f;
    static constexpr float MIN_LKFS = -70.0f;
    static constexpr float CHANNEL_WEIGHT = 1.0f;

    struct KWeightingCoefficients
    {
        double b0_pre = 1.53512485958697, b1_pre = -2.69169618940638, b2_pre = 1.19839281085285;
        double a1_pre = -1.69065929318241, a2_pre = 0.73248077421585;
        double b0_rlb = 1.0, b1_rlb = -2.0, b2_rlb = 1.0;
        double a1_rlb = -1.99004745483398, a2_rlb = 0.99007225036621;

        void updateForSampleRate(double /*sampleRate*/)
        {
            // 48kHz 基準係数をそのまま使用（簡略化）
        }
    };

    struct FilterState
    {
        double x1_pre = 0, x2_pre = 0, y1_pre = 0, y2_pre = 0;
        double x1_rlb = 0, x2_rlb = 0, y1_rlb = 0, y2_rlb = 0;
        void reset() { x1_pre = x2_pre = y1_pre = y2_pre = 0; x1_rlb = x2_rlb = y1_rlb = y2_rlb = 0; }
    };

    float processKWeighting(float input, FilterState& s)
    {
        double x0 = input;
        double y0_pre = coefficients.b0_pre * x0
                      + coefficients.b1_pre * s.x1_pre
                      + coefficients.b2_pre * s.x2_pre
                      - coefficients.a1_pre * s.y1_pre
                      - coefficients.a2_pre * s.y2_pre;
        s.x2_pre = s.x1_pre; s.x1_pre = x0;
        s.y2_pre = s.y1_pre; s.y1_pre = y0_pre;

        double y0_rlb = coefficients.b0_rlb * y0_pre
                       + coefficients.b1_rlb * s.x1_rlb
                       + coefficients.b2_rlb * s.x2_rlb
                       - coefficients.a1_rlb * s.y1_rlb
                       - coefficients.a2_rlb * s.y2_rlb;
        s.x2_rlb = s.x1_rlb; s.x1_rlb = y0_pre;
        s.y2_rlb = s.y1_rlb; s.y1_rlb = y0_rlb;

        return static_cast<float>(y0_rlb);
    }

    float calculateLKFS(float msLeft, float msRight) const
    {
        if (msLeft <= 0.0f && msRight <= 0.0f) return MIN_LKFS;
        float sum = CHANNEL_WEIGHT * msLeft + CHANNEL_WEIGHT * msRight;
        if (sum <= 0.0f) return MIN_LKFS;
        return std::max(-0.691f + 10.0f * std::log10(sum), MIN_LKFS);
    }

    class MeanSquareBuffer
    {
    public:
        void setSizeInSamples(int n)
        {
            bufferSize = n > 0 ? static_cast<size_t>(n) : 0;
            buffer.resize(bufferSize, 0.0f);
            reset();
        }
        void reset()
        {
            std::fill(buffer.begin(), buffer.end(), 0.0f);
            writeIndex = 0; sum = 0.0f; isFull = false;
        }
        void pushSample(float weighted)
        {
            if (bufferSize == 0) return;
            float sq = weighted * weighted;
            if (isFull) sum -= buffer[writeIndex];
            buffer[writeIndex] = sq;
            sum += sq;
            writeIndex = (writeIndex + 1) % bufferSize;
            if (!isFull && writeIndex == 0) isFull = true;
        }
        float getMeanSquare() const
        {
            size_t count = isFull ? bufferSize : writeIndex;
            return count > 0 ? sum / static_cast<float>(count) : 0.0f;
        }
    private:
        std::vector<float> buffer;
        size_t writeIndex = 0, bufferSize = 0;
        float sum = 0.0f;
        bool isFull = false;
    };

    double currentSampleRate = 48000.0;
    KWeightingCoefficients coefficients;
    std::array<FilterState, 2> filterStates;
    std::array<MeanSquareBuffer, 2> meanSquareBuffers;
    float momentaryLKFS = -100.0f;
    float peakHoldLKFS = -100.0f;
};

} // namespace mc_wasm
