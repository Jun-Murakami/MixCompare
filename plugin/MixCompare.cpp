// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#include "MixCompare.h"
#include "IPlug_include_in_plug_src.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>  // 一時診断ログ (UnserializeState の chunk 形態確認用、解決後削除)

#include "json.hpp"
#include "zlib.h"  // JUCE 版 copyXmlToBinary (zlib) 状態の復元用 (WDL/zlib, mc_zlib でリンク)
#include "audio/AudioDecoder.h"
#include "audio/FileDialog.h"
#include "audio/InMemorySource.h"
#include "audio/StreamingSource.h"

using nlohmann::json;

#ifdef OS_WIN
// 前方宣言。定義は下部 "Window / WebView bounds" セクション。OnMessage の resize
// 経路 (kMsgWindowAction) が定義より前にあるため宣言を先出しする。
static float GetWindowDpiScale(HWND hwnd);

// 一時診断: UnserializeState の chunk 形態を %TEMP%\mixcompare_state.log に追記する。
// (fopen は WDL の fopenUTF8 マクロで std:: 経由が壊れるため ofstream を使う。)
// 解決後はこのヘルパとすべての mcStateLog 呼び出しを削除すること。
static void mcStateLog(const std::string& s)
{
  char tmp[MAX_PATH];
  if (GetTempPathA(MAX_PATH, tmp) == 0) return;
  std::ofstream f(std::string(tmp) + "mixcompare_state.log", std::ios::app);
  if (f) f << s;
}
#endif

namespace
{
// このサイズを超えるファイルは StreamingSource で逐次再生する (それ以下は全展開)。
constexpr int64_t kStreamingThresholdBytes = 32LL * 1024 * 1024; // 32 MB

// Unicode 安全なテキスト書き/読み (m3u8 用)。
bool writeTextFileUtf8(const std::string& pathUtf8, const std::string& content)
{
  FILE* fp = nullptr;
#ifdef OS_WIN
  const int wlen = MultiByteToWideChar(CP_UTF8, 0, pathUtf8.c_str(), static_cast<int>(pathUtf8.size()), nullptr, 0);
  if (wlen <= 0) return false;
  std::wstring wpath(static_cast<size_t>(wlen), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, pathUtf8.c_str(), static_cast<int>(pathUtf8.size()), wpath.data(), wlen);
  fp = _wfopen(wpath.c_str(), L"wb");
#else
  fp = std::fopen(pathUtf8.c_str(), "wb");
#endif
  if (!fp) return false;
  if (!content.empty()) std::fwrite(content.data(), 1, content.size(), fp);
  std::fclose(fp);
  return true;
}

std::string readTextFileUtf8(const std::string& pathUtf8)
{
  std::string out;
  FILE* fp = nullptr;
#ifdef OS_WIN
  const int wlen = MultiByteToWideChar(CP_UTF8, 0, pathUtf8.c_str(), static_cast<int>(pathUtf8.size()), nullptr, 0);
  if (wlen <= 0) return out;
  std::wstring wpath(static_cast<size_t>(wlen), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, pathUtf8.c_str(), static_cast<int>(pathUtf8.size()), wpath.data(), wlen);
  fp = _wfopen(wpath.c_str(), L"rb");
#else
  fp = std::fopen(pathUtf8.c_str(), "rb");
#endif
  if (!fp) return out;
  std::fseek(fp, 0, SEEK_END); const long sz = std::ftell(fp); std::fseek(fp, 0, SEEK_SET);
  if (sz > 0) { out.resize(static_cast<size_t>(sz)); if (std::fread(&out[0], 1, out.size(), fp) != out.size()) out.clear(); }
  std::fclose(fp);
  return out;
}
} // namespace

#ifdef OS_WIN
#include <windows.h>
#include "resources/resource.h"
extern HINSTANCE gHINSTANCE;
#endif

MixCompare::MixCompare(const InstanceInfo& info)
    // CLAP ビルドでは IPlug_include_in_plug_src.h が clap::helpers::Plugin をスコープに
    // 持ち込むため、無修飾 Plugin は ctor 初期化子で曖昧になる。iplug:: で明示修飾する
    // (公式 IPlugWebUI 例と同じ作法)。
    : iplug::Plugin(info, MakeConfig(kNumParams, kNumPresets))
{
  // パラメータ JSON は base64 で JS に渡されるため、iPlug2 既定 (8192) を拡張する。
  SetMaxJSStringLength(64 * 1024);

  // 開発者ツール / WebView 既定の右クリックメニューは Debug のみ有効化する。
  // Release では無効化し、製品ビルドで Inspect 等が出ないようにする。
  // (WebView 生成時に GetEnableDevTools() が参照されるため ctor で設定する。
  //  mEditorInitFunc は生成後に走るので遅すぎる。)
#ifdef _DEBUG
  SetEnableDevTools(true);
#else
  SetEnableDevTools(false);
#endif

  // === 14 IParam (JUCE 版 APVTS createParameterLayout と同順・同範囲) ===
  // gain は -120..0 dB を線形正規化 (JUCE NormalisableRange と同じ。曲線は WebUI 側)。
  GetParam(kHostGain)->InitDouble("Host Gain", 0.0, -120.0, 0.0, 0.1, "dB");
  GetParam(kPlaylistGain)->InitDouble("Playlist Gain", 0.0, -120.0, 0.0, 0.1, "dB");
  // LPF freq は 20..20000 Hz を線形正規化 (JUCE と同じ。log カーブは WebUI スライダ側)。
  GetParam(kLpfFreq)->InitDouble("LPF Frequency (24dB/oct)", 120.0, 20.0, 20000.0, 0.01, "Hz");
  GetParam(kLpfEnabled)->InitBool("LPF Enabled", false);

  GetParam(kHostSyncCapable)->InitBool("Host Sync Capable", true, "",
                                       IParam::kFlagCannotAutomate | IParam::kFlagMeta);
  GetParam(kHostSyncEnabled)->InitBool("Sync To Host", false, "", IParam::kFlagMeta);

  GetParam(kSourceSelect)->InitEnum("Source Select", kSourceHost, {"Host", "Playlist"});
  GetParam(kMeteringMode)->InitEnum("Metering Mode", kMeteringPeak, {"Peak", "RMS", "Momentary"});

  GetParam(kTransportPlaying)->InitBool("Transport Playing", false, "", IParam::kFlagCannotAutomate);
  GetParam(kTransportLoopEnabled)->InitBool("Loop Enabled (Transport)", false, "", IParam::kFlagCannotAutomate);
  GetParam(kTransportSeekNorm)->InitDouble("Transport Seek (Normalized)", 0.0, 0.0, 1.0, 0.0001, "", IParam::kFlagCannotAutomate);
  GetParam(kLoopStartNorm)->InitDouble("Loop Start (Normalized)", 0.0, 0.0, 1.0, 0.0001, "", IParam::kFlagCannotAutomate);
  GetParam(kLoopEndNorm)->InitDouble("Loop End (Normalized)", 1.0, 0.0, 1.0, 0.0001, "", IParam::kFlagCannotAutomate);
  GetParam(kPlaylistCurrentIndexNorm)->InitDouble("Playlist Current Index (Normalized)", 0.0, 0.0, 1.0, 0.0001, "", IParam::kFlagCannotAutomate);

#ifdef APP_API
  // Standalone は DAW トランスポートが無いため Host Sync 不可。UI トグルを無効化させる。
  GetParam(kHostSyncCapable)->Set(0.0);
#endif

  // WebView ロードコールバック。base が WebView を生成したあとに呼ばれる。
  mEditorInitFunc = [&]() {
    LoadIndexHtmlForCurrentBuild();
    EnableScroll(false);
  };

  StartDecodeThread();
}

MixCompare::~MixCompare()
{
  StopDecodeThread();
}

// ---------------------------------------------------------------------------
// Audio thread
// ---------------------------------------------------------------------------
void MixCompare::OnReset()
{
  const double sr = GetSampleRate();
  const int bs = GetBlockSize();
  mEngine.prepare(sr, bs);

  const int cap = std::max(bs, 8192);
  mInL.assign(cap, 0.0f);
  mInR.assign(cap, 0.0f);
  mOutL.assign(cap, 0.0f);
  mOutR.assign(cap, 0.0f);

  // 現在の全パラメータ値をエンジンに反映 (セッション復元/SR 変更時)。
  mEngine.setHostGainDb(static_cast<float>(GetParam(kHostGain)->Value()));
  mEngine.setPlaylistGainDb(static_cast<float>(GetParam(kPlaylistGain)->Value()));
  mEngine.setLpfFrequency(static_cast<float>(GetParam(kLpfFreq)->Value()));
  mEngine.setLpfEnabled(GetParam(kLpfEnabled)->Bool());
  mEngine.setSourceSelect(GetParam(kSourceSelect)->Int());
  mEngine.setMeteringMode(GetParam(kMeteringMode)->Int());
}

