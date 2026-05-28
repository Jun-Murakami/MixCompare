// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
/**
 * juce-framework-frontend-mirror の iPlug2 互換 shim。
 *
 * iPlug2 へ移植したプラグインビルド時、Vite エイリアス (vite.config.ts) で本家
 * モジュールの代わりにこれが解決される。既存 React コンポーネントの import
 * (`getSliderState` / `getToggleState` / `getComboBoxState` / `getNativeFunction`)
 * と `window.__JUCE__.backend` (= bridge/juce.ts の JuceBridgeManager) をそのまま
 * 動かすため、JUCE WebView 由来の API 形を維持したまま、内部を iPlug2 の
 * WebViewEditorDelegate プロトコル (SPVFD/SAMFD ⇄ SPVFUI/BPCFUI/EPCFUI/SAMFUI) に橋渡しする。
 *
 * C++ 側 plugin/ParameterIDs.h の EParams / EArbitraryMsgTags と必ず同期する。
 */

// ---------------------------------------------------------------------------
// 型 (window 拡張)
// ---------------------------------------------------------------------------
type CppMessage =
  | { msg: 'BPCFUI'; paramIdx: number }
  | { msg: 'EPCFUI'; paramIdx: number }
  | { msg: 'SPVFUI'; paramIdx: number; value: number }
  | { msg: 'SAMFUI'; msgTag: number; ctrlTag: number; data: string };

declare global {
  interface Window {
    webkit?: { messageHandlers?: { callback?: { postMessage: (m: unknown) => void } } };
    chrome?: { webview?: { postMessage: (m: unknown) => void } };
    SPVFD?: (paramIdx: number, normalized: number) => void;
    SCVFD?: (ctrlTag: number, normalized: number) => void;
    SCMFD?: (ctrlTag: number, msgTag: number, size: number, base64: string) => void;
    SAMFD?: (msgTag: number, size: number, base64: string) => void;
    SMMFD?: (status: number, d1: number, d2: number) => void;
  }
}

// ---------------------------------------------------------------------------
// メッセージタグ (C++ plugin/ParameterIDs.h EArbitraryMsgTags と一致)
// ---------------------------------------------------------------------------
const MSG = {
  // JS → C++
  PlaylistAction: 100,
  WindowAction: 101,
  SystemAction: 102,
  OpenUrl: 103,
  MeteringReset: 104,
  RequestPlaylistUpdate: 105,
  // C++ → JS
  InitialParams: 200,
  MeterUpdate: 201,
  TransportUpdate: 202,
  TransportPositionUpdate: 203,
  PlaylistUpdate: 204,
  TrackChange: 205,
  Error: 206,
  DpiScale: 207,
} as const;

// C++ → JS の msgTag を JuceBridgeManager のイベント名にマップする。
const MSG_TO_EVENT: Record<number, string> = {
  [MSG.MeterUpdate]: 'meterUpdate',
  [MSG.TransportUpdate]: 'transportUpdate',
  [MSG.TransportPositionUpdate]: 'transportPositionUpdate',
  [MSG.PlaylistUpdate]: 'playlistUpdate',
  [MSG.TrackChange]: 'trackChange',
  [MSG.Error]: 'errorNotification',
  [MSG.DpiScale]: 'dpiScaleChanged',
};

// ---------------------------------------------------------------------------
// パラメータテーブル (index = C++ EParams の並びと完全一致)
// ---------------------------------------------------------------------------
type ParamKind = 'slider' | 'toggle' | 'combo';
interface ParamDef {
  id: string;
  kind: ParamKind;
  start?: number;
  end?: number;
  label?: string;
  choices?: string[];
}

const PARAMS: ParamDef[] = [
  { id: 'HOST_GAIN', kind: 'slider', start: -120, end: 0, label: 'dB' },
  { id: 'PLAYLIST_GAIN', kind: 'slider', start: -120, end: 0, label: 'dB' },
  { id: 'LPF_FREQ', kind: 'slider', start: 20, end: 20000, label: 'Hz' },
  { id: 'LPF_ENABLED', kind: 'toggle' },
  { id: 'HOST_SYNC_CAPABLE', kind: 'toggle' },
  { id: 'HOST_SYNC_ENABLED', kind: 'toggle' },
  { id: 'SOURCE_SELECT', kind: 'combo', choices: ['Host', 'Playlist'] },
  { id: 'METERING_MODE', kind: 'combo', choices: ['Peak', 'RMS', 'Momentary'] },
  { id: 'TRANSPORT_PLAYING', kind: 'toggle' },
  { id: 'TRANSPORT_LOOP_ENABLED', kind: 'toggle' },
  { id: 'TRANSPORT_SEEK_NORM', kind: 'slider', start: 0, end: 1 },
  { id: 'LOOP_START_NORM', kind: 'slider', start: 0, end: 1 },
  { id: 'LOOP_END_NORM', kind: 'slider', start: 0, end: 1 },
  { id: 'PLAYLIST_CURRENT_INDEX_NORM', kind: 'slider', start: 0, end: 1 },
];

