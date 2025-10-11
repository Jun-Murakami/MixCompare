#include "MomentaryProcessor.h"
#include <cmath>

namespace MixCompare
{

MomentaryProcessor::MomentaryProcessor()
{
    reset();
}

void MomentaryProcessor::prepareToPlay(double sampleRate, int maximumBlockSize)
{
    currentSampleRate = sampleRate;
    samplesPerBlock = maximumBlockSize;

    // K-weighting係数をサンプルレートに応じて更新
    coefficients.updateForSampleRate(sampleRate);

    // 400msウィンドウのサンプル数を計算
    const int windowSamples = static_cast<int>(sampleRate * WINDOW_SIZE_MS / 1000.0);

    // Debug logs removed for production cleanliness

    // バッファサイズを設定
    for (int i = 0; i < 2; ++i)
    {
        meanSquareBuffers[static_cast<std::size_t>(i)].setSizeInSamples(windowSamples);
    }

    reset();
}

void MomentaryProcessor::reset()
{
    // フィルタ状態をリセット
    for (auto& state : filterStates)
    {
        state.reset();
    }

    // バッファをリセット
    for (auto& buffer : meanSquareBuffers)
    {
        buffer.reset();
    }

    // 出力値をリセット
    momentaryLKFS = MIN_LKFS;
    peakHoldLKFS = MIN_LKFS;
}

void MomentaryProcessor::processBlock(const juce::AudioBuffer<float>& buffer)
{
    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    if (numChannels == 0 || numSamples == 0)
        return;

    // 各チャンネルを処理
    for (int channel = 0; channel < std::min(numChannels, 2); ++channel)
    {
        const float* channelData = buffer.getReadPointer(channel);
        auto& filterState = filterStates[static_cast<std::size_t>(channel)];
        auto& meanSquareBuffer = meanSquareBuffers[static_cast<std::size_t>(channel)];

    // Debug logs removed for production cleanliness

        for (int sample = 0; sample < numSamples; ++sample)
        {
            // K-weightingフィルタを適用
            float weighted = processKWeighting(channelData[sample], filterState);

            // Mean squareバッファに追加
            meanSquareBuffer.pushSample(weighted);
        }
    }

    // LKFS値を計算
    float meanSquareLeft = meanSquareBuffers[0].getMeanSquare();
    float meanSquareRight = numChannels >= 2 ? meanSquareBuffers[1].getMeanSquare() : meanSquareLeft;

    // Debug logs removed for production cleanliness

    float currentLKFS = calculateLKFS(meanSquareLeft, meanSquareRight);

    // アトミック変数を更新
    momentaryLKFS = currentLKFS;

    // ピークホールド更新
    float currentPeak = peakHoldLKFS.load();
    while (currentLKFS > currentPeak)
    {
        if (peakHoldLKFS.compare_exchange_weak(currentPeak, currentLKFS))
        {
            break;
        }
    }
}

float MomentaryProcessor::getMomentaryLKFS() const noexcept
{
    return momentaryLKFS.load();
}

float MomentaryProcessor::getPeakHoldLKFS() const noexcept
{
    return peakHoldLKFS.load();
}

void MomentaryProcessor::resetPeakHold()
{
    peakHoldLKFS = momentaryLKFS.load();
}

float MomentaryProcessor::processKWeighting(float input, FilterState& state)
{
    // Stage 1: Pre-filter (高域シェルフ)
    double x0_pre = input;
    double y0_pre = coefficients.b0_pre * x0_pre 
                  + coefficients.b1_pre * state.x1_pre 
                  + coefficients.b2_pre * state.x2_pre
                  - coefficients.a1_pre * state.y1_pre 
                  - coefficients.a2_pre * state.y2_pre;

    state.x2_pre = state.x1_pre;
    state.x1_pre = x0_pre;
    state.y2_pre = state.y1_pre;
    state.y1_pre = y0_pre;

    // Stage 2: RLB-weighting (高域通過)
    double x0_rlb = y0_pre;
    double y0_rlb = coefficients.b0_rlb * x0_rlb 
                  + coefficients.b1_rlb * state.x1_rlb 
                  + coefficients.b2_rlb * state.x2_rlb
                  - coefficients.a1_rlb * state.y1_rlb 
                  - coefficients.a2_rlb * state.y2_rlb;

    state.x2_rlb = state.x1_rlb;
    state.x1_rlb = x0_rlb;
    state.y2_rlb = state.y1_rlb;
    state.y1_rlb = y0_rlb;

    return static_cast<float>(y0_rlb);
}

float MomentaryProcessor::calculateLKFS(float meanSquareLeft, float meanSquareRight) const
{
    // Gated loudnessを計算（ITU-R BS.1770-4）
    // LK = -0.691 + 10 * log10(sum(Gi * zi))
    // Gi = チャンネルの重み（ステレオは1.0）
    // zi = mean square value

    if (meanSquareLeft <= 0.0f && meanSquareRight <= 0.0f)
        return MIN_LKFS;

    // チャンネル加重平均
    float weightedSum = CHANNEL_WEIGHT * meanSquareLeft + CHANNEL_WEIGHT * meanSquareRight;

    if (weightedSum <= 0.0f)
        return MIN_LKFS;

    // LKFS計算
    float lkfs = -0.691f + 10.0f * std::log10(weightedSum);

    // 範囲制限
    return std::max(lkfs, MIN_LKFS);
}

// K-weighting係数の更新（サンプルレート依存）
void MomentaryProcessor::KWeightingCoefficients::updateForSampleRate(double sampleRate)
{
    // ITU-R BS.1770-4 の係数は48kHz基準
    // 他のサンプルレートでは双一次変換で再計算が必要
    
    if (std::abs(sampleRate - 48000.0) < 1.0)
    {
        // 48kHzの場合はデフォルト値を使用
        return;
    }

    // Note: 実際の実装では、異なるサンプルレートに対応するために
    // 双一次変換を使用して係数を再計算する必要があります
    // ここでは簡略化のため、48kHz係数をそのまま使用
}

// FilterState実装
void MomentaryProcessor::FilterState::reset()
{
    x1_pre = x2_pre = y1_pre = y2_pre = 0.0;
    x1_rlb = x2_rlb = y1_rlb = y2_rlb = 0.0;
}

// MeanSquareBuffer実装
void MomentaryProcessor::MeanSquareBuffer::setSizeInSamples(int numSamples)
{
    const std::size_t size = numSamples > 0 ? static_cast<std::size_t>(numSamples) : 0;
    bufferSize = size;
    buffer.resize(bufferSize, 0.0f);
    reset();
}

void MomentaryProcessor::MeanSquareBuffer::reset()
{
    std::fill(buffer.begin(), buffer.end(), 0.0f);
    writeIndex = 0;
    sum = 0.0f;
    isFull = false;
}

void MomentaryProcessor::MeanSquareBuffer::pushSample(float weightedSample)
{
    if (bufferSize == 0)
        return;

    // 二乗値を計算
    float squared = weightedSample * weightedSample;

    // 古い値を引いて新しい値を加える
    if (isFull)
    {
        sum -= buffer[writeIndex];
    }
    
    buffer[writeIndex] = squared;
    sum += squared;

    // インデックスを更新
    writeIndex = (writeIndex + 1) % bufferSize;
    
    if (!isFull && writeIndex == 0)
    {
        isFull = true;
    }
}

float MomentaryProcessor::MeanSquareBuffer::getMeanSquare() const
{
    if (!isFull && writeIndex == 0)
        return 0.0f;

    const std::size_t count = isFull ? bufferSize : writeIndex;
    if (count == 0)
        return 0.0f;

    return sum / static_cast<float>(count);
}

} // namespace MixCompare