void MixCompare::OnParamChange(int paramIdx)
{
  // audio thread からも呼ばれうる。係数差し替えのみ (alloc/lock/log 禁止)。
  switch (paramIdx)
  {
    case kHostGain:     mEngine.setHostGainDb(static_cast<float>(GetParam(kHostGain)->Value())); break;
    case kPlaylistGain: mEngine.setPlaylistGainDb(static_cast<float>(GetParam(kPlaylistGain)->Value())); break;
    case kLpfFreq:      mEngine.setLpfFrequency(static_cast<float>(GetParam(kLpfFreq)->Value())); break;
    case kLpfEnabled:   mEngine.setLpfEnabled(GetParam(kLpfEnabled)->Bool()); break;
    case kSourceSelect: mEngine.setSourceSelect(GetParam(kSourceSelect)->Int()); break;
    case kMeteringMode: mEngine.setMeteringMode(GetParam(kMeteringMode)->Int()); break;
    // --- Playlist トランスポート (engine の atomic 更新のみ。UI データ非依存) ---
    case kTransportPlaying:     mEngine.setPlaying(GetParam(kTransportPlaying)->Bool()); break;
    case kTransportLoopEnabled: mEngine.setLoopEnabled(GetParam(kTransportLoopEnabled)->Bool());
                                UpdateLoopRangeFromParams(); break;
    case kTransportSeekNorm:    mEngine.seekSeconds(GetParam(kTransportSeekNorm)->Value()
                                                    * mEngine.getActiveDurationSec()); break;
    case kLoopStartNorm:
    case kLoopEndNorm:          UpdateLoopRangeFromParams(); break;
    // kPlaylistCurrentIndexNorm はトラック選択 (UI データ依存) なので OnParamChangeUI で処理。
    default: break;
  }
}

void MixCompare::OnParamChangeUI(int paramIdx, EParamSource source)
{
  if (paramIdx < 0 || paramIdx >= kNumParams) return;

  // トラック選択は UI データ (mPlaylist) 依存なので UI スレッドのここで処理する
  // (WebUI は PLAYLIST_CURRENT_INDEX_NORM の setNormalisedValue で選択する)。
  if (paramIdx == kPlaylistCurrentIndexNorm)
  {
    if (mInSelect) return; // SelectTrackByIndex 内の param 設定による再入を無視
    const int count = static_cast<int>(mPlaylist.size());
    if (count > 0)
    {
      const double norm = GetParam(kPlaylistCurrentIndexNorm)->Value();
      const int idx = (count > 1) ? static_cast<int>(norm * (count - 1) + 0.5) : 0;
      SelectTrackByIndex(idx);
    }
    return;
  }

  // UI 由来の変更はフロントが既に値を持っているので送り返さない (echo 防止)。
  // host automation / preset / recall 由来のときだけ SPVFD で WebUI に反映する。
  if (source == kUI) return;
  SendParameterValueFromDelegate(paramIdx, GetParam(paramIdx)->GetNormalized(), true);
}

void MixCompare::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
#ifndef APP_API
  // Host Sync: HOST_SYNC_ENABLED 時、Playlist 再生を DAW トランスポートに追従させる。
  // DAW タイムライン秒 = トラック秒の 1:1 マッピング。drift/ジャンプ時のみ seek。
  if (GetParam(kHostSyncEnabled)->Bool())
  {
    const double sr = GetSampleRate();
    const double posSec = (sr > 0.0) ? GetSamplePos() / sr : -1.0;
    if (posSec >= 0.0)
    {
      const bool running = GetTransportIsRunning();
      mEngine.setPlaying(running);
      if (running)
      {
        if (std::fabs(mEngine.getPositionSec() - posSec) > 0.05)
          mEngine.seekSeconds(posSec);
      }
      else
      {
        mEngine.seekSeconds(posSec); // 停止中は DAW のロケート位置に追従
      }
    }
  }
#endif

  const int nIn = NInChansConnected();
  const int nOut = NOutChansConnected();
  const int cap = static_cast<int>(mInL.size());
  const int n = std::min(nFrames, cap);

  const sample* inL = (nIn > 0) ? inputs[0] : nullptr;
  const sample* inR = (nIn > 1) ? inputs[1] : inL; // mono 入力は L を複製

  for (int i = 0; i < n; ++i)
  {
    mInL[i] = inL ? static_cast<float>(inL[i]) : 0.0f;
    mInR[i] = inR ? static_cast<float>(inR[i]) : 0.0f;
  }

  mEngine.processBlock(mInL.data(), mInR.data(), mOutL.data(), mOutR.data(), n);

  for (int ch = 0; ch < nOut; ++ch)
  {
    const float* src = (ch == 0) ? mOutL.data() : mOutR.data();
    for (int i = 0; i < n; ++i)
      outputs[ch][i] = static_cast<sample>(src[i]);
    // n を超える分 (理論上発生しない) は無音で埋める。
    for (int i = n; i < nFrames; ++i)
      outputs[ch][i] = 0.0;
  }
}

// ---------------------------------------------------------------------------
// UI thread
// ---------------------------------------------------------------------------
void MixCompare::OnIdle()
{
  // セッション復元: UnserializeState で積まれたプレイリストを UI スレッドで再構築する。
  if (mPendingStateRestore.exchange(false))
    RestorePlaylistFromState();

  DrainDecodeResults();
  SendMeterSnapshot();

  // 再生終端に達したら Play パラメータを Off に同期して UI に通知。
  if (mEngine.consumeStoppedAtEnd())
  {
    GetParam(kTransportPlaying)->Set(0.0);
    SendParameterValueFromDelegate(kTransportPlaying, 0.0, true);
    SendTransportUpdate();
  }

  // 再生位置の高頻度更新 (~30Hz)。
  if (!mPlaylist.empty())
    SendTransportPositionUpdate();
}

void MixCompare::SendMeterSnapshot()
{
  // 24-float snapshot を WebUI の MeterUpdateData (host/playlist/output のネスト構造)
  // JSON に変換して送る。レイアウトは AudioEngine::fillMeterSnapshot / Web デモの
  // dsp_get_meter_data と一致:
  //   [0]=mode
  //   host:     [3]=tpL [4]=tpR [5]=rmsL [6]=rmsR [7]=mom [8]=momHold
  //   playlist: [11]=tpL [12]=tpR [13]=rmsL [14]=rmsR [15]=mom [16]=momHold
  //   output:   [17]=rmsL [18]=rmsR [19]=mom [20]=momHold
  float m[mc::AudioEngine::kMeterFloats];
  mEngine.readMeterSnapshot(m);

  char buf[640];
  const int n = std::snprintf(
      buf, sizeof(buf),
      "{\"meteringMode\":%d,"
      "\"host\":{\"rmsLeft\":%.2f,\"rmsRight\":%.2f,\"truePeakLeft\":%.2f,\"truePeakRight\":%.2f,\"momentary\":%.2f,\"momentaryHold\":%.2f},"
      "\"playlist\":{\"rmsLeft\":%.2f,\"rmsRight\":%.2f,\"truePeakLeft\":%.2f,\"truePeakRight\":%.2f,\"momentary\":%.2f,\"momentaryHold\":%.2f},"
      "\"output\":{\"rmsLeft\":%.2f,\"rmsRight\":%.2f,\"momentary\":%.2f,\"momentaryHold\":%.2f}}",
      static_cast<int>(m[0]),
      m[5], m[6], m[3], m[4], m[7], m[8],
      m[13], m[14], m[11], m[12], m[15], m[16],
      m[17], m[18], m[19], m[20]);
  if (n > 0)
    SendArbitraryMsgFromDelegate(kMsgMeterUpdate, n, buf);
}

void MixCompare::OnWebContentLoaded()
{
  // base がパラメータ JSON schema を SAMFD(-1) で送る。先に呼ぶ。
  WebViewEditorDelegate::OnWebContentLoaded();

  // 現在の全 IParam 値を normalized で WebUI に送る (SPVFD)。
  for (int i = 0; i < kNumParams; ++i)
    SendParameterValueFromDelegate(i, GetParam(i)->GetNormalized(), true);

  SendInitialStateToWeb();
}

void MixCompare::SendInitialStateToWeb()
{
  SendPlaylistUpdate();
  SendTransportUpdate();
}

// プレイリスト items 配列を JSON 化する (PlaylistItem は private nested なのでメンバ内で構築)。
json MixCompare::BuildPlaylistItemsJson() const
{
  json arr = json::array();
  for (const auto& it : mPlaylist)
  {
    arr.push_back({
      {"id", it.id},
      {"name", it.name},
      {"duration", it.durationSec},
      {"isLoaded", it.loaded},
      {"file", it.path},
      {"exists", it.exists},
    });
  }
  return arr;
}

void MixCompare::SendPlaylistUpdate()
{
  const json j = {
    {"items", BuildPlaylistItemsJson()},
    {"currentIndex", mCurrentIndex},
    {"revision", mPlaylistRevision},
  };
  const std::string s = j.dump();
  SendArbitraryMsgFromDelegate(kMsgPlaylistUpdate, static_cast<int>(s.size()), s.data());
}

void MixCompare::SendTransportUpdate()
{
  const double dur = mEngine.getActiveDurationSec();
  const json j = {
    {"isPlaying", GetParam(kTransportPlaying)->Bool()},
    {"position", mEngine.getPositionSec()},
    {"duration", dur},
    {"loopStart", GetParam(kLoopStartNorm)->Value() * dur},
    {"loopEnd", GetParam(kLoopEndNorm)->Value() * dur},
    {"loopEnabled", GetParam(kTransportLoopEnabled)->Bool()},
    {"currentIndex", mCurrentIndex},
    {"sequenceNumber", mTransportSeq},
    {"sessionId", mTransportSeq},
    {"revision", mPlaylistRevision},
  };
  const std::string s = j.dump();
  SendArbitraryMsgFromDelegate(kMsgTransportUpdate, static_cast<int>(s.size()), s.data());
}

