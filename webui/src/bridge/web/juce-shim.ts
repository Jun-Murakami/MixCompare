// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
/**
 * juce-framework-frontend-mirror の Web 互換 shim。
 * Vite エイリアスで本家モジュールの代わりにこれが解決される。
 *
 * 既存コンポーネントの import をそのまま動かすために、
 * 同じ関数名・同じ戻り値の形を維持する。
 */

import {
  WebSliderState,
  WebToggleState,
  WebComboBoxState,
} from './WebParamState';
import { webAudioEngine } from './WebAudioEngine';

// ---------- パラメータレジストリ ----------

const sliderStates = new Map<string, WebSliderState>();
const toggleStates = new Map<string, WebToggleState>();
const comboBoxStates = new Map<string, WebComboBoxState>();

/**
 * スライダーパラメータの定義。
 * toScaled / fromScaled で正規化値とスケール値を相互変換する。
 */
function registerDefaults() {
  // ゲイン (-120..0 dB) — 指数カーブ
  // プラグイン側と同じスケール: normalised 0.5 = -15dB (unity gain ポイント)
  const gainOpts = {
    defaultValue: 1.0, // 1.0 = 0dB (fromScaled(0) = 1.0)
    min: -120,
    max: 0,
    toScaled: (n: number) => {
      // 指数カーブ: k = ln(121) ≈ 4.796
      if (n <= 0) return -120;
      return -120 + 120 * Math.pow(n, 1 / 3.064);
    },
    fromScaled: (db: number) => {
      if (db <= -120) return 0;
      return Math.pow((db + 120) / 120, 3.064);
    },
  };

  sliderStates.set('HOST_GAIN', new WebSliderState(gainOpts));
  sliderStates.set('PLAYLIST_GAIN', new WebSliderState(gainOpts));

  // LPF 周波数 (20..20000 Hz) — 線形スケール
  // Controls.tsx が自前で対数カーブ変換し、線形正規化で setNormalisedValue に送るため
  sliderStates.set(
    'LPF_FREQ',
    new WebSliderState({
      defaultValue: 1.0, // 1.0 = 20kHz
      min: 20,
      max: 20000,
      toScaled: (n: number) => 20 + n * (20000 - 20),
      fromScaled: (hz: number) => (hz - 20) / (20000 - 20),
    })
  );

  // トランスポート
  sliderStates.set('TRANSPORT_SEEK_NORM', new WebSliderState({ defaultValue: 0 }));
  sliderStates.set('LOOP_START_NORM', new WebSliderState({ defaultValue: 0 }));
  sliderStates.set('LOOP_END_NORM', new WebSliderState({ defaultValue: 1 }));
  sliderStates.set(
    'PLAYLIST_CURRENT_INDEX_NORM',
    new WebSliderState({ defaultValue: 0 })
  );

  // トグル
  toggleStates.set('TRANSPORT_PLAYING', new WebToggleState(false));
  toggleStates.set('TRANSPORT_LOOP_ENABLED', new WebToggleState(false));
  toggleStates.set('LPF_ENABLED', new WebToggleState(false));
  toggleStates.set('HOST_SYNC_ENABLED', new WebToggleState(false));
  toggleStates.set('HOST_SYNC_CAPABLE', new WebToggleState(false));

  // コンボボックス
  comboBoxStates.set('SOURCE_SELECT', new WebComboBoxState(1, 2)); // Web 版はデフォルト Playlist
  comboBoxStates.set('METERING_MODE', new WebComboBoxState(0, 3)); // 0=Peak, 1=RMS, 2=Momentary

  // --- パラメータ変更 → WebAudioEngine 連携 ---

  // ゲイン
  sliderStates.get('HOST_GAIN')!.valueChangedEvent.addListener(() => {
    webAudioEngine.setHostGain(sliderStates.get('HOST_GAIN')!.getScaledValue());
  });
  sliderStates.get('PLAYLIST_GAIN')!.valueChangedEvent.addListener(() => {
    webAudioEngine.setPlaylistGain(sliderStates.get('PLAYLIST_GAIN')!.getScaledValue());
  });

  // ソース切替 → C++ WASM に直送
  comboBoxStates.get('SOURCE_SELECT')!.valueChangedEvent.addListener(() => {
    const ix = comboBoxStates.get('SOURCE_SELECT')!.getChoiceIndex();
    webAudioEngine.ensureAudioContext().then(() => webAudioEngine.setSourceSelect(ix));
  });

  // メータリングモード → C++ WASM に直送
  comboBoxStates.get('METERING_MODE')!.valueChangedEvent.addListener(() => {
    const ix = comboBoxStates.get('METERING_MODE')!.getChoiceIndex();
    webAudioEngine.setMeteringMode(ix);
  });

  // LPF
  toggleStates.get('LPF_ENABLED')!.valueChangedEvent.addListener(() => {
    webAudioEngine.setLpfEnabled(toggleStates.get('LPF_ENABLED')!.getValue());
  });
  sliderStates.get('LPF_FREQ')!.valueChangedEvent.addListener(() => {
    webAudioEngine.setLpfFrequency(sliderStates.get('LPF_FREQ')!.getScaledValue());
  });

  // トランスポート
  toggleStates.get('TRANSPORT_PLAYING')!.valueChangedEvent.addListener(() => {
    if (toggleStates.get('TRANSPORT_PLAYING')!.getValue()) {
      webAudioEngine.play();
    } else {
      webAudioEngine.pause();
    }
  });

  toggleStates.get('TRANSPORT_LOOP_ENABLED')!.valueChangedEvent.addListener(() => {
    webAudioEngine.setLoop(
      toggleStates.get('TRANSPORT_LOOP_ENABLED')!.getValue()
    );
  });

  // シーク
  sliderStates.get('TRANSPORT_SEEK_NORM')!.valueChangedEvent.addListener(() => {
    const norm = sliderStates.get('TRANSPORT_SEEK_NORM')!.getNormalisedValue();
    const dur = webAudioEngine.getDuration();
    if (dur > 0) webAudioEngine.seek(norm * dur);
  });

  // ループ範囲
  const updateLoopRange = () => {
    const dur = webAudioEngine.getDuration();
    if (dur <= 0) return;
    const startNorm = sliderStates.get('LOOP_START_NORM')!.getNormalisedValue();
    const endNorm = sliderStates.get('LOOP_END_NORM')!.getNormalisedValue();
    webAudioEngine.setLoop(
      toggleStates.get('TRANSPORT_LOOP_ENABLED')!.getValue(),
      startNorm * dur,
      endNorm * dur
    );
  };
  sliderStates.get('LOOP_START_NORM')!.valueChangedEvent.addListener(updateLoopRange);
  sliderStates.get('LOOP_END_NORM')!.valueChangedEvent.addListener(updateLoopRange);

  // トラック選択
  sliderStates
    .get('PLAYLIST_CURRENT_INDEX_NORM')!
    .valueChangedEvent.addListener(() => {
      const norm = sliderStates.get('PLAYLIST_CURRENT_INDEX_NORM')!.getNormalisedValue();
      const count = webAudioEngine.getTrackCount();
      if (count > 0) {
        const idx = Math.round(norm * (count - 1));
        webAudioEngine.selectTrack(idx);
      }
    });
}

registerDefaults();

// ---------- juce-framework-frontend-mirror 互換 API ----------

export function getSliderState(id: string): WebSliderState | null {
  return sliderStates.get(id) ?? null;
}

export function getToggleState(id: string): WebToggleState | null {
  return toggleStates.get(id) ?? null;
}

export function getComboBoxState(id: string): WebComboBoxState | null {
  return comboBoxStates.get(id) ?? null;
}

export function getNativeFunction(
  _name: string
): ((...args: unknown[]) => Promise<unknown>) | null {
  // Web 版のネイティブ関数は WebBridgeManager 経由で処理するため、
  // ここでは null を返す（juceBridge.callNative が直接ハンドルする）
  return null;
}