// ---------------------------------------------------------------------------
// 送信 (JS → C++)。iPlug2 は「JS オブジェクトをそのまま」postMessage する前提
// (JSON.stringify した文字列を渡すと WebView2 で二重エンコード → abort する)。
// ---------------------------------------------------------------------------
function postMessageToCpp(payload: CppMessage): void {
  if (window.webkit?.messageHandlers?.callback) {
    window.webkit.messageHandlers.callback.postMessage(payload);
    return;
  }
  if (window.chrome?.webview) {
    window.chrome.webview.postMessage(payload);
    return;
  }
  // ホスト未接続 (Vite dev server を直接ブラウザで開いた dev-mock 環境) では握り潰す。
  if (import.meta.env.DEV) {
    // eslint-disable-next-line no-console
    console.debug('[iplug-shim] no host bridge, dropping:', payload);
  }
}

function bytesToBase64(bytes: Uint8Array): string {
  let bin = '';
  for (let i = 0; i < bytes.length; i++) bin += String.fromCharCode(bytes[i]);
  return btoa(bin);
}
function utf8ToBase64(s: string): string {
  return bytesToBase64(new TextEncoder().encode(s));
}
function base64ToUtf8(base64: string): string {
  const bin = atob(base64);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  return new TextDecoder('utf-8').decode(bytes);
}

function sendParamNormalised(idx: number, value: number): void {
  postMessageToCpp({ msg: 'SPVFUI', paramIdx: idx, value });
}
function sendSAMFUI(msgTag: number, data: Uint8Array | string): void {
  const b64 = typeof data === 'string' ? utf8ToBase64(data) : bytesToBase64(data);
  postMessageToCpp({ msg: 'SAMFUI', msgTag, ctrlTag: -1, data: b64 });
}

// ---------------------------------------------------------------------------
// ListenerList (juce-framework-frontend-mirror 互換)
// ---------------------------------------------------------------------------
class ListenerList {
  private listeners = new Map<number, (args: unknown) => void>();
  private nextId = 1;
  addListener(fn: (args: unknown) => void): number {
    const id = this.nextId++;
    this.listeners.set(id, fn);
    return id;
  }
  removeListener(id: number): void {
    this.listeners.delete(id);
  }
  callListeners(payload?: unknown): void {
    this.listeners.forEach((fn) => fn(payload));
  }
}

// ---------------------------------------------------------------------------
// 状態オブジェクト
// ---------------------------------------------------------------------------
class SliderState {
  readonly name: string;
  readonly identifier: string;
  scaledValue = 0;
  properties: {
    start: number; end: number; skew: number; name: string;
    label: string; numSteps: number; interval: number; parameterIndex: number;
  };
  readonly valueChangedEvent = new ListenerList();
  readonly propertiesChangedEvent = new ListenerList();
  private normValue = 0;
  private readonly index: number;

  constructor(index: number, def: ParamDef) {
    this.index = index;
    this.name = def.id;
    this.identifier = def.id;
    this.properties = {
      start: def.start ?? 0,
      end: def.end ?? 1,
      skew: 1,
      name: def.id,
      label: def.label ?? '',
      numSteps: 0,
      interval: 0,
      parameterIndex: index,
    };
  }

  getNormalisedValue(): number { return this.normValue; }
  getScaledValue(): number {
    // 浮動小数の normalize↔denormalize 精度ロスを吸収する (例: LPF 500Hz 線形
    // 20..20000 で (500-20)/19980*19980+20 が 499.99999998 になり Math.floor で 499 と
    // 表示される)。固定桁丸めだと値域によって精度ロス側に振れるので、有効数字 6 桁で
    // 丸めて任意レンジで整数近接値を確実に整数化する。
    const raw = this.properties.start + this.normValue * (this.properties.end - this.properties.start);
    if (!isFinite(raw) || raw === 0) return raw;
    return parseFloat(raw.toPrecision(6));
  }
  setNormalisedValue(v: number): void {
    this.normValue = Math.max(0, Math.min(1, v));
    this.scaledValue = this.getScaledValue();
    sendParamNormalised(this.index, this.normValue);
    this.valueChangedEvent.callListeners();
  }
  sliderDragStarted(): void { postMessageToCpp({ msg: 'BPCFUI', paramIdx: this.index }); }
  sliderDragEnded(): void { postMessageToCpp({ msg: 'EPCFUI', paramIdx: this.index }); }