void MixCompare::SendTransportPositionUpdate()
{
  const json j = {
    {"position", mEngine.getPositionSec()},
    {"isPlaying", GetParam(kTransportPlaying)->Bool()},
    {"duration", mEngine.getActiveDurationSec()},
    {"sessionId", mTransportSeq},
  };
  const std::string s = j.dump();
  SendArbitraryMsgFromDelegate(kMsgTransportPositionUpdate, static_cast<int>(s.size()), s.data());
}

void MixCompare::SendTrackChange()
{
  const json j = {
    {"items", BuildPlaylistItemsJson()},
    {"currentIndex", mCurrentIndex},
    {"isPlaying", GetParam(kTransportPlaying)->Bool()},
    {"position", mEngine.getPositionSec()},
    {"loopEnabled", GetParam(kTransportLoopEnabled)->Bool()},
    {"sessionId", mTransportSeq},
    {"playlistRevision", mPlaylistRevision},
  };
  const std::string s = j.dump();
  SendArbitraryMsgFromDelegate(kMsgTrackChange, static_cast<int>(s.size()), s.data());
}

// ---------------------------------------------------------------------------
// バックグラウンドデコード
// ---------------------------------------------------------------------------
void MixCompare::StartDecodeThread()
{
  mDecodeStop = false;
  mDecodeThread = std::thread([this]() {
    for (;;)
    {
      DecodeJob job;
      {
        std::unique_lock<std::mutex> lk(mJobMutex);
        mJobCv.wait(lk, [this]() { return mDecodeStop || !mJobs.empty(); });
        if (mDecodeStop && mJobs.empty()) return;
        job = std::move(mJobs.front());
        mJobs.pop_front();
      }
      DecodeResult res;
      res.id = job.id;
      res.streaming = job.streaming;
      if (job.streaming)
      {
        // 大ファイル: duration だけ取得 (全 PCM は select 時に StreamingSource が逐次読む)。
        mc::AudioInfo info = mc::AudioDecoder::probe(job.path);
        if (info.ok) { res.durationSec = info.durationSec; res.ok = true; }
      }
      else
      {
        mc::DecodedAudio dec = mc::AudioDecoder::decodeFull(job.path);
        if (dec.ok)
        {
          auto track = std::make_shared<mc::TrackAudio>();
          track->left = std::move(dec.left);
          track->right = std::move(dec.right);
          track->sampleRate = dec.sampleRate;
          track->numFrames = dec.numFrames;
          track->durationSec = dec.durationSec;
          res.audio = std::move(track);
          res.durationSec = dec.durationSec;
          res.ok = true;
        }
      }
      {
        std::lock_guard<std::mutex> lk(mResultMutex);
        mResults.push_back(std::move(res));
      }
    }
  });
}

void MixCompare::StopDecodeThread()
{
  {
    std::lock_guard<std::mutex> lk(mJobMutex);
    mDecodeStop = true;
  }
  mJobCv.notify_all();
  if (mDecodeThread.joinable()) mDecodeThread.join();
}

void MixCompare::EnqueueDecode(const std::string& id, const std::string& path, bool streaming)
{
  {
    std::lock_guard<std::mutex> lk(mJobMutex);
    mJobs.push_back({id, path, streaming});
  }
  mJobCv.notify_one();
}

int MixCompare::FindItemIndexById(const std::string& id) const
{
  for (size_t i = 0; i < mPlaylist.size(); ++i)
    if (mPlaylist[i].id == id) return static_cast<int>(i);
  return -1;
}

void MixCompare::DrainDecodeResults()
{
  std::deque<DecodeResult> results;
  {
    std::lock_guard<std::mutex> lk(mResultMutex);
    if (mResults.empty()) return;
    results.swap(mResults);
  }
  for (auto& res : results)
  {
    const int idx = FindItemIndexById(res.id);
    if (idx < 0) continue; // デコード完了前に削除された
    auto& item = mPlaylist[static_cast<size_t>(idx)];
    if (res.ok)
    {
      item.streaming = res.streaming;
      item.audio = res.audio;          // streaming のときは nullptr (select 時に逐次読み)
      item.durationSec = res.durationSec;
      item.loaded = true;
      item.exists = true;
      // 現在トラックがこれなら engine 反映。未選択なら自動選択。
      if (idx == mCurrentIndex || mCurrentIndex < 0)
        SelectTrackByIndex(idx);
    }
    else
    {
      item.loaded = false;
      item.exists = false;
    }
  }
  ++mPlaylistRevision;
  RebuildStateJson();
  SendPlaylistUpdate();
}

// ---------------------------------------------------------------------------
// プレイリスト操作 / トラック選択 / ループ
// ---------------------------------------------------------------------------
void MixCompare::UpdateLoopRangeFromParams()
{
  const double dur = mEngine.getActiveDurationSec();
  const double start = GetParam(kLoopStartNorm)->Value() * dur;
  double end = GetParam(kLoopEndNorm)->Value() * dur;
  if (end <= 0.0) end = dur;
  mEngine.setLoopRangeSec(start, end);
}

void MixCompare::SelectTrackByIndex(int index)
{
  if (mPlaylist.empty())
  {
    mCurrentIndex = -1;
    mEngine.clearCurrentSource();
    return;
  }
  index = std::max(0, std::min(index, static_cast<int>(mPlaylist.size()) - 1));
  mCurrentIndex = index;
  ++mTransportSeq;

  const auto& item = mPlaylist[static_cast<size_t>(index)];
  if (item.loaded && item.streaming)
    mEngine.setCurrentSource(std::make_shared<mc::StreamingSource>(item.path));
  else if (item.loaded && item.audio)
    mEngine.setCurrentSource(std::make_shared<mc::InMemorySource>(item.audio));
  else
    mEngine.clearCurrentSource();

  UpdateLoopRangeFromParams();

  // PLAYLIST_CURRENT_INDEX_NORM を UI 同期 (OnParamChangeUI 再入は mInSelect で無視)。
  {
    mInSelect = true;
    const int count = static_cast<int>(mPlaylist.size());
    const double norm = (count > 1) ? static_cast<double>(index) / (count - 1) : 0.0;
    GetParam(kPlaylistCurrentIndexNorm)->Set(norm);
    SendParameterValueFromDelegate(kPlaylistCurrentIndexNorm, norm, true);
    mInSelect = false;
  }

  RebuildStateJson();
  SendTrackChange();
  SendTransportUpdate();
}

void MixCompare::HandlePlaylistAction(const std::string& jsonText)
{
  json msg = json::parse(jsonText, nullptr, false);
  if (msg.is_discarded() || !msg.contains("args") || !msg["args"].is_array()) return;
  const auto& args = msg["args"];
  if (args.empty() || !args[0].is_string()) return;
  const std::string action = args[0].get<std::string>();

  if (action == "add")
  {
#ifdef OS_WIN
    void* parent = mNativeParent;
#else
    void* parent = nullptr;
#endif
    std::vector<std::string> files = mc::PromptForAudioFiles(parent);
    if (files.empty()) return;
    for (const auto& path : files)
      AppendPlaylistItem(path);
    ++mPlaylistRevision;
    RebuildStateJson();
    SendPlaylistUpdate();
  }
  else if (action == "remove" && args.size() >= 2 && args[1].is_string())
  {
    const int idx = FindItemIndexById(args[1].get<std::string>());
    if (idx < 0) return;
    mPlaylist.erase(mPlaylist.begin() + idx);
    if (mCurrentIndex == idx)
    {
      if (mPlaylist.empty()) { mCurrentIndex = -1; mEngine.clearCurrentSource(); }
      else SelectTrackByIndex(std::min(idx, static_cast<int>(mPlaylist.size()) - 1));
    }
    else if (mCurrentIndex > idx)
    {
      --mCurrentIndex;
    }
    ++mPlaylistRevision;
    RebuildStateJson();
    SendPlaylistUpdate();
    SendTransportUpdate();
  }
  else if (action == "reorder" && args.size() >= 3 && args[1].is_number() && args[2].is_number())
  {
    const int n = static_cast<int>(mPlaylist.size());
    const int from = args[1].get<int>();
    const int to = args[2].get<int>();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    PlaylistItem moved = mPlaylist[static_cast<size_t>(from)];
    mPlaylist.erase(mPlaylist.begin() + from);
    mPlaylist.insert(mPlaylist.begin() + to, std::move(moved));
    if (mCurrentIndex == from) mCurrentIndex = to;
    else if (from < mCurrentIndex && to >= mCurrentIndex) --mCurrentIndex;
    else if (from > mCurrentIndex && to <= mCurrentIndex) ++mCurrentIndex;
    // 再生中トラックは変えず、index param のみ新位置に同期する。
    if (mCurrentIndex >= 0)
    {
      mInSelect = true;
      const int count = static_cast<int>(mPlaylist.size());
      const double norm = (count > 1) ? static_cast<double>(mCurrentIndex) / (count - 1) : 0.0;
      GetParam(kPlaylistCurrentIndexNorm)->Set(norm);
      SendParameterValueFromDelegate(kPlaylistCurrentIndexNorm, norm, true);
      mInSelect = false;
    }
    ++mPlaylistRevision;
    RebuildStateJson();
    SendPlaylistUpdate();
  }
  else if (action == "clear")
  {
    mPlaylist.clear();
    mCurrentIndex = -1;
    mEngine.clearCurrentSource();
    mEngine.setPlaying(false);
    GetParam(kTransportPlaying)->Set(0.0);
    SendParameterValueFromDelegate(kTransportPlaying, 0.0, true);
    ++mPlaylistRevision;
    RebuildStateJson();
    SendPlaylistUpdate();
    SendTransportUpdate();
  }
  else if (action == "export")
  {
#ifdef OS_WIN
    void* parent = mNativeParent;
#else
    void* parent = nullptr;
#endif
    std::string out;
    if (mc::PromptForSavePlaylist(parent, out))
      ExportPlaylistM3U8(out);
  }
  else if (action == "import")
  {
#ifdef OS_WIN
    void* parent = mNativeParent;
#else
    void* parent = nullptr;
#endif
    std::string in;
    if (mc::PromptForOpenPlaylist(parent, in))
      ImportPlaylistM3U8(in);
  }
}

