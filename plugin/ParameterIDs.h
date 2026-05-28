// MixCompare — parameter / message tag definitions (iPlug2).
//
// EParams の並びは JUCE 版 APVTS `createParameterLayout()` の追加順と完全一致させる
// (= パラメータ index が一致)。WebUI 側は文字列 ID (HOST_GAIN 等) で参照するため、
// index ↔ 文字列 ID の対応表を webui/src/bridge/iplug-shim.ts と必ず同期する。
//
//   idx  EParams                       webui string ID
//   ---  ----------------------------  ---------------------------
//    0   kHostGain                     HOST_GAIN
//    1   kPlaylistGain                 PLAYLIST_GAIN
//    2   kLpfFreq                      LPF_FREQ
//    3   kLpfEnabled                   LPF_ENABLED
//    4   kHostSyncCapable              HOST_SYNC_CAPABLE
//    5   kHostSyncEnabled              HOST_SYNC_ENABLED
//    6   kSourceSelect                 SOURCE_SELECT
//    7   kMeteringMode                 METERING_MODE
//    8   kTransportPlaying             TRANSPORT_PLAYING
//    9   kTransportLoopEnabled         TRANSPORT_LOOP_ENABLED
//   10   kTransportSeekNorm            TRANSPORT_SEEK_NORM
//   11   kLoopStartNorm                LOOP_START_NORM
//   12   kLoopEndNorm                  LOOP_END_NORM
//   13   kPlaylistCurrentIndexNorm     PLAYLIST_CURRENT_INDEX_NORM

#pragma once

enum EParams
{
  kHostGain = 0,
  kPlaylistGain,
  kLpfFreq,
  kLpfEnabled,
  kHostSyncCapable,
  kHostSyncEnabled,
  kSourceSelect,
  kMeteringMode,
  kTransportPlaying,
  kTransportLoopEnabled,
  kTransportSeekNorm,
  kLoopStartNorm,
  kLoopEndNorm,
  kPlaylistCurrentIndexNorm,
  kNumParams
};

// SourceSelect / MeteringMode の enum 値 (DSP 側と共有)。
enum ESourceSelect { kSourceHost = 0, kSourcePlaylist = 1 };
enum EMeteringMode { kMeteringPeak = 0, kMeteringRMS = 1, kMeteringMomentary = 2 };

// SCVFD で送る非パラメータ・コントロール値があれば使う (現状メーターは SAMFD
// バイナリで送るので未使用。将来用に枠だけ用意)。
enum ECtrlTags
{
  kNumCtrlTags = 0
};

// 任意メッセージタグ。JS↔C++ で同じ整数値を共有する
// (webui/src/bridge/iplug-shim.ts の MSG_TAG と必ずペアで更新する)。
enum EArbitraryMsgTags
{
  // --- JS → C++ (コマンド系) ---
  kMsgPlaylistAction        = 100, // add/remove/reorder/select/clear/export/import
  kMsgWindowAction          = 101, // resizeTo / resizeBy
  kMsgSystemAction          = 102, // ready / forward_key_event
  kMsgOpenUrl               = 103, // OS デフォルトブラウザで URL を開く
  kMsgMeteringReset         = 104, // momentary / truepeak リセット
  kMsgRequestPlaylistUpdate = 105, // プレイリスト状態の再送要求

  // --- C++ → JS (イベント系) ---
  kMsgInitialParams         = 200, // 初期パラメータ一括 (ready 応答)
  kMsgMeterUpdate           = 201, // メーターバイナリ (float 配列)
  kMsgTransportUpdate       = 202, // 再生状態 / ループ / duration / index
  kMsgTransportPositionUpdate = 203, // 再生位置 (高頻度)
  kMsgPlaylistUpdate        = 204, // items[] / currentIndex / revision
  kMsgTrackChange           = 205, // トラック切替通知
  kMsgError                 = 206, // 非ブロッキングなエラー通知
  kMsgDpiScale              = 207  // Windows DPI スケール変更通知
};

// Editor サイズ定数。config.h (PLUG_*) および webui の bridge 定数と 3 箇所同期。
namespace editor_size
{
  static constexpr int kDefaultWidth  = 392;
  static constexpr int kDefaultHeight = 650;
  static constexpr int kMinWidth      = 392;
  static constexpr int kMinHeight     = 610;
  static constexpr int kMaxWidth      = 2560;
  static constexpr int kMaxHeight     = 1440;
  // Pro Tools / AAX 実機では初期幅を広めに取る (JUCE 版 450x650 を踏襲)。
  static constexpr int kAaxWidth      = 450;
  static constexpr int kAaxHeight     = 650;
}