  /** C++ (SPVFD) からの更新。SPVFUI を送り返さない (echo 防止)。 */
  setFromHost(norm: number): void {
    this.normValue = Math.max(0, Math.min(1, norm));
    this.scaledValue = this.getScaledValue();
    this.valueChangedEvent.callListeners();
  }
  applySchema(min: number, max: number, numSteps: number): void {
    this.properties = { ...this.properties, start: min, end: max, numSteps };
    this.scaledValue = this.getScaledValue();
    this.propertiesChangedEvent.callListeners();
  }
}

class ToggleState {
  readonly name: string;
  readonly identifier: string;
  value = false;
  properties: { name: string; parameterIndex: number };
  readonly valueChangedEvent = new ListenerList();
  readonly propertiesChangedEvent = new ListenerList();
  private readonly index: number;

  constructor(index: number, def: ParamDef) {
    this.index = index;
    this.name = def.id;
    this.identifier = def.id;
    this.properties = { name: def.id, parameterIndex: index };
  }
  getValue(): boolean { return this.value; }
  setValue(v: boolean): void {
    this.value = v;
    sendParamNormalised(this.index, v ? 1 : 0);
    this.valueChangedEvent.callListeners();
  }
  setFromHost(norm: number): void {
    this.value = norm >= 0.5;
    this.valueChangedEvent.callListeners();
  }
  applySchema(): void { this.propertiesChangedEvent.callListeners(); }
}

class ComboBoxState {
  readonly name: string;
  readonly identifier: string;
  value = 0;
  properties: { name: string; parameterIndex: number; choices: string[] };
  readonly valueChangedEvent = new ListenerList();
  readonly propertiesChangedEvent = new ListenerList();
  private readonly index: number;

  constructor(index: number, def: ParamDef) {
    this.index = index;
    this.name = def.id;
    this.identifier = def.id;
    this.properties = { name: def.id, parameterIndex: index, choices: def.choices ?? [] };
  }
  private get numItems(): number { return Math.max(1, this.properties.choices.length); }
  getChoiceIndex(): number { return this.value; }
  setChoiceIndex(i: number): void {
    this.value = Math.max(0, Math.min(this.numItems - 1, i));
    const denom = this.numItems - 1;
    sendParamNormalised(this.index, denom > 0 ? this.value / denom : 0);
    this.valueChangedEvent.callListeners();
  }
  setFromHost(norm: number): void {
    const denom = this.numItems - 1;
    this.value = denom > 0 ? Math.round(norm * denom) : 0;
    this.valueChangedEvent.callListeners();
  }
  applySchema(): void { this.propertiesChangedEvent.callListeners(); }
}

// ---------------------------------------------------------------------------
// レジストリ
// ---------------------------------------------------------------------------
const sliderStates = new Map<string, SliderState>();
const toggleStates = new Map<string, ToggleState>();
const comboStates = new Map<string, ComboBoxState>();

PARAMS.forEach((def, idx) => {
  if (def.kind === 'slider') sliderStates.set(def.id, new SliderState(idx, def));
  else if (def.kind === 'toggle') toggleStates.set(def.id, new ToggleState(idx, def));
  else comboStates.set(def.id, new ComboBoxState(idx, def));
});

function stateByIndex(idx: number): SliderState | ToggleState | ComboBoxState | undefined {
  const def = PARAMS[idx];
  if (!def) return undefined;
  if (def.kind === 'slider') return sliderStates.get(def.id);
  if (def.kind === 'toggle') return toggleStates.get(def.id);
  return comboStates.get(def.id);
}

// ---------------------------------------------------------------------------
// C++ → JS イベント配信 (window.__JUCE__.backend)
// ---------------------------------------------------------------------------
const eventListeners = new Map<string, Map<number, (args: unknown) => void>>();
let eventListenerNextId = 1;

function dispatchEvent(eventId: string, data: unknown): void {
  const map = eventListeners.get(eventId);
  if (map) map.forEach((fn) => fn(data));
}

// JS → C++ の emitEvent マッピング (JuceBridgeManager.emitEvent 経由)。
const EVENT_TO_MSG: Record<string, number> = {
  requestPlaylistUpdate: MSG.RequestPlaylistUpdate,
};

const backend = {
  addEventListener(eventId: string, fn: (args: unknown) => void): [string, number] {
    let map = eventListeners.get(eventId);
    if (!map) {
      map = new Map();
      eventListeners.set(eventId, map);
    }
    const id = eventListenerNextId++;
    map.set(id, fn);
    return [eventId, id];
  },
  removeEventListener(handle: [string, number]): void {
    const [eventId, id] = handle;
    eventListeners.get(eventId)?.delete(id);
  },
  emitEvent(eventId: string, data: unknown): void {
    const tag = EVENT_TO_MSG[eventId];
    if (tag !== undefined) {
      sendSAMFUI(tag, typeof data === 'string' ? data : JSON.stringify(data ?? {}));
    }
  },
  emitByBackend(eventId: string, data: unknown): void {
    dispatchEvent(eventId, data);
  },
};