void MixCompare::AppendPlaylistItem(const std::string& path)
{
  if (!mc::AudioDecoder::isSupported(path)) return;
  PlaylistItem item;
  item.id = "item-" + std::to_string(mNextItemId++);
  item.path = path;
  const auto slash = path.find_last_of("/\\");
  item.name = (slash == std::string::npos) ? path : path.substr(slash + 1);
  item.streaming = mc::AudioDecoder::isStreamable(path)
                && (mc::AudioDecoder::fileSizeBytes(path) > kStreamingThresholdBytes);
  mPlaylist.push_back(item);
  EnqueueDecode(item.id, item.path, item.streaming);
}

void MixCompare::ExportPlaylistM3U8(const std::string& pathUtf8) const
{
  std::string content = "#EXTM3U\n";
  for (const auto& it : mPlaylist)
  {
    std::string title = it.name;
    const auto dot = title.find_last_of('.');
    if (dot != std::string::npos && dot > 0) title = title.substr(0, dot);
    content += "#EXTINF:" + std::to_string(static_cast<int>(it.durationSec)) + "," + title + "\n";
    content += it.path + "\n";
  }
  writeTextFileUtf8(pathUtf8, content);
}

void MixCompare::ImportPlaylistM3U8(const std::string& pathUtf8)
{
  const std::string content = readTextFileUtf8(pathUtf8);
  if (content.empty()) return;

  // 既存プレイリストを置き換える (JUCE 版 importPlaylistFromM3U8 と同じ挙動)。
  mPlaylist.clear();
  mCurrentIndex = -1;
  mEngine.clearCurrentSource();
  mEngine.setPlaying(false);
  GetParam(kTransportPlaying)->Set(0.0);
  SendParameterValueFromDelegate(kTransportPlaying, 0.0, true);

  // 行ごとにパース (# 行と空行はスキップ)。
  size_t pos = 0;
  while (pos < content.size())
  {
    size_t eol = content.find('\n', pos);
    if (eol == std::string::npos) eol = content.size();
    std::string line = content.substr(pos, eol - pos);
    pos = eol + 1;
    // CR / 前後空白を除去
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
    size_t s = 0; while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
    line = line.substr(s);
    if (line.empty() || line[0] == '#') continue;
    AppendPlaylistItem(line);
  }

  ++mPlaylistRevision;
  RebuildStateJson();
  SendPlaylistUpdate();
  SendTransportUpdate();
}

// ---------------------------------------------------------------------------
// 永続化スナップショット
// ---------------------------------------------------------------------------
void MixCompare::RebuildStateJson()
{
  json items = json::array();
  for (const auto& it : mPlaylist)
    items.push_back({{"id", it.id}, {"path", it.path}, {"name", it.name}});
  const json j = {{"playlist", items}, {"currentIndex", mCurrentIndex}};
  const std::string s = j.dump();
  std::lock_guard<std::mutex> lock(mStateMutex);
  mStateJson = s;
}

void MixCompare::RestorePlaylistFromState()
{
  std::string snapshot;
  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    snapshot = mStateJson;
  }
  if (snapshot.empty()) return;
  json j = json::parse(snapshot, nullptr, false);
  if (j.is_discarded()) return;

  mPlaylist.clear();
  mCurrentIndex = -1;
  if (j.contains("playlist") && j["playlist"].is_array())
  {
    for (const auto& e : j["playlist"])
    {
      PlaylistItem item;
      item.id = e.value("id", std::string("item-") + std::to_string(mNextItemId++));
      item.path = e.value("path", std::string());
      item.name = e.value("name", std::string());
      if (item.path.empty()) continue;
      item.streaming = mc::AudioDecoder::isStreamable(item.path)
                    && (mc::AudioDecoder::fileSizeBytes(item.path) > kStreamingThresholdBytes);
      mPlaylist.push_back(item);
      EnqueueDecode(item.id, item.path, item.streaming);
    }
  }
  // 復元 currentIndex は decode 完了後に有効化されるよう保持しておく。
  if (j.contains("currentIndex") && j["currentIndex"].is_number())
    mCurrentIndex = j["currentIndex"].get<int>();
  if (mCurrentIndex >= static_cast<int>(mPlaylist.size())) mCurrentIndex = -1;

  ++mPlaylistRevision;
  SendPlaylistUpdate();
}

bool MixCompare::OnMessage(int msgTag, int /*ctrlTag*/, int dataSize, const void* pData)
{
  switch (msgTag)
  {
    case kMsgSystemAction:
    {
      // payload はアクション名 (ready / test_error / forward_key_event ...)。
      // ready のときだけ初期状態を送り直す。
      const std::string action = (pData && dataSize > 0)
          ? std::string(static_cast<const char*>(pData), static_cast<size_t>(dataSize))
          : std::string();
      if (action == "ready")
        SendInitialStateToWeb();
      return true;
    }

    case kMsgRequestPlaylistUpdate:
      SendPlaylistUpdate();
      return true;

    case kMsgMeteringReset:
      mEngine.requestResetTruePeak();
      mEngine.requestResetMomentaryHold();
      return true;

    case kMsgWindowAction:
      // resizeTo: payload = int32 width, int32 height (logical CSS px)。
      if (pData != nullptr && dataSize >= static_cast<int>(sizeof(int32_t) * 2))
      {
        const int32_t* p = static_cast<const int32_t*>(pData);
        // 内部 (WebUI ResizeGripper) 経路は logical CSS px。logical 範囲で clamp する
        // (host の checkSizeConstraint 経路の ConstrainEditorResize は physical 換算する
        //  ので、こちらで override を呼ぶと logical 値が physical 範囲に丸められて壊れる)。
        int wLogical = p[0];
        int hLogical = p[1];
        {
          using namespace editor_size;
          wLogical = std::max(kMinWidth, std::min(wLogical, kMaxWidth));
          hLogical = std::max(kMinHeight, std::min(hLogical, kMaxHeight));
        }
#if defined(OS_WIN) && defined(APP_API)
        // Standalone: iPlug2 APP::EditorResize は (logical + ncW_phys)*scale で client を
        // 過大化させるため、自前で正しい物理サイズに SetWindowPos する(OpenWindow 補正と同式)。
        if (HWND hwnd = static_cast<HWND>(mNativeParent))
        {
          const float scale = GetWindowDpiScale(hwnd);
          RECT rcClient{}, rcWindow{};
          GetClientRect(hwnd, &rcClient);
          GetWindowRect(hwnd, &rcWindow);
          const int ncW = (rcWindow.right - rcWindow.left) - rcClient.right;
          const int ncH = (rcWindow.bottom - rcWindow.top) - rcClient.bottom;
          const int physW = static_cast<int>(wLogical * scale + 0.5f);
          const int physH = static_cast<int>(hLogical * scale + 0.5f);
          SetEditorSize(wLogical, hLogical);
          SetWindowPos(hwnd, nullptr, 0, 0, physW + ncW, physH + ncH,
                       SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
          return true;
        }
#endif
        int wToHost = wLogical;
        int hToHost = hLogical;
#if defined(OS_WIN) && defined(VST3_API)
        // VST3 host は resizeView 引数を physical px と解釈するので logical→physical 変換。
        if (HWND hwnd = static_cast<HWND>(mNativeParent))
        {
          const float scale = GetWindowDpiScale(hwnd);
          wToHost = static_cast<int>(wLogical * scale + 0.5f);
          hToHost = static_cast<int>(hLogical * scale + 0.5f);
        }
#endif
        EditorResizeFromUI(wToHost, hToHost, true);
#if defined(OS_WIN) && defined(AAX_API)
        // AAX wrapper には onSize callback が無いので bounds を明示的に 100% DPI
        // physical px で揃える。
        SetAAXWebViewBoundsPhysical(wLogical, hLogical);
#endif
      }
      return true;

    case kMsgOpenUrl:
    {
      if (pData == nullptr || dataSize <= 0) return true;
      std::string url(static_cast<const char*>(pData), static_cast<size_t>(dataSize));
      const bool okScheme = url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0;
      if (!okScheme) return true;
#ifdef OS_WIN
      const int wlen = MultiByteToWideChar(CP_UTF8, 0, url.c_str(),
                                           static_cast<int>(url.size()), nullptr, 0);
      if (wlen > 0)
      {
        std::wstring wurl(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, url.c_str(), static_cast<int>(url.size()),
                            wurl.data(), wlen);
        ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      }
#endif
      return true;
    }

    case kMsgPlaylistAction:
      if (pData != nullptr && dataSize > 0)
        HandlePlaylistAction(std::string(static_cast<const char*>(pData),
                                         static_cast<size_t>(dataSize)));
      return true;

    default:
      return false;
  }
}

bool MixCompare::ConstrainEditorResize(int& w, int& h) const
{
  using namespace editor_size;
  const int origW = w, origH = h;
#if defined(OS_WIN) && defined(VST3_API)
  // Cubase 等の VST3 host は高 DPI で checkSizeConstraint の ViewRect を physical px
  // として扱う。logical の min/max をそのまま返すと 1.5x で見切れる/過大になるので、
  // physical px に scale して clamp する。host 経路 (checkSizeConstraint) 専用。
  if (HWND hwnd = static_cast<HWND>(mNativeParent))
  {
    const float scale = GetWindowDpiScale(hwnd);
    if (scale != 1.0f)
    {
      const int minW = static_cast<int>(kMinWidth * scale + 0.5f);
      const int maxW = static_cast<int>(kMaxWidth * scale + 0.5f);
      const int minH = static_cast<int>(kMinHeight * scale + 0.5f);
      const int maxH = static_cast<int>(kMaxHeight * scale + 0.5f);
      w = std::max(minW, std::min(w, maxW));
      h = std::max(minH, std::min(h, maxH));
      return (w == origW && h == origH);
    }
  }
#endif
  w = std::max(kMinWidth, std::min(w, kMaxWidth));
  h = std::max(kMinHeight, std::min(h, kMaxHeight));
  return (w == origW && h == origH);
}

// ---------------------------------------------------------------------------
// Window / WebView bounds (Windows DPI handling)
// ---------------------------------------------------------------------------
//
// iPlug2 + WebView2 + Windows には DPI 周りの不整合がある:
//   * IPlugAPP_main が Per-Monitor V2 awareness を立てるので Win32 API は physical px。
//   * GetEditorWidth/Height は logical px(PLUG_WIDTH)。両者を混ぜると DPI scale 分崩れる。
//   * SetWebViewBounds は内部で GetScaleForHWND(parent) を掛けるので logical px を渡す。
//   * Standalone: IPlugAPP_dialog::ClientResize が logical を physical として SetWindowPos
//     に渡すため、1.5x で window が縮み内容が見切れる(=報告された不具合)。
//   * VST3: host は getSize()/resizeView 引数を physical px として解釈する。
// 詳細な背景は Synth80 の docs/windows-dpi-webview-aax.md を参照。
#ifdef OS_WIN
// 親ウィンドウの DPI スケール(Per-Monitor V2 では現モニタの DPI)。古い OS は 1.0。
static float GetWindowDpiScale(HWND hwnd)
{
  if (!hwnd) return 1.0f;
  using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
  static GetDpiForWindowFn pGetDpiForWindow = []() -> GetDpiForWindowFn {
    HMODULE h = GetModuleHandleW(L"user32.dll");
    return h ? reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(h, "GetDpiForWindow"))
             : nullptr;
  }();
  if (!pGetDpiForWindow) return 1.0f;
  const UINT dpi = pGetDpiForWindow(hwnd);
  return dpi == 0 ? 1.0f : static_cast<float>(dpi) / 96.0f;
}

