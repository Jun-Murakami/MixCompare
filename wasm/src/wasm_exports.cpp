#include "dsp_engine.h"
#include <cstdlib>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define WASM_EXPORT
#endif

static mc_wasm::DspEngine* g_engine = nullptr;

extern "C"
{

// ====== 初期化 / 解放 ======

WASM_EXPORT void dsp_init(double sampleRate, int blockSize)
{
    if (g_engine) delete g_engine;
    g_engine = new mc_wasm::DspEngine();
    g_engine->prepare(sampleRate, blockSize);
}

WASM_EXPORT void dsp_destroy()
{
    delete g_engine;
    g_engine = nullptr;
}

// ====== メモリ管理（JS → WASM PCM 転送用） ======

WASM_EXPORT float* dsp_alloc_buffer(int numSamples)
{
    return static_cast<float*>(std::malloc(sizeof(float) * numSamples));
}

WASM_EXPORT void dsp_free_buffer(float* ptr)
{
    std::free(ptr);
}

// ====== Host トラック ======

WASM_EXPORT void dsp_load_host_track(const float* left, const float* right,
                                      int numSamples, double trackSampleRate)
{
    if (g_engine) g_engine->loadHostTrack(left, right, numSamples, trackSampleRate);
}

WASM_EXPORT void dsp_set_source_select(int source)
{
    if (g_engine) g_engine->setSourceSelect(source);
}

// ====== Playlist トラック管理 ======

WASM_EXPORT void dsp_load_track(int index, const float* left, const float* right,
                                 int numSamples, double trackSampleRate)
{
    if (g_engine) g_engine->loadTrack(index, left, right, numSamples, trackSampleRate);
}

WASM_EXPORT void dsp_remove_track(int index)
{
    if (g_engine) g_engine->removeTrack(index);
}

WASM_EXPORT void dsp_clear_tracks()
{
    if (g_engine) g_engine->clearTracks();
}

WASM_EXPORT void dsp_select_track(int index)
{
    if (g_engine) g_engine->selectTrack(index);
}

WASM_EXPORT int dsp_get_track_count()
{
    return g_engine ? g_engine->getTrackCount() : 0;
}

WASM_EXPORT double dsp_get_track_duration(int index)
{
    return g_engine ? g_engine->getTrackDuration(index) : 0.0;
}

// ====== トランスポート ======

WASM_EXPORT void dsp_set_playing(int playing)
{
    if (g_engine) g_engine->setPlaying(playing != 0);
}

WASM_EXPORT int dsp_is_playing()
{
    return (g_engine && g_engine->isPlaying()) ? 1 : 0;
}

WASM_EXPORT void dsp_seek(double positionSec)
{
    if (g_engine) g_engine->seek(positionSec);
}

WASM_EXPORT void dsp_set_loop(int enabled, double startSec, double endSec)
{
    if (g_engine) g_engine->setLoop(enabled != 0, startSec, endSec);
}

WASM_EXPORT double dsp_get_position()
{
    return g_engine ? g_engine->getPositionSec() : 0.0;
}

WASM_EXPORT double dsp_get_duration()
{
    return g_engine ? g_engine->getDurationSec() : 0.0;
}

WASM_EXPORT int dsp_get_current_track_index()
{
    return g_engine ? g_engine->getCurrentTrackIndex() : -1;
}

WASM_EXPORT int dsp_consume_stopped_at_end()
{
    return (g_engine && g_engine->consumeStoppedAtEnd()) ? 1 : 0;
}

// ====== DSP パラメータ ======

WASM_EXPORT void dsp_set_host_gain(float db)
{
    if (g_engine) g_engine->setHostGainDb(db);
}

WASM_EXPORT void dsp_set_playlist_gain(float db)
{
    if (g_engine) g_engine->setPlaylistGainDb(db);
}

WASM_EXPORT void dsp_set_lpf_enabled(int enabled)
{
    if (g_engine) g_engine->setLpfEnabled(enabled != 0);
}

WASM_EXPORT void dsp_set_lpf_frequency(float freqHz)
{
    if (g_engine) g_engine->setLpfFrequency(freqHz);
}

// ====== メイン処理 ======

WASM_EXPORT void dsp_process_block(float* outputL, float* outputR, int numSamples)
{
    if (g_engine) g_engine->processBlock(outputL, outputR, numSamples);
}

// ====== メータリング（プラグイン互換: dB 変換済み） ======

WASM_EXPORT void dsp_set_metering_mode(int mode)
{
    if (g_engine) g_engine->setMeteringMode(mode);
}

static float gainToDb(float linear, float minDb = -60.0f)
{
    if (linear <= 0.0f) return minDb;
    float db = 20.0f * std::log10(linear);
    return db < minDb ? minDb : db;
}

/// メーター値を一括で float 配列に書き出す（worklet から1回のコールで取得）。
/// プラグインの PluginEditor.cpp と同じフォーマット（dB 変換済み）。
/// 配列レイアウト (24 floats):
///   [0]  meteringMode
///   [1]  host left     [2] host right      [3] host truePeakL  [4] host truePeakR
///   [5]  host rmsL     [6] host rmsR       [7] host momentary  [8] host momentaryHold
///   [9]  playlist left [10] playlist right  [11] pl truePeakL   [12] pl truePeakR
///   [13] pl rmsL       [14] pl rmsR        [15] pl momentary   [16] pl momentaryHold
///   [17] output left   [18] output right   [19] out momentary  [20] out momentaryHold
///   [21..23] reserved
WASM_EXPORT void dsp_get_meter_data(float* out)
{
    if (!g_engine || !out) return;
    auto& m = g_engine->getMetering();
    const int mode = g_engine->getMeteringMode();
    const bool isPeak = (mode == 0);
    const bool isMomentary = (mode == 2);

    out[0] = static_cast<float>(mode);

    // Helper: source-specific meter fill
    auto fillSource = [&](mc_wasm::MeteringService::Source src, float* dst)
    {
        auto vals = m.getMeterValues(src);
        if (isPeak)
        {
            float tpL = m.getTruePeakLevelAndDecay(src, 0);
            float tpR = m.getTruePeakLevelAndDecay(src, 1);
            dst[0] = gainToDb(tpL);      // left (bar)
            dst[1] = gainToDb(tpR);      // right (bar)
            dst[2] = gainToDb(tpL);      // truePeakL (label)
            dst[3] = gainToDb(tpR);      // truePeakR (label)
        }
        else
        {
            dst[0] = gainToDb(vals.rmsLeft);
            dst[1] = gainToDb(vals.rmsRight);
            dst[2] = gainToDb(m.getTruePeakLevelAndDecay(src, 0));
            dst[3] = gainToDb(m.getTruePeakLevelAndDecay(src, 1));
        }
        dst[4] = gainToDb(vals.rmsLeft);
        dst[5] = gainToDb(vals.rmsRight);
        dst[6] = isMomentary ? vals.momentaryLKFS : -70.0f;
        dst[7] = isMomentary ? vals.momentaryHoldLKFS : -70.0f;
    };

    // Host（サンプル音源のメーター）
    fillSource(mc_wasm::MeteringService::Source::Host, out + 1);
    // Playlist（ユーザーアップロードのメーター）
    fillSource(mc_wasm::MeteringService::Source::Playlist, out + 9);

    // Output
    auto outVals = m.getMeterValues(mc_wasm::MeteringService::Source::Output);
    out[17] = gainToDb(outVals.rmsLeft);
    out[18] = gainToDb(outVals.rmsRight);
    out[19] = isMomentary ? outVals.momentaryLKFS : -70.0f;
    out[20] = isMomentary ? outVals.momentaryHoldLKFS : -70.0f;
}

WASM_EXPORT void dsp_reset_true_peak()       { if (g_engine) g_engine->getMetering().resetTruePeakMeters(); }
WASM_EXPORT void dsp_reset_momentary_hold()  { if (g_engine) g_engine->getMetering().resetMomentaryHold(); }

} // extern "C"
