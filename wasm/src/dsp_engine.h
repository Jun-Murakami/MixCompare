#pragma once
#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>
#include "smoothed_value.h"
#include "svt_filter.h"
#include "metering.h"

namespace mc_wasm
{

struct Track
{
    std::vector<float> left;
    std::vector<float> right;
    int numSamples = 0;
    double trackSampleRate = 44100.0;
    double durationSec = 0.0;
};

/// プラグイン AudioEngine の WASM 版（フルスタック）
/// Host(A) / Playlist(B) の2系統 + ソース切替 + DSP を C++ で一括管理。
class DspEngine
{
public:
    static constexpr int MAX_TRACKS = 64;

    DspEngine() = default;

    void prepare(double sr, int bs)
    {
        sampleRate = sr;
        blockSize = bs;

        lpfLeft.prepare(sr);
        lpfRight.prepare(sr);
        lpfLeft.setCutoffFrequency(20000.0f);
        lpfRight.setCutoffFrequency(20000.0f);

        hostGainSmoothed.reset(sr, 0.002);
        hostGainSmoothed.setCurrentAndTargetValue(1.0f);
        playlistGainSmoothed.reset(sr, 0.002);
        playlistGainSmoothed.setCurrentAndTargetValue(1.0f);
        sourceBlendSmoothed.reset(sr, 0.003);
        sourceBlendSmoothed.setCurrentAndTargetValue(1.0f); // default=Playlist（plugin UI と一致）
        lpfMixSmoothed.reset(sr, 0.003);
        lpfMixSmoothed.setCurrentAndTargetValue(0.0f);

        metering.prepareToPlay(sr, bs);
        lastLpfEnabled = false;
    }

    // ====== Host トラック（デモ用サンプル音源、独立トランスポート） ======
    //
    // 実プラグインでは HOST 入力は DAW 由来なので transport を持たない。
    // Web デモでは DAW が居ないので、HOST 用の独立した play/pause/seek/loop を
    // C++ 側に持たせて再現する。playlist 側 transport (playing/seek/setLoop) と完全独立。

    void loadHostTrack(const float* left, const float* right,
                       int numSamples, double trackSampleRate)
    {
        hostTrack.left.assign(left, left + numSamples);
        hostTrack.right.assign(right, right + numSamples);
        hostTrack.numSamples = numSamples;
        hostTrack.trackSampleRate = trackSampleRate;
        hostTrack.durationSec = trackSampleRate > 0 ? numSamples / trackSampleRate : 0.0;
        hostPlaybackPos = 0.0;
        hostStoppedAtEnd = false;
    }

    // ====== Host トランスポート（playlist の playing/seek/setLoop とは別系統） ======

    void hostSetPlaying(bool p)
    {
        if (p && hostTrack.numSamples == 0) return;
        if (p && hostStoppedAtEnd) { hostPlaybackPos = 0.0; hostStoppedAtEnd = false; }
        hostPlaying = p;
    }
    bool hostIsPlaying() const { return hostPlaying; }

    void hostSeek(double positionSec)
    {
        if (hostTrack.numSamples <= 0 || hostTrack.trackSampleRate <= 0) return;
        double pos = positionSec * hostTrack.trackSampleRate;
        pos = std::max(0.0, std::min(pos, static_cast<double>(hostTrack.numSamples)));
        hostPlaybackPos = pos;
        hostStoppedAtEnd = false;
    }

    void hostSetLoop(bool enabled) { hostLoopEnabled = enabled; }
    bool hostGetLoopEnabled() const { return hostLoopEnabled; }

    double hostGetPositionSec() const
    {
        if (hostTrack.trackSampleRate <= 0) return 0.0;
        return hostPlaybackPos / hostTrack.trackSampleRate;
    }
    double hostGetDurationSec() const { return hostTrack.durationSec; }

    bool consumeHostStoppedAtEnd()
    {
        bool v = hostStoppedAtEnd;
        hostStoppedAtEnd = false;
        return v;
    }

    // ====== Playlist トラック管理 ======

    void loadTrack(int index, const float* left, const float* right,
                   int numSamples, double trackSampleRate)
    {
        if (index < 0 || index >= MAX_TRACKS) return;
        if (index >= static_cast<int>(tracks.size()))
            tracks.resize(index + 1);
        auto& t = tracks[index];
        t.left.assign(left, left + numSamples);
        t.right.assign(right, right + numSamples);
        t.numSamples = numSamples;
        t.trackSampleRate = trackSampleRate;
        t.durationSec = trackSampleRate > 0 ? numSamples / trackSampleRate : 0.0;
    }

    void removeTrack(int index)
    {
        if (index < 0 || index >= static_cast<int>(tracks.size())) return;
        tracks.erase(tracks.begin() + index);
        if (currentTrackIndex >= static_cast<int>(tracks.size()))
            currentTrackIndex = static_cast<int>(tracks.size()) - 1;
    }

    void clearTracks()
    {
        tracks.clear();
        currentTrackIndex = -1;
        playbackPosSamples = 0.0;
        playing = false;
    }