// システム DPI スケール (= プライマリモニタの DPI / 96)。Cubase の pre-attach view size は
// SYSTEM DPI の physical px で渡されるため、現在モニタの physical へ変換する基準として使う。
static float GetSystemDpiScale()
{
  using GetDpiForSystemFn = UINT(WINAPI*)();
  static GetDpiForSystemFn p = []() -> GetDpiForSystemFn {
    HMODULE h = GetModuleHandleW(L"user32.dll");
    return h ? reinterpret_cast<GetDpiForSystemFn>(GetProcAddress(h, "GetDpiForSystem")) : nullptr;
  }();
  if (!p) return 1.0f;
  const UINT dpi = p();
  return dpi == 0 ? 1.0f : static_cast<float>(dpi) / 96.0f;
}

#ifdef AAX_API
// Pro Tools Windows は AAX plug-in view を実質 100% DPI の座標系で扱う。WebView2 側
// だけが monitor DPI を拾うと Chromium が拡大描画して見切れるので、起動前に
// --force-device-scale-factor=1 を環境変数で渡し DPR を 1 に固定する。
static void InstallAAXWebViewScaleOverride()
{
  const char* kEnvName = "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS";
  const char* kArg = "--force-device-scale-factor=1";
  char* existing = nullptr;
  size_t len = 0;
  if (_dupenv_s(&existing, &len, kEnvName) == 0 && existing != nullptr)
  {
    std::string combined(existing);
    free(existing);
    if (combined.find("--force-device-scale-factor") == std::string::npos)
    {
      if (!combined.empty()) combined += ' ';
      combined += kArg;
      _putenv_s(kEnvName, combined.c_str());
    }
    return;
  }
  _putenv_s(kEnvName, kArg);
}
#endif
#endif // OS_WIN

#ifdef AAX_API
void MixCompare::GetCustomAAXParamID(int paramIdx, WDL_String& out) const
{
  // 旧 JUCE 版 APVTS の string ID に揃える (順序は EParams 定義順)。Pro Tools セッションは
  // (param_id_string -> normalized value) のマップで保存されているため、これらの ID 名が
  // iPlug2 側 AAX param 登録と一致しないと値が引き継がれない。
  // 旧 plugin/src/ParameterIDs.h (commit a9989ea) で実値を確認した文字列。
  static constexpr const char* kJuceParamIDs[] = {
    "HOST_GAIN",                   // kHostGain
    "PLAYLIST_GAIN",               // kPlaylistGain
    "LPF_FREQ",                    // kLpfFreq
    "LPF_ENABLED",                 // kLpfEnabled
    "HOST_SYNC_CAPABLE",           // kHostSyncCapable
    "HOST_SYNC_ENABLED",           // kHostSyncEnabled
    "SOURCE_SELECT",               // kSourceSelect
    "METERING_MODE",               // kMeteringMode
    "TRANSPORT_PLAYING",           // kTransportPlaying
    "TRANSPORT_LOOP_ENABLED",      // kTransportLoopEnabled
    "TRANSPORT_SEEK_NORM",         // kTransportSeekNorm
    "LOOP_START_NORM",             // kLoopStartNorm
    "LOOP_END_NORM",               // kLoopEndNorm
    "PLAYLIST_CURRENT_INDEX_NORM", // kPlaylistCurrentIndexNorm
  };
  if (paramIdx >= 0 && paramIdx < static_cast<int>(sizeof(kJuceParamIDs) / sizeof(kJuceParamIDs[0])))
    out.Set(kJuceParamIDs[paramIdx]);
}
#endif

#if defined(OS_WIN) && defined(AAX_API)
void MixCompare::SetAAXWebViewBoundsPhysical(int widthPx, int heightPx)
{
  HWND hwnd = static_cast<HWND>(mNativeParent);
  const float scale = GetWindowDpiScale(hwnd);
  const float invScale = scale > 0.0f ? 1.0f / scale : 1.0f;
  // iPlug2 の SetWebViewBounds は内部で GetScaleForHWND(parent) を掛ける。AAX/PT は
  // view を 100% DPI physical px として扱うので、渡す値を先に割って最終 bounds を
  // widthPx/heightPx (physical) に戻す。
  SetWebViewBounds(0, 0, static_cast<float>(widthPx) * invScale,
                   static_cast<float>(heightPx) * invScale);
}
#endif

void* MixCompare::OpenWindow(void* pParent)
{
#ifdef OS_WIN
  mNativeParent = pParent;
#ifdef AAX_API
  // base が WebView2 environment を生成する前に DPR=1 を仕込む。
  InstallAAXWebViewScaleOverride();
#endif
#endif
  void* result = WebViewEditorDelegate::OpenWindow(pParent);
#ifdef OS_WIN
  // iPlug2 の OpenWebView は bounds を初期化しないため、明示的に設定しないと
  // WebView2 が 0x0 (invisible) のままになる。
#ifdef AAX_API
  // AAX wrapper は onSize 相当を呼ばないので OpenWindow で bounds を確定させる。
  // PT は 100% DPI physical px なので scale 相殺版で渡す。
  SetAAXWebViewBoundsPhysical(GetEditorWidth(), GetEditorHeight());
#else
  // logical px を渡す (SetWebViewBounds が内部で DPI 乗算して physical に変換)。
  SetWebViewBounds(0, 0, static_cast<float>(GetEditorWidth()),
                   static_cast<float>(GetEditorHeight()));
#endif
#ifdef APP_API
  // Standalone: iPlug2 の ClientResize が誤サイズ(logical を physical 扱い)を撃つので、
  // 非 1x DPI のときは OnParentWindowResize で target 物理サイズに到達するまで補正する。
  if (HWND hwnd = static_cast<HWND>(pParent))
    mDpiInitPending = (GetWindowDpiScale(hwnd) != 1.0f);
  else
    mDpiInitPending = false;
#endif
#endif
  return result;
}

void MixCompare::CloseWindow()
{
  WebViewEditorDelegate::CloseWindow();
#ifdef OS_WIN
  mNativeParent = nullptr;
  mDpiInitPending = false;
  mInitialSizeCorrectionDone = false;  // reopen 時に再補正できるよう戻す
  mVST3PreAttachW = 0;
  mVST3PreAttachH = 0;
#endif
}

