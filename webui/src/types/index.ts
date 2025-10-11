// プレイリストアイテム
export interface PlaylistItem {
  id: string;
  name: string;
  duration: number;
  isLoaded: boolean;
  file?: string;
  // バックエンドから送られるファイル存在フラグ
  // 欠落時はUIでグレーアウト・選択不可にする
  exists?: boolean;
}

// トランスポート状態
export interface TransportState {
  isPlaying: boolean;
  position: number;
  loopStart: number;
  loopEnd: number;
  loopEnabled: boolean;
}

// オーディオソース
export const AudioSource = {
  Host: 'host',
  Playlist: 'playlist'
} as const;
export type AudioSource = typeof AudioSource[keyof typeof AudioSource];

// メーターレベル
export interface MeterLevels {
  left: number;
  right: number;
  // TruePeak（C++側で区間最大をdB化して送信）
  // 受信できない古いビルドとの互換のため optional
  truePeakLeft?: number;
  truePeakRight?: number;
  // Momentary LKFS values (for Momentary mode)
  momentary?: number;
  momentaryHold?: number;
}

// パラメータ
export interface Parameters {
  hostGain: number;     // HOST側ゲイン
  playlistGain: number; // PLAYLIST側ゲイン
  lpfFreq: number;
  lpfEnabled: boolean;
  meteringMode: number;  // 0=Peak, 1=RMS, 2=Momentary
}

// JUCE Backend型定義（juce-framework-frontend-mirrorに準拠）
declare class Backend {
  addEventListener(eventId: string, fn: (args: unknown) => unknown): [string, number];
  removeEventListener(param: [string, number]): void;
  emitEvent(eventId: string, object: unknown): void;
  emitByBackend(eventId: string, object: unknown): void;
}

declare global {
  interface Window {
    __JUCE__?: {
      backend: Backend;
      initialisationData: Record<string, unknown>;
      postMessage: () => void;
    };
    // JUCEのグローバル関数
    getNativeFunction?: (name: string) => (...args: unknown[]) => Promise<unknown>;
    getSliderState?: (name: string) => unknown;
    getToggleState?: (name: string) => unknown;
    getComboBoxState?: (name: string) => unknown;
    getBackendResourceAddress?: (path: string) => string;

    // WebUI 内部の一時的な更新ガード/状態共有
    __transportUpdateGuard?: {
      suppressLoopEnabled: (expected: boolean, durationMs?: number) => void;
      suppressPosition: (expected: number, durationMs?: number) => void;
    };
    __playlistUpdateState?: {
      suppressUntil: (duration: number) => void;
    };
    __gainDragState?: {
      setHostDragging: (dragging: boolean) => void;
      setPlaylistDragging: (dragging: boolean) => void;
    };
    __resizeRAF?: number;
  }
}

// JUCEイベントデータ型定義
export interface PlaylistUpdateData {
  items?: PlaylistItem[];
  currentIndex?: number;
  revision?: number;
}

export interface TransportUpdateData {
  isPlaying?: boolean;
  position?: number;
  loopStart?: number;
  loopEnd?: number;
  loopEnabled?: boolean;
  currentIndex?: number;
  sequenceNumber?: number;
  revision?: number;
  sessionId?: number;
}

export interface MeterUpdateData {
  meteringMode?: number;  // 0=Peak, 1=RMS, 2=Momentary
  host?: {
    rmsLeft?: number;
    rmsRight?: number;
    truePeakLeft?: number;
    truePeakRight?: number;
    momentary?: number;
    momentaryHold?: number;
  };
  playlist?: {
    rmsLeft?: number;
    rmsRight?: number;
    truePeakLeft?: number;
    truePeakRight?: number;
    momentary?: number;
    momentaryHold?: number;
  };
  output?: {
    rmsLeft?: number;
    rmsRight?: number;
    truePeakLeft?: number;
    truePeakRight?: number;
    momentary?: number;
    momentaryHold?: number;
  };
  momentary?: number;
  momentaryHold?: number;
}

export interface ParameterUpdateData {
  id: string;
  value: number;
}

export interface InitialParametersData {
  hostGain?: number;
  playlistGain?: number;
  lpfFreq?: number;
  lpfEnabled?: boolean;
  meteringModeIndex?: number;  // 0=Peak, 1=RMS, 2=Momentary
}

export interface SourceUpdateData {
  source: 'host' | 'playlist';
}

export interface TrackChangeData {
  items?: PlaylistItem[];
  currentIndex?: number;
  isPlaying?: boolean;
  position?: number;
  loopStart?: number;
  loopEnd?: number;
  loopEnabled?: boolean;
  playlistRevision?: number;
  transportRevision?: number;
}

export interface ErrorNotificationData {
  code?: number;
  severity?: 'info' | 'warning' | 'error' | 'critical';
  message?: string;
  details?: string;
  filePath?: string;
}