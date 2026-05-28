// MixCompare — iPlug2 plugin identity / build configuration.
// JUCE 版 (PRODUCT_NAME "MixCompare" / COMPANY "Jun Murakami" / BUNDLE_ID
// com.junmurakami.mixcompare / PLUGIN_CODE Mx03 / MANUFACTURER Jmbc) からの移植。
// 雛形は Synth80/plugin/config.h。

#define PLUG_NAME "MixCompare"
#define PLUG_MFR "Jun Murakami"
#define PLUG_VERSION_HEX 0x00030004
#define PLUG_VERSION_STR "3.0.4"
#define PLUG_UNIQUE_ID 'Mx03'
#define PLUG_MFR_ID 'Jmbc'
#define PLUG_URL_STR "https://junmurakami.com/plugins/mixcompare"
#define PLUG_EMAIL_STR "contact@bucketrelay.com"
#define PLUG_COPYRIGHT_STR "Copyright 2026 Jun Murakami"
#define PLUG_CLASS_NAME MixCompare

#define BUNDLE_NAME "MixCompare"
#define BUNDLE_MFR "JunMurakami"
#define BUNDLE_DOMAIN "com"

#define SHARED_RESOURCES_SUBPATH "MixCompare"

// ステレオ入力 (= DAW の Host 信号) → ステレオ出力 (= 選択ソース)。エフェクト。
#define PLUG_CHANNEL_IO "2-2"

#define PLUG_LATENCY 0
#define PLUG_TYPE 0
#define PLUG_DOES_MIDI_IN 0
#define PLUG_DOES_MIDI_OUT 0
#define PLUG_DOES_MPE 0
#define PLUG_DOES_STATE_CHUNKS 1
#define PLUG_HAS_UI 1

// WebUI の論理サイズ (CSS px)。JUCE 版 Editor の値を踏襲:
//   既定 392x650 / 最小 392x610 / 最大 2560x1440 / AAX 初期 450x650。
// ParameterIDs.h editor_size:: と webui の bridge 定数と必ず 3 箇所同期する。
#define PLUG_WIDTH 392
#define PLUG_HEIGHT 650
#define PLUG_FPS 60
#define PLUG_SHARED_RESOURCES 0
#define PLUG_HOST_RESIZE 1
#define PLUG_MIN_WIDTH 392
#define PLUG_MIN_HEIGHT 610
#define PLUG_MAX_WIDTH 2560
#define PLUG_MAX_HEIGHT 1440

#define AUV2_ENTRY MixCompare_Entry
#define AUV2_ENTRY_STR "MixCompare_Entry"
#define AUV2_FACTORY MixCompare_Factory
#define AUV2_VIEW_CLASS MixCompare_View
#define AUV2_VIEW_CLASS_STR "MixCompare_View"

// AAX Plug-in Type ID (= AAX_eProperty_PlugInID_Native).
// 旧 JUCE 版 Pro Tools プロジェクトとのバインド互換のため、JUCE 8 の AudioProcessor::
// getAAXPluginIDForMainBusConfig が生成する値と一致させる必要がある:
//   PluginID_Native     = 'jcaa'(0x6a636161) + ((aaxFormatIdxIn << 8) | aaxFormatIdxOut)
//   PluginID_AudioSuite = 'jyaa'(0x6a796161) + ((aaxFormatIdxIn << 8) | aaxFormatIdxOut)
// (PLUG_UNIQUE_ID / PLUG_MFR_ID / PluginCode は使わず、stem 配列 index のみ参照。
//  stereo=2 / mono=1。/ MixCompare はステレオ 2-2 効果。)
// ステレオ(2,2) → offset 0x202 → Native = 0x6a636363 = 'jccc' / AudioSuite = 'jycc'。
// 実 .ptx session ファイルにも `JmbcMx03jccc` triad が直接埋まっていることを確認済。
#define AAX_TYPE_IDS 'jccc'
#define AAX_TYPE_IDS_AUDIOSUITE 'jycc'
#define AAX_PLUG_MFR_STR "Jun Murakami"
#define AAX_PLUG_NAME_STR "MixCompare\nMxCp"
#define AAX_PLUG_CATEGORY_STR "SoundField"
#define AAX_DOES_AUDIOSUITE 0

#define VST3_SUBCATEGORY "Fx|Analyzer"

// VST3 クラス ID を JUCE 版と同一にする (互換性維持)。
// これを定義しないと iPlug2 は既定 {0xF2AEE70D, 0x00DE4F4E, ...} を使うため、
// JUCE 版を挿していたプロジェクトを開いても DAW (Cubase 等) が別プラグインと判定し、
// 設定が引き継がれない。JUCE は VST3 FUID を
//   processor : {0xABCDEF01, 0x9182FAEB, mfrCode, plugCode}
//   controller: {0xABCDEF01, 0x1234ABCD, mfrCode, plugCode}
// で生成する (末尾 2 word は PLUG_MFR_ID='Jmbc' + PLUG_UNIQUE_ID='Mx03' で既に一致)。
// 先頭 2 word は旧 JUCE ビルドの moduleinfo.json (CID=ABCDEF019182FAEB4A6D62634D783033 /
// ABCDEF011234ABCD4A6D62634D783033) から実読した値。iPlug2 は
// IPlug_include_in_plug_src.h の `#if !defined VST3_PROCESSOR_UID` で上書きを許可する。
#define VST3_PROCESSOR_UID  0xABCDEF01, 0x9182FAEB, PLUG_MFR_ID, PLUG_UNIQUE_ID
#define VST3_CONTROLLER_UID 0xABCDEF01, 0x1234ABCD, PLUG_MFR_ID, PLUG_UNIQUE_ID

#define APP_NUM_CHANNELS 2
#define APP_N_VECTOR_WAIT 0
#define APP_MULT 1
#define APP_COPY_AUV3 0
#define APP_SIGNAL_VECTOR_SIZE 64