void MixCompare::OnParentWindowResize(int width, int height)
{
#ifdef OS_WIN
  { char b[160]; std::snprintf(b,sizeof(b),"OnParentWindowResize: arg=%dx%d nativeParent=%p\n",width,height,mNativeParent); mcStateLog(b); }
#endif
#if defined(OS_WIN) && defined(VST3_API)
  // pre-attach (hwnd=NULL) で Cubase が saved view size を渡してくる。これを記憶して
  // attach 時に user の保存サイズへ復元する根拠とする。
  if (mNativeParent == nullptr && width > 0 && height > 0)
  {
    mVST3PreAttachW = width;
    mVST3PreAttachH = height;
  }
#endif
#if defined(OS_WIN) && defined(AAX_API)
  // AAX wrapper は通常 onSize 相当を呼ばないが、呼ばれた場合も base に渡すと iPlug2
  // 内部で DPI 倍の bounds になる。PT の view size と同じ 100% DPI physical px に揃える。
  SetAAXWebViewBoundsPhysical(width, height);
  EditorResizeFromUI(width, height, false);
  return;
#endif
#if defined(OS_WIN) && !defined(AAX_API)
  HWND hwnd = static_cast<HWND>(mNativeParent);
  if (hwnd)
  {
    const float scale = GetWindowDpiScale(hwnd);
    RECT rcClient{};
    GetClientRect(hwnd, &rcClient);
    const int physW = rcClient.right;
    const int physH = rcClient.bottom;
#ifdef OS_WIN
    { char b[200]; std::snprintf(b,sizeof(b)," .scale=%.2f client=%dx%d phys, default=%dx%d logical, initCorrectionDone=%d\n",scale,physW,physH,GetEditorWidth(),GetEditorHeight(),mInitialSizeCorrectionDone?1:0); mcStateLog(b); }
#endif
    if (physW > 0 && physH > 0)
    {
#ifdef VST3_API
      // 新規インスタンス初回 attach 補正:
      //   (1) pre-attach (hwnd=NULL) で Cubase が saved view size を渡しておきながら、
      //       attach 時に min*scale 等で開くことがある (実機ログで確認済) ので、pre-attach
      //       値を覚えておいて ユーザー保存サイズへ復元する。これは scale 値に関係なく
      //       動かす (例: マルチモニタでセカンダリ 1.0x に open する場合も saved size を
      //       尊重する必要がある)。
      //   (2) pre-attach が無い場合のみ、新規インスタンスで Cubase が default(logical) を
      //       physical 扱いして縮めるケース (defaultAsPhys / shrunkBelowMin) を補正する。
      //       これは hiDPI (scale != 1) のときのみ意味がある。kMinWidth==kDefaultWidth な
      //       MixCompare では minAsPhys はユーザーの min 選択と区別不能なので使わない。
      if (!mInitialSizeCorrectionDone)
      {
        mInitialSizeCorrectionDone = true;
        using namespace editor_size;

        // (1) pre-attach 経路: Cubase が attach 前に渡した saved view size を優先する。
        // pre-attach 値は **SYSTEM DPI (= プライマリモニタ) での physical px** で渡される
        // (実機ログとの突き合わせで確定)。logical = pre / system_scale、target_phys =
        // logical * current_monitor_scale で現在モニタの physical 値に変換する。
        // 例: プライマリ 1.5x + セカンダリ 1.0x で saved 744 phys (= 496 logical) のとき、
        // 現在モニタが secondary なら 496 phys、primary なら 744 phys になる。
        if (mVST3PreAttachW > 0 && mVST3PreAttachH > 0)
        {
          const float sysScale = GetSystemDpiScale();
          const float ratio = (sysScale > 0.0f) ? (scale / sysScale) : 1.0f;
          const int tgtPhysW = static_cast<int>(mVST3PreAttachW * ratio + 0.5f);
          const int tgtPhysH = static_cast<int>(mVST3PreAttachH * ratio + 0.5f);
          if (std::abs(tgtPhysW - physW) > 4 || std::abs(tgtPhysH - physH) > 4)
          {
            const int origW = mVST3PreAttachW, origH = mVST3PreAttachH;
            mVST3PreAttachW = 0; mVST3PreAttachH = 0;
#ifdef OS_WIN
            { char b[220]; std::snprintf(b,sizeof(b)," .VST3 restore preAttach (sysPhys=%dx%d sysScale=%.2f curScale=%.2f) -> phys=%dx%d\n",origW,origH,sysScale,scale,tgtPhysW,tgtPhysH); mcStateLog(b); }
#endif
            // iPlug2::EditorResize は viewWidth == GetEditorWidth() だと resizeView を
            // 呼ばないため、現在の物理サイズに先に内部 editor size を合わせて mismatch
            // を作り、host に必ず resizeView が飛ぶようにする。
            SetEditorSize(physW, physH);
            EditorResizeFromUI(tgtPhysW, tgtPhysH, true);
            return;
          }
        }

        // (2) pre-attach 無し (新規インスタンス):
        //   Cubase が default サイズを physical 単位で渡してくる解釈バリエーション:
        //     a. raw default (kDefaultWidth)             - 単一 hiDPI 古典ケース
        //     b. default × system_scale                  - マルチモニタで Cubase が primary
        //                                                  DPI で開いた状態 (現在モニタが
        //                                                  secondary なら大きく見える)
        //     c. logical < min                           - host が default を physical 解釈
        //                                                  → min clamp された過渡
        //   いずれも default × current_scale に補正 (no-op になる場合は skip)。
        const float sysScale = GetSystemDpiScale();
        const int targetPhysW = static_cast<int>(kDefaultWidth * scale + 0.5f);
        const int targetPhysH = static_cast<int>(kDefaultHeight * scale + 0.5f);
        const int defaultAtSysW = static_cast<int>(kDefaultWidth * sysScale + 0.5f);
        const int defaultAtSysH = static_cast<int>(kDefaultHeight * sysScale + 0.5f);
        const double logicalW = physW / scale;
        const double logicalH = physH / scale;
        const bool defaultAsPhys = std::abs(physW - kDefaultWidth) <= 4 &&
                                   std::abs(physH - kDefaultHeight) <= 4;
        const bool defaultAsSysPhys = std::abs(physW - defaultAtSysW) <= 4 &&
                                       std::abs(physH - defaultAtSysH) <= 4;
        const bool shrunkBelowMin = logicalW < kMinWidth - 1.0 || logicalH < kMinHeight - 1.0;
        const bool sizeAlreadyMatches = std::abs(physW - targetPhysW) <= 4 &&
                                         std::abs(physH - targetPhysH) <= 4;
        const bool needCorrect = (defaultAsPhys || defaultAsSysPhys || shrunkBelowMin)
                                  && !sizeAlreadyMatches;
#ifdef OS_WIN
        { char b[240]; std::snprintf(b,sizeof(b)," .VST3 init-correct (no preAttach): defAsPhys=%d defAsSysPhys=%d shrunk=%d sysScale=%.2f curScale=%.2f -> targetPhys=%dx%d action=%s\n",defaultAsPhys?1:0,defaultAsSysPhys?1:0,shrunkBelowMin?1:0,sysScale,scale,targetPhysW,targetPhysH,needCorrect?"RESIZE":"keep"); mcStateLog(b); }
#endif
        if (needCorrect)
        {
          // iPlug2::EditorResize の no-op 回避 (preAttach 経路と同じ理由)。
          SetEditorSize(physW, physH);
          EditorResizeFromUI(targetPhysW, targetPhysH, true);
          return;  // host が onSize を再発火 → 次回 bounds 設定
        }
      }
#endif
#ifdef APP_API
      const int targetPhysW = static_cast<int>(GetEditorWidth() * scale);
      const int targetPhysH = static_cast<int>(GetEditorHeight() * scale);
      if (mDpiInitPending && scale != 1.0f)
      {
        if (width == targetPhysW && height == targetPhysH)
        {
          mDpiInitPending = false;  // target 到達。以降のユーザドラッグは尊重する
        }
        else
        {
          RECT rcWindow{};
          GetWindowRect(hwnd, &rcWindow);
          const int ncW = (rcWindow.right - rcWindow.left) - rcClient.right;
          const int ncH = (rcWindow.bottom - rcWindow.top) - rcClient.bottom;
          SetWindowPos(hwnd, nullptr, 0, 0, targetPhysW + ncW, targetPhysH + ncH,
                       SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
          return;  // SetWindowPos が WM_SIZE を再発火する
        }
      }
#endif
      // 確定した実 client 物理 px を scale で割って logical px を bounds に渡す
      // (SetWebViewBounds が内部で scale 倍して physical に戻す)。引数 width/height は
      // host により logical/physical が揺れるため信用せず GetClientRect を信頼する。
      const int logicalW = static_cast<int>(physW / scale);
      const int logicalH = static_cast<int>(physH / scale);
#ifdef OS_WIN
      { char b[140]; std::snprintf(b,sizeof(b)," .SetWebViewBounds logical=%dx%d\n",logicalW,logicalH); mcStateLog(b); }
#endif
      SetWebViewBounds(0, 0, static_cast<float>(logicalW), static_cast<float>(logicalH));
      return;
    }
  }
#endif
  WebViewEditorDelegate::OnParentWindowResize(width, height);
}

// ---------------------------------------------------------------------------
// State serialization
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// JUCE 版 (copyXmlToBinary) で保存された状態の移行ヘルパ
// ---------------------------------------------------------------------------
namespace
{
// 単一 Unicode コードポイントを UTF-8 バイト列に変換して追記する。
void AppendUtf8(std::string& out, uint32_t cp)
{
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x110000) {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// XML エンティティをアンエスケープ。名前付き (&amp; 等) + 数値文字参照 (&#NNNN; / &#xHHHH;)。
// JUCE は非 ASCII (日本語等) を &#NNNN; で書き出すので、これを処理しないと文字化けする。
std::string XmlUnescape(const std::string& s)
{
  std::string r;
  r.reserve(s.size());
  for (size_t i = 0; i < s.size();)
  {
    if (s[i] == '&')
    {
      if (s.compare(i, 5, "&amp;") == 0)  { r += '&';  i += 5; continue; }
      if (s.compare(i, 4, "&lt;") == 0)   { r += '<';  i += 4; continue; }
      if (s.compare(i, 4, "&gt;") == 0)   { r += '>';  i += 4; continue; }
      if (s.compare(i, 6, "&quot;") == 0) { r += '"';  i += 6; continue; }
      if (s.compare(i, 6, "&apos;") == 0) { r += '\''; i += 6; continue; }
      if (i + 2 < s.size() && s[i + 1] == '#')
      {
        const size_t semi = s.find(';', i + 2);
        if (semi != std::string::npos && semi - (i + 2) <= 8)
        {
          uint32_t cp = 0; bool ok = false;
          if (s[i + 2] == 'x' || s[i + 2] == 'X')
          {
            for (size_t k = i + 3; k < semi; ++k) {
              const char c = s[k];
              int d = (c >= '0' && c <= '9') ? c - '0'
                    : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                    : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
              if (d < 0) { ok = false; break; }
              cp = (cp << 4) | static_cast<uint32_t>(d); ok = true;
            }
          }
          else
          {
            for (size_t k = i + 2; k < semi; ++k) {
              const char c = s[k];
              if (c < '0' || c > '9') { ok = false; break; }
              cp = cp * 10u + static_cast<uint32_t>(c - '0'); ok = true;
            }
          }
          if (ok && cp > 0 && cp < 0x110000)
          {
            AppendUtf8(r, cp);
            i = semi + 1;
            continue;
          }
        }
      }
    }
    r += s[i++];
  }
  return r;
}

// 開きタグ1個分の文字列 elem から attr="..." (or '...') を取り出す。属性名は直前が
// 空白/タグ境界・直後が '=' の語境界でのみマッチさせ、値内の偶然一致を避ける。
bool XmlAttr(const std::string& elem, const std::string& name, std::string& out)
{
  size_t pos = 0;
  while ((pos = elem.find(name, pos)) != std::string::npos)
  {
    const bool boundaryBefore = (pos == 0) || elem[pos - 1] == ' ' || elem[pos - 1] == '\t'
                              || elem[pos - 1] == '\n' || elem[pos - 1] == '\r' || elem[pos - 1] == '<';
    size_t q = pos + name.size();
    while (q < elem.size() && (elem[q] == ' ' || elem[q] == '\t')) ++q;
    if (boundaryBefore && q < elem.size() && elem[q] == '=')
    {
      ++q;
      while (q < elem.size() && (elem[q] == ' ' || elem[q] == '\t')) ++q;
      if (q < elem.size() && (elem[q] == '"' || elem[q] == '\''))
      {
        const char quote = elem[q++];
        const size_t end = elem.find(quote, q);
        if (end != std::string::npos) { out = XmlUnescape(elem.substr(q, end - q)); return true; }
      }
    }
    pos += name.size();
  }
  return false;
}

// JUCE APVTS の param id 文字列 → iPlug2 EParams index。保存対象外/未知は -1。
int JuceParamIdToIndex(const std::string& id)
{
  if (id == "HOST_GAIN")              return kHostGain;
  if (id == "PLAYLIST_GAIN")          return kPlaylistGain;
  if (id == "LPF_FREQ")               return kLpfFreq;
  if (id == "LPF_ENABLED")            return kLpfEnabled;
  if (id == "HOST_SYNC_ENABLED")      return kHostSyncEnabled;
  if (id == "SOURCE_SELECT")          return kSourceSelect;
  if (id == "METERING_MODE")          return kMeteringMode;
  if (id == "TRANSPORT_LOOP_ENABLED") return kTransportLoopEnabled;
  if (id == "LOOP_START_NORM")        return kLoopStartNorm;
  if (id == "LOOP_END_NORM")          return kLoopEndNorm;
  return -1;  // TRANSPORT_PLAYING/SEEK_NORM/CURRENT_INDEX_NORM/SYNC_CAPABLE は JUCE 側も非保存
}

double ParseDoubleC(const std::string& s)
{
  try { return std::stod(s); } catch (...) { return 0.0; }
}
} // namespace

bool MixCompare::RestoreFromJuceState(const uint8_t* data, int size)
{
  if (data == nullptr || size < 8) return false;
  auto rdLE32 = [](const uint8_t* p) -> uint32_t {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
  };
  // JUCE AudioProcessor::copyXmlToBinary の magic (0x21324356, LE) でなければ JUCE 状態でない。
  if (rdLE32(data) != 0x21324356u) return false;
  const uint32_t rawLen = rdLE32(data + 4);
  if (rawLen == 0 || rawLen > 64u * 1024u * 1024u) return true; // 異常。検出済なので fallthrough しない

  // JUCE 7+ の copyXmlToBinary はマジック+長さ+**生 UTF-8 XML** を書く (圧縮なし、長さは
  // 末尾 NUL 含む sizeInBytes)。古い JUCE は GZIP 圧縮していたので、両方に対応する:
  //   オフセット 8 が '<' なら plain XML、それ以外なら zlib uncompress を試す。
  const uint8_t* xmlBytes = data + 8;
  const size_t avail = static_cast<size_t>(size - 8);
  std::string xml;
  if (rawLen <= avail && rawLen > 0 && xmlBytes[0] == '<')
  {
    // 生 XML 経路: 末尾 NUL を含む可能性があるので除去。
    size_t xlen = rawLen;
    while (xlen > 0 && xmlBytes[xlen - 1] == '\0') --xlen;
    xml.assign(reinterpret_cast<const char*>(xmlBytes), xlen);
#ifdef OS_WIN
    { char b[128]; std::snprintf(b,sizeof(b),"RestoreFromJuceState: plain XML, rawLen=%u xlen=%zu\n",rawLen,xlen); mcStateLog(b); }
#endif
  }
  else
  {
    // フォールバック: 古い JUCE 互換の zlib 圧縮。
    std::vector<unsigned char> xmlBuf(rawLen);
    uLongf destLen = rawLen;
    const int zr = uncompress(xmlBuf.data(), &destLen, xmlBytes, static_cast<uLong>(avail));
#ifdef OS_WIN
    { char b[128]; std::snprintf(b,sizeof(b),"RestoreFromJuceState: zlib path rawLen=%u rc=%d destLen=%lu\n",rawLen,zr,(unsigned long)destLen); mcStateLog(b); }
#endif
    if (zr != Z_OK) return true;
    xml.assign(reinterpret_cast<const char*>(xmlBuf.data()), static_cast<size_t>(destLen));
  }

  // --- <PARAM id=".." value=".."/> を IParam (実値) に適用 ---
  for (size_t pos = 0; (pos = xml.find("<PARAM", pos)) != std::string::npos;)
  {
    const size_t end = xml.find('>', pos);
    if (end == std::string::npos) break;
    const std::string elem = xml.substr(pos, end - pos + 1);
    pos = end + 1;
    std::string id, val;
    if (!XmlAttr(elem, "id", id) || !XmlAttr(elem, "value", val)) continue;
    const int idx = JuceParamIdToIndex(id);
    if (idx < 0) continue;
    // APVTS の value は実値 (dB/Hz/enum index/bool)。iPlug2 IParam も実値範囲なので直接 Set。
    GetParam(idx)->Set(ParseDoubleC(val));
    OnParamChange(idx);  // engine 係数へ反映 (UI スレッドから呼ぶので安全)
  }

  // --- <Playlist currentIndex="N"><Item id/path/name/></Playlist> を復元 JSON 化 ---
  json items = json::array();
  int currentIndex = -1;
  const size_t plPos = xml.find("<Playlist");
  if (plPos != std::string::npos)
  {
    const size_t plOpenEnd = xml.find('>', plPos);
    if (plOpenEnd != std::string::npos)
    {
      std::string ci;
      if (XmlAttr(xml.substr(plPos, plOpenEnd - plPos + 1), "currentIndex", ci))
        currentIndex = static_cast<int>(ParseDoubleC(ci));
    }
    const size_t plEnd = xml.find("</Playlist>", plPos);
    const size_t scanEnd = (plEnd == std::string::npos) ? xml.size() : plEnd;
    for (size_t ip = plPos; (ip = xml.find("<Item", ip)) != std::string::npos && ip < scanEnd;)
    {
      const size_t ie = xml.find('>', ip);
      if (ie == std::string::npos) break;
      const std::string elem = xml.substr(ip, ie - ip + 1);
      ip = ie + 1;
      std::string id, path, name;
      XmlAttr(elem, "id", id);
      XmlAttr(elem, "path", path);
      XmlAttr(elem, "name", name);
      if (path.empty()) continue;
      items.push_back({{"id", id}, {"path", path}, {"name", name}});
    }
  }

  const json j = {{"playlist", items}, {"currentIndex", currentIndex}};
  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    mStateJson = j.dump();
  }
  mPendingStateRestore.store(true, std::memory_order_relaxed);  // OnIdle で RestorePlaylistFromState
#ifdef OS_WIN
  { char b[160]; std::snprintf(b,sizeof(b),"RestoreFromJuceState OK: items=%zu currentIndex=%d xmlSize=%zu\n",items.size(),currentIndex,xml.size()); mcStateLog(b); }
#endif
  return true;
}

bool MixCompare::SerializeState(IByteChunk& chunk) const
{
  if (!SerializeParams(chunk)) return false;
  // 非パラメータ状態 (プレイリスト等) を JSON で同梱 (Phase 1 は枠のみ)。
  std::string snapshot;
  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    snapshot = mStateJson;
  }
  chunk.PutStr(snapshot.c_str());
  return true;
}