// ---------------------------------------------------------------------------
// C++ → JS 受信口 (iPlug2 WebViewEditorDelegate が EvaluateJavaScript で呼ぶ)
// ---------------------------------------------------------------------------
interface SchemaParam { id: number; type?: string; min?: number; max?: number }

function handleParamSchema(json: string): void {
  let parsed: { params?: SchemaParam[] };
  try {
    parsed = JSON.parse(json) as { params?: SchemaParam[] };
  } catch {
    return;
  }
  const params = parsed.params ?? [];
  for (const p of params) {
    const st = stateByIndex(p.id);
    if (!st) continue;
    const min = p.min ?? 0;
    const max = p.max ?? 1;
    if (st instanceof SliderState) {
      // 連続スライダーは細かいステップ、enum/bool 由来の int は max-min+1 段。
      const numSteps = p.type === 'float' ? 0 : Math.round(max - min) + 1;
      st.applySchema(min, max, numSteps);
    } else {
      st.applySchema();
    }
  }
}

function installReceivers(): void {
  window.SPVFD = (paramIdx: number, normalized: number): void => {
    const st = stateByIndex(paramIdx);
    if (st) st.setFromHost(normalized);
  };
  window.SCVFD = (): void => { /* 非 param コントロール: Phase 1 未使用 */ };
  window.SCMFD = (): void => { /* バイナリ宛コントロール: 未使用 */ };
  window.SMMFD = (): void => { /* MIDI: 未使用 */ };
  window.SAMFD = (msgTag: number, _size: number, base64: string): void => {
    if (msgTag === -1) {
      handleParamSchema(base64ToUtf8(base64));
      return;
    }
    const eventId = MSG_TO_EVENT[msgTag];
    if (!eventId) return;
    let payload: unknown = {};
    if (base64) {
      try {
        payload = JSON.parse(base64ToUtf8(base64));
      } catch {
        payload = {};
      }
    }
    dispatchEvent(eventId, payload);
  };
}

function installJuceGlobal(): void {
  // bridge/juce.ts の JuceBridgeManager は window.__JUCE__.backend をポーリングする。
  // window.__JUCE__ は types/index.ts と本家 global.d.ts の二重宣言で交差型になり
  // 直接代入が通らないため、緩めた window キャスト経由で設定する。
  (window as unknown as { __JUCE__: unknown }).__JUCE__ = {
    backend,
    initialisationData: {},
    postMessage: () => {},
  };
}

installReceivers();
installJuceGlobal();

// ---------------------------------------------------------------------------
// juce-framework-frontend-mirror 互換エクスポート
// ---------------------------------------------------------------------------
export function getSliderState(id: string): SliderState | undefined {
  return sliderStates.get(id);
}
export function getToggleState(id: string): ToggleState | undefined {
  return toggleStates.get(id);
}
export function getComboBoxState(id: string): ComboBoxState | undefined {
  return comboStates.get(id);
}
export function getBackendResourceAddress(path: string): string {
  return path;
}

// getNativeFunction(name) は引数を iPlug2 の SAMFUI コマンドに変換する関数を返す。
export function getNativeFunction(
  name: string
): (...args: unknown[]) => Promise<unknown> {
  return (...args: unknown[]): Promise<unknown> => {
    switch (name) {
      case 'open_url': {
        const url = typeof args[0] === 'string' ? args[0] : '';
        sendSAMFUI(MSG.OpenUrl, url);
        return Promise.resolve(true);
      }
      case 'window_action': {
        const action = args[0];
        if (action === 'resizeTo') {
          const w = Math.round(Number(args[1] ?? 0));
          const h = Math.round(Number(args[2] ?? 0));
          const buf = new ArrayBuffer(8);
          const view = new DataView(buf);
          view.setInt32(0, w, true);
          view.setInt32(4, h, true);
          sendSAMFUI(MSG.WindowAction, new Uint8Array(buf));
        }
        return Promise.resolve(true);
      }
      case 'system_action': {
        const action = typeof args[0] === 'string' ? args[0] : '';
        sendSAMFUI(MSG.SystemAction, action);
        return Promise.resolve(true);
      }
      case 'metering_reset': {
        const which = typeof args[0] === 'string' ? args[0] : '';
        sendSAMFUI(MSG.MeteringReset, which);
        return Promise.resolve(true);
      }
      case 'playlist_action': {
        // Phase 1: C++ 側はスタブ。将来のために action + args を JSON で送る。
        sendSAMFUI(MSG.PlaylistAction, JSON.stringify({ args }));
        return Promise.resolve(null);
      }
      default:
        return Promise.resolve(null);
    }
  };
}