    void selectTrack(int index)
    {
        if (index < 0 || index >= static_cast<int>(tracks.size())) return;
        currentTrackIndex = index;
        playbackPosSamples = 0.0;
        loopStartSec = 0.0;
        loopEndSec = tracks[index].durationSec;
    }

    int getTrackCount() const { return static_cast<int>(tracks.size()); }
    double getTrackDuration(int index) const
    {
        if (index < 0 || index >= static_cast<int>(tracks.size())) return 0.0;
        return tracks[index].durationSec;
    }

    // ====== ソース切替 ======

    void setSourceSelect(int source) { sourceSelect = source; } // 0=Host, 1=Playlist

    // ====== トランスポート ======

    void setPlaying(bool p) { playing = p; }
    bool isPlaying() const { return playing; }

    void seek(double positionSec)
    {
        const auto* t = currentTrack();
        if (!t) return;
        playbackPosSamples = positionSec * t->trackSampleRate;
        playbackPosSamples = std::max(0.0, std::min(playbackPosSamples, static_cast<double>(t->numSamples)));
        stoppedAtEnd = false;
    }

    void setLoop(bool enabled, double startSec, double endSec)
    {
        loopEnabled = enabled;
        loopStartSec = startSec;
        loopEndSec = endSec;
    }

    bool getLoopEnabled() const { return loopEnabled; }
    double getLoopStartSec() const { return loopStartSec; }
    double getLoopEndSec() const { return loopEndSec; }

    double getPositionSec() const
    {
        const auto* t = currentTrack();
        if (!t || t->trackSampleRate <= 0) return 0.0;
        return playbackPosSamples / t->trackSampleRate;
    }

    double getDurationSec() const
    {
        const auto* t = currentTrack();
        return t ? t->durationSec : 0.0;
    }

    int getCurrentTrackIndex() const { return currentTrackIndex; }

    bool consumeStoppedAtEnd()
    {
        bool v = stoppedAtEnd;
        stoppedAtEnd = false;
        return v;
    }

    // ====== DSP パラメータ ======

    void setHostGainDb(float db) { hostGainDb = db; }
    void setPlaylistGainDb(float db) { playlistGainDb = db; }
    void setLpfEnabled(bool e) { lpfEnabled = e; }
    void setLpfFrequency(float hz)
    {
        lpfFreqHz = hz;
        lpfLeft.setCutoffFrequency(hz);
        lpfRight.setCutoffFrequency(hz);
    }

    void setMeteringMode(int mode) { meteringMode = mode; } // 0=Peak, 1=RMS, 2=Momentary
    int getMeteringMode() const { return meteringMode; }

    MeteringService& getMetering() { return metering; }
    const MeteringService& getMetering() const { return metering; }

    // ====== メイン処理 ======

