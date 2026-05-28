// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
//
// MixCompare — iPlug2 plugin entry (JUCE からの移植版)。
//
// Phase 1 scope:
//   - 14 IParam (APVTS と同順) を保持し、OnParamChange を AudioEngine に流す
//   - ProcessBlock で Host(=DAW入力) パスに gain / source blend / LPF / metering
//   - OnIdle で 24-float メータ snapshot を SAMFD で WebUI に送る (Web デモ互換)
//   - WebView を Debug=Vite dev server / Release=埋め込みリソースから読む
//   - OnMessage で window/open_url/metering_reset/system_action 等を処理
//
// Phase 2 以降: Playlist 再生 (IPlaybackSource + デコーダ) / トランスポート同期 /
// プレイリスト管理 / 状態永続化の本実装を段階的に追加する。
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "IPlug_include_in_plug_hdr.h"
#include "json.hpp"  // nlohmann::json (iPlug2::WebView の include パス上)
#include "ParameterIDs.h"
#include "core/AudioEngine.h"

using namespace iplug;

const int kNumPresets = 1;

class MixCompare final : public Plugin
{
public:
  explicit MixCompare(const InstanceInfo& info);
  ~MixCompare();

  // --- audio thread ---
  void ProcessBlock(sample** inputs, sample** outputs, int nFrames) override;
  void OnReset() override;
  void OnParamChange(int paramIdx) override;
  // UI スレッド呼び。host/automation/preset 由来の param 変更を SPVFD で WebUI に反映する
  // (UI 由来=kUI はエコー防止のため送らない)。
  void OnParamChangeUI(int paramIdx, EParamSource source = kUnknown) override;

  // --- UI thread ---
  void OnIdle() override;
  void OnWebContentLoaded() override;
  bool OnMessage(int msgTag, int ctrlTag, int dataSize, const void* pData) override;
  bool ConstrainEditorResize(int& w, int& h) const override;

  void* OpenWindow(void* pParent) override;
  void CloseWindow() override;
  void OnParentWindowResize(int width, int height) override;

#if defined(OS_WIN) && defined(AAX_API)
  // AAX/Pro Tools は view を 100% DPI physical px として扱う。iPlug2 の
  // SetWebViewBounds 内部の scale 乗算を相殺し、最終 bounds を physical px に揃える。
  void SetAAXWebViewBoundsPhysical(int widthPx, int heightPx);
#endif

#ifdef AAX_API
  // 旧 JUCE 版 (APVTS) で保存された Pro Tools セッションの param 値を引き継ぐため、
  // AAX param ID 文字列を JUCE と同一にする。iPlug2 既定の "1".."14" だと PT は param
  // を見つけられず値を破棄する。iPlug2 ローカルパッチ (IPlugAAX.h/.cpp) で有効化。
  void GetCustomAAXParamID(int paramIdx, WDL_String& out) const override;
#endif

  // --- state ---
  bool SerializeState(IByteChunk& chunk) const override;
  int  UnserializeState(const IByteChunk& chunk, int startPos) override;

private:
  void LoadIndexHtmlForCurrentBuild();
  void SendInitialStateToWeb();
  void SendTransportUpdate();
  void SendPlaylistUpdate();
  void SendTrackChange();
  void SendTransportPositionUpdate();
  void SendMeterSnapshot();

  // --- プレイリスト (UI スレッドのみが触る) ---
  struct PlaylistItem
  {
    std::string id;       // 安定 ID ("item-N")
    std::string path;     // UTF-8 ファイルパス
    std::string name;     // 表示名 (ファイル名)
    double durationSec = 0.0;
    bool loaded = false;    // デコード完了 (in-memory) or probe 完了 (streaming)
    bool exists = true;
    bool streaming = false; // true = 大ファイル。select 時に StreamingSource を構築
    std::shared_ptr<mc::TrackAudio> audio; // in-memory のデコード済み PCM (streaming 時は null)
  };

  void HandlePlaylistAction(const std::string& jsonText);
  void AppendPlaylistItem(const std::string& pathUtf8); // 1 ファイルを追加 + デコード投入 (イベント送出はしない)
  void ExportPlaylistM3U8(const std::string& pathUtf8) const;
  void ImportPlaylistM3U8(const std::string& pathUtf8);
  nlohmann::json BuildPlaylistItemsJson() const;
  void SelectTrackByIndex(int index);     // UI スレッド
  void UpdateLoopRangeFromParams();        // 現在トラック duration で loop 秒を再計算
  int  FindItemIndexById(const std::string& id) const;
  void EnqueueDecode(const std::string& id, const std::string& path, bool streaming);
  void DrainDecodeResults();               // OnIdle
  void StartDecodeThread();
  void StopDecodeThread();
  void RebuildStateJson();                 // mPlaylist → mStateJson (永続化用スナップショット)
  void RestorePlaylistFromState();         // UnserializeState 後に OnIdle で復元
  // JUCE 版 (copyXmlToBinary = magic+zlib XML) で保存された状態チャンクを検出して
  // IParam / プレイリストに復元する。検出して処理したら true (= iPlug2 形式の
  // UnserializeParams にフォールバックしない)。旧プロジェクト互換のため。
  bool RestoreFromJuceState(const uint8_t* data, int size);

  std::vector<PlaylistItem> mPlaylist;
  int mCurrentIndex = -1;
  uint32_t mPlaylistRevision = 0;
  uint32_t mTransportSeq = 0;
  int mNextItemId = 1;
  bool mInSelect = false;  // SelectTrackByIndex 再入ガード
  std::atomic<bool> mPendingStateRestore { false }; // UnserializeState → OnIdle 復元

  // --- バックグラウンドデコード (worker → UI を queue で受け渡し) ---
  struct DecodeJob { std::string id; std::string path; bool streaming = false; };
  struct DecodeResult { std::string id; std::shared_ptr<mc::TrackAudio> audio; double durationSec = 0.0; bool ok = false; bool streaming = false; };
  std::thread mDecodeThread;
  std::mutex mJobMutex;
  std::condition_variable mJobCv;
  std::deque<DecodeJob> mJobs;
  bool mDecodeStop = false;
  std::mutex mResultMutex;
  std::deque<DecodeResult> mResults;

  mc::AudioEngine mEngine;

  // ProcessBlock の double↔float 変換用スクラッチ (OnReset で確保、audio で alloc なし)。
  std::vector<float> mInL, mInR, mOutL, mOutR;

  // 非パラメータ状態 (プレイリスト path / currentIndex / loop) の JSON。SerializeState で
  // chunk に同梱、UnserializeState で復元する。UI/serialize 両スレッドから触るため mutex。
  mutable std::mutex mStateMutex;
  std::string mStateJson;

#ifdef OS_WIN
  void* mNativeParent{nullptr};
  // APP(standalone): iPlug2 の ClientResize が logical px を physical px として
  // 扱うため、非 1x DPI で初期ウィンドウが縮み WebView 内容が見切れる。target 物理
  // サイズに到達するまで OnParentWindowResize で補正する間 true。
  bool mDpiInitPending{false};
  // VST3: 新規インスタンス初回 attach のサイズ補正を一度だけ行うためのフラグ。
  bool mInitialSizeCorrectionDone{false};
  // VST3: pre-attach (hwnd=NULL) で Cubase が渡す saved view size を記憶しておき、attach
  // 時に host が min*scale 等で開いても保存サイズに復元する。
  int mVST3PreAttachW{0};
  int mVST3PreAttachH{0};
#endif
};