int MixCompare::UnserializeState(const IByteChunk& chunk, int startPos)
{
  // 一時診断ログ (Cubase 実機での chunk 形態を確認するため)。問題解決後に削除。
#ifdef OS_WIN
  {
    const int sz = chunk.Size();
    std::string line = "=== UnserializeState ===\nchunkSize=" + std::to_string(sz)
                     + " startPos=" + std::to_string(startPos) + "\nfirst64=";
    const int n = std::min(64, sz);
    char hx[3];
    for (int i = 0; i < n; ++i) { std::snprintf(hx, sizeof(hx), "%02x", chunk.GetData()[i]); line += hx; }
    line += "\nascii=";
    for (int i = 0; i < n; ++i) {
      const unsigned char c = chunk.GetData()[i];
      line += (c >= 32 && c < 127) ? static_cast<char>(c) : '.';
    }
    if (sz >= 8) {
      const uint8_t* p = chunk.GetData();
      const uint32_t m = (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
      const uint32_t rl = (uint32_t)p[4] | ((uint32_t)p[5]<<8) | ((uint32_t)p[6]<<16) | ((uint32_t)p[7]<<24);
      char buf[96]; std::snprintf(buf, sizeof(buf), "\nmagic_LE32=%08x (expect 21324356) rawLen=%u\n", m, rl);
      line += buf;
    }
    mcStateLog(line);
  }
#endif

  // 旧 JUCE 版 (copyXmlToBinary) で保存されたプロジェクトからの移行:
  // iPlug2 VST3 の SetState はストリーム全体を chunk に読み込んで UnserializeState(chunk,0)
  // を呼ぶため、chunk 先頭が JUCE の magic なら JUCE 状態として復元する。これをしないと
  // UnserializeParams が JUCE のバイト列を iPlug2 param として誤読してゴミ値になる。
  if (startPos == 0 && chunk.Size() >= 8 &&
      RestoreFromJuceState(chunk.GetData(), chunk.Size()))
  {
#ifdef OS_WIN
    { char b[96]; std::snprintf(b,sizeof(b),"JUCE path taken; returning chunk.Size()=%d\n\n",chunk.Size()); mcStateLog(b); }
#endif
    // 全消費扱い。SetState 後続の bypass read は失敗するが (JUCE 形式に bypass trailer は
    // 無い)、param/playlist は適用済みで bypass も触らない。
    return chunk.Size();
  }

  startPos = UnserializeParams(chunk, startPos);
  WDL_String jsonStr;
  const int next = chunk.GetStr(jsonStr, startPos);
  if (next > startPos)
  {
    {
      std::lock_guard<std::mutex> lock(mStateMutex);
      mStateJson.assign(jsonStr.Get());
    }
    mPendingStateRestore.store(true, std::memory_order_relaxed);
    startPos = next;
  }
#ifdef OS_WIN
  { char b[96]; std::snprintf(b,sizeof(b),"normal iPlug2 path; returning startPos=%d\n\n",startPos); mcStateLog(b); }
#endif
  return startPos;
}

// ---------------------------------------------------------------------------
// WebView source selection
// ---------------------------------------------------------------------------
void MixCompare::LoadIndexHtmlForCurrentBuild()
{
#if defined(_DEBUG) && !defined(OS_IOS)
  // Debug: Vite dev server。事前に `cd webui && npm run dev` を起動しておく。
  //   MIXCOMPARE_DEV_USE_DISK=1 で iPlug2 既定の disk 経路にフォールバック。
  //   MIXCOMPARE_DEV_URL=... で URL 上書き。
#ifdef OS_WIN
  char* useDiskRaw = nullptr; size_t envLen = 0;
  _dupenv_s(&useDiskRaw, &envLen, "MIXCOMPARE_DEV_USE_DISK");
  const bool useDisk = (useDiskRaw != nullptr && std::strcmp(useDiskRaw, "0") != 0);
  if (useDiskRaw) free(useDiskRaw);
  char* devUrlRaw = nullptr;
  _dupenv_s(&devUrlRaw, &envLen, "MIXCOMPARE_DEV_URL");
  const std::string devUrl = devUrlRaw ? devUrlRaw : "http://127.0.0.1:5173/";
  if (devUrlRaw) free(devUrlRaw);
#else
  const char* useDiskEnv = std::getenv("MIXCOMPARE_DEV_USE_DISK");
  const bool useDisk = (useDiskEnv != nullptr && std::strcmp(useDiskEnv, "0") != 0);
  const char* devUrlEnv = std::getenv("MIXCOMPARE_DEV_URL");
  const std::string devUrl = devUrlEnv ? devUrlEnv : "http://127.0.0.1:5173/";
#endif
  if (useDisk)
  {
    LoadIndexHtml(__FILE__, GetBundleID());
    return;
  }
  LoadURL(devUrl.c_str());
#elif defined(OS_WIN)
  // Release on Windows: vite-plugin-singlefile が出力した単一 index.html を
  // main.rc の RCDATA (IDR_INDEX_HTML) として EXE/DLL に埋め込む。
  //
  // 重要: WebView2 の NavigateToString (= iPlug2 LoadHTML) は ~2MB の上限があり、
  // それを超える文字列は黙って拒否され WebView が真っ白になる。MixCompare の UI は
  // MUI を含み単一 HTML が 2.5MB 前後になるため上限を超える。そこで RCDATA を
  // ユーザー TEMP に展開し、LoadFile() 経由 (SetVirtualHostNameToFolderMapping →
  // https://iplug.example/index.html へ navigate、サイズ無制限) でロードする。
  // 単一ファイル構成なので index.html 1 つで完結する。
  HINSTANCE moduleHandle = gHINSTANCE ? gHINSTANCE : GetModuleHandleW(nullptr);
  if (moduleHandle)
  {
    HRSRC hRes = FindResourceW(moduleHandle, MAKEINTRESOURCEW(IDR_INDEX_HTML), MAKEINTRESOURCEW(10));
    if (!hRes)
      hRes = FindResourceW(moduleHandle, L"IDR_INDEX_HTML", MAKEINTRESOURCEW(10));
    if (hRes)
    {
      HGLOBAL hGlob = LoadResource(moduleHandle, hRes);
      DWORD size = SizeofResource(moduleHandle, hRes);
      void* data = LockResource(hGlob);
      if (hGlob && data && size > 0)
      {
        // %TEMP%\MixCompare\index.html へ展開 (RCDATA はゼロ終端されないので size バイトをそのまま書く)。
        wchar_t tempDir[MAX_PATH];
        const DWORD tlen = GetTempPathW(MAX_PATH, tempDir);
        if (tlen > 0 && tlen < MAX_PATH)
        {
          const std::wstring webDir = std::wstring(tempDir) + L"MixCompare";
          CreateDirectoryW(webDir.c_str(), nullptr); // 既存でも可 (ERROR_ALREADY_EXISTS は無視)
          const std::wstring htmlPathW = webDir + L"\\index.html";
          HANDLE hFile = CreateFileW(htmlPathW.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
          if (hFile != INVALID_HANDLE_VALUE)
          {
            DWORD written = 0;
            const BOOL ok = WriteFile(hFile, data, size, &written, nullptr);
            CloseHandle(hFile);
            if (ok && written == size)
            {
              const int u8len = WideCharToMultiByte(CP_UTF8, 0, htmlPathW.c_str(), -1, nullptr, 0, nullptr, nullptr);
              if (u8len > 0)
              {
                std::string htmlPathU8(static_cast<size_t>(u8len), '\0');
                WideCharToMultiByte(CP_UTF8, 0, htmlPathW.c_str(), -1, htmlPathU8.data(), u8len, nullptr, nullptr);
                htmlPathU8.resize(static_cast<size_t>(u8len) - 1); // 終端 NUL を除去
                LoadFile(htmlPathU8.c_str(), GetBundleID());
                return;
              }
            }
          }
        }
        // フォールバック: TEMP 展開に失敗した場合のみ NavigateToString (2MB 未満なら表示される)。
        std::string html(static_cast<const char*>(data), size);
        LoadHTML(html.c_str());
        return;
      }
    }
  }
  LoadIndexHtml(__FILE__, GetBundleID());
#else
  // Release on macOS: bundle 経由の LoadIndexHtml がそのまま動く。
  LoadIndexHtml(__FILE__, GetBundleID());
#endif
}