    void processBlock(float* outputL, float* outputR, int numSamples)
    {
        float hostL[512], hostR[512], plL[512], plR[512];
        const int n = std::min(numSamples, 512);

        // --- Host PCM（独立トランスポート + ループ） ---
        if (hostTrack.numSamples > 0 && hostPlaying)
        {
            for (int i = 0; i < n; ++i)
            {
                if (hostPlaybackPos >= hostTrack.numSamples)
                {
                    if (hostLoopEnabled)
                    {
                        hostPlaybackPos = std::fmod(hostPlaybackPos, static_cast<double>(hostTrack.numSamples));
                    }
                    else
                    {
                        hostPlaying = false;
                        hostStoppedAtEnd = true;
                        for (int j = i; j < n; ++j) { hostL[j] = 0.0f; hostR[j] = 0.0f; }
                        // i 以降を 0 で埋めたので外側の処理に進む
                        goto host_done;
                    }
                }
                int pos = static_cast<int>(hostPlaybackPos);
                if (pos < 0) pos = 0;
                hostL[i] = hostTrack.left[pos];
                hostR[i] = hostTrack.right[pos];
                hostPlaybackPos += 1.0;
            }
        }
        else
        {
            std::memset(hostL, 0, sizeof(float) * n);
            std::memset(hostR, 0, sizeof(float) * n);
        }
    host_done:;

        // --- Playlist PCM ---
        const auto* t = currentTrack();
        if (t && playing)
        {
            const int trackLen = t->numSamples;
            const double loopStartSmp = loopStartSec * t->trackSampleRate;
            const double loopEndSmp = loopEndSec * t->trackSampleRate;

            for (int i = 0; i < n; ++i)
            {
                int pos = static_cast<int>(playbackPosSamples);
                if (pos >= 0 && pos < trackLen)
                {
                    plL[i] = t->left[pos];
                    plR[i] = t->right[pos];
                }
                else
                {
                    plL[i] = 0.0f;
                    plR[i] = 0.0f;
                }
                playbackPosSamples += 1.0;

                if (loopEnabled && loopEndSmp > loopStartSmp)
                {
                    if (playbackPosSamples >= loopEndSmp)
                    {
                        const double loopLen = loopEndSmp - loopStartSmp;
                        playbackPosSamples = loopStartSmp
                            + std::fmod(playbackPosSamples - loopStartSmp, loopLen);
                    }
                }
                else if (playbackPosSamples >= trackLen)
                {
                    playing = false;
                    stoppedAtEnd = true;
                    for (int j = i + 1; j < n; ++j) { plL[j] = 0.0f; plR[j] = 0.0f; }
                    break;
                }
            }
        }
        else
        {
            std::memset(plL, 0, sizeof(float) * n);
            std::memset(plR, 0, sizeof(float) * n);
        }

        // --- ゲイン適用 ---
        const float hostGainLin = std::pow(10.0f, hostGainDb / 20.0f);
        const float plGainLin = std::pow(10.0f, playlistGainDb / 20.0f);
        hostGainSmoothed.setTargetValue(hostGainLin);
        playlistGainSmoothed.setTargetValue(plGainLin);

        for (int i = 0; i < n; ++i)
        {
            const float hg = hostGainSmoothed.getNextValue();
            const float pg = playlistGainSmoothed.getNextValue();
            hostL[i] *= hg;  hostR[i] *= hg;
            plL[i] *= pg;    plR[i] *= pg;
        }

        // --- Host/Playlist 個別メータリング（ゲイン適用後、ブレンド前） ---
        const float* hostCh[2] = { hostL, hostR };
        metering.processBuffer(hostCh, 2, n, MeteringService::Source::Host);
        const float* plCh[2] = { plL, plR };
        metering.processBuffer(plCh, 2, n, MeteringService::Source::Playlist);

        // --- ソースブレンド (0=Host, 1=Playlist) ---
        sourceBlendSmoothed.setTargetValue(sourceSelect == 0 ? 0.0f : 1.0f);
        for (int i = 0; i < n; ++i)
        {
            const float mix = sourceBlendSmoothed.getNextValue();
            const float inv = 1.0f - mix;
            outputL[i] = hostL[i] * inv + plL[i] * mix;
            outputR[i] = hostR[i] * inv + plR[i] * mix;
        }

        // --- LPF ---
        if (lpfEnabled && !lastLpfEnabled) { lpfLeft.reset(); lpfRight.reset(); }
        lastLpfEnabled = lpfEnabled;
        lpfMixSmoothed.setTargetValue(lpfEnabled ? 1.0f : 0.0f);

        const bool lpfActive = lpfEnabled || lpfMixSmoothed.isSmoothing();
        if (lpfActive)
        {
            float fL[512], fR[512];
            std::memcpy(fL, outputL, sizeof(float) * n);
            std::memcpy(fR, outputR, sizeof(float) * n);
            for (int i = 0; i < n; ++i)
            {
                fL[i] = lpfLeft.processSample(fL[i]);
                fR[i] = lpfRight.processSample(fR[i]);
            }
            for (int i = 0; i < n; ++i)
            {
                const float mix = lpfMixSmoothed.getNextValue();
                const float inv = 1.0f - mix;
                outputL[i] = outputL[i] * inv + fL[i] * mix;
                outputR[i] = outputR[i] * inv + fR[i] * mix;
            }
        }

        // --- 出力メータリング ---
        const float* outCh[2] = { outputL, outputR };
        metering.processBuffer(outCh, 2, n, MeteringService::Source::Output);
    }

private:
    double sampleRate = 44100.0;
    int blockSize = 128;

    // Host (デモ用 — playlist transport とは独立)
    Track hostTrack;
    double hostPlaybackPos = 0.0;
    bool hostPlaying = false;
    bool hostLoopEnabled = true;
    bool hostStoppedAtEnd = false;
    float hostGainDb = 0.0f;

    // Playlist
    std::vector<Track> tracks;
    int currentTrackIndex = -1;
    bool playing = false;
    double playbackPosSamples = 0.0;
    bool loopEnabled = false;
    double loopStartSec = 0.0;
    double loopEndSec = 0.0;
    bool stoppedAtEnd = false;
    float playlistGainDb = 0.0f;

    // ソース。plugin UI APVTS 既定 (Playlist) と一致させる。
    // ずらすと UI 未操作時に WASM/UI で食い違って意図しない音が出る。
    int sourceSelect = 1; // 0=Host, 1=Playlist

    // メータリング
    int meteringMode = 0; // 0=Peak,1=RMS,2=Momentary

    // DSP
    bool lpfEnabled = false;
    float lpfFreqHz = 20000.0f;
    bool lastLpfEnabled = false;
    SmoothedValue hostGainSmoothed{1.0f};
    SmoothedValue playlistGainSmoothed{1.0f};
    SmoothedValue sourceBlendSmoothed{1.0f};
    SmoothedValue lpfMixSmoothed{0.0f};
    StateVariableTPTFilter lpfLeft;
    StateVariableTPTFilter lpfRight;
    MeteringService metering;

    const Track* currentTrack() const
    {
        if (currentTrackIndex < 0 || currentTrackIndex >= static_cast<int>(tracks.size()))
            return nullptr;
        return &tracks[currentTrackIndex];
    }
};

} // namespace mc_wasm
