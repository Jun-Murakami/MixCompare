// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
/**
 * JuceBridgeManager の Web 互換実装。
 * 既存コンポーネントが `juceBridge.addEventListener(...)` 等を呼ぶのと同じ API を提供する。
 */

import { webAudioEngine } from './WebAudioEngine';
import { getSliderState, getToggleState } from './juce-shim';

type EventCallback = (data: unknown) => void;

function snapshotFiles(files: FileList | File[] | null | undefined): File[] {
  return Array.from(files ?? []).filter((file): file is File => file instanceof File);
}

async function importFiles(files: File[]): Promise<void> {
  if (files.length === 0) {
    return;
  }

  await webAudioEngine.ensureAudioContext();
  await webAudioEngine.addFiles(files);
}

class WebBridgeManager {
  private initialized = false;
  private initCallbacks: Array<() => void> = [];
  private suppressEchoUntil = new Map<string, number>();
  private outgoingLockUntil = new Map<string, number>();

  constructor() {
    this.initialize();
  }

  private async initialize() {
    try {
      await webAudioEngine.initialize();
      // Host サンプル音源をプリロード（デモ用）
      await webAudioEngine.loadHostSample('/audio/sample.mp3');
    } catch (err) {
      console.error('[WebBridge] Initialization failed:', err);
    }
    // WASM やサンプルが無くても UI は表示できるように初期化完了とする
    this.initialized = true;
    this.initCallbacks.forEach((cb) => cb());
    this.initCallbacks = [];
  }

  public whenReady(callback: () => void) {
    if (this.initialized) {
      callback();
    } else {
      this.initCallbacks.push(callback);
    }
  }

  public async callNative(functionName: string, ...args: unknown[]): Promise<unknown> {
    // Web 版のネイティブ関数をローカルで処理
    if (functionName === 'playlist_action') {
      return this.handlePlaylistAction(args);
    }
    if (functionName === 'metering_reset') {
      if (args[0] === 'momentary') {
        webAudioEngine.resetMeteringMomentary();
      }
      return null;
    }
    if (functionName === 'system_action') {
      // ready, forward_key_event 等は Web では no-op
      return null;
    }
    if (functionName === 'window_action') {
      // リサイズは Web では no-op
      return null;
    }
    if (functionName === 'open_url') {
      if (typeof args[0] === 'string') window.open(args[0], '_blank');
      return true;
    }
    return null;
  }

  private async handlePlaylistAction(args: unknown[]): Promise<unknown> {
    const action = args[0] as string;

    switch (action) {
      case 'add': {
        const input = document.createElement('input');
        input.type = 'file';
        input.multiple = true;
        input.accept = 'audio/*,.wav,.flac,.mp3,.m4a,.aac,.ogg';

        // Safari では DOM 未接続 input の file picker が不安定なことがあるため body に一時接続する。
        input.style.position = 'fixed';
        input.style.left = '-9999px';
        input.style.width = '1px';
        input.style.height = '1px';
        input.style.opacity = '0';
        input.style.pointerEvents = 'none';
        document.body.appendChild(input);

        return new Promise<void>((resolve) => {
          let settled = false;

          const cleanup = () => {
            input.removeEventListener('change', handleChange);
            input.removeEventListener('cancel', handleCancel);
            input.remove();
          };

          const finish = () => {
            if (settled) {
              return;
            }
            settled = true;
            cleanup();
            resolve();
          };

          const handleChange = async () => {
            const files = snapshotFiles(input.files);

            try {
              await importFiles(files);
            } finally {
              finish();
            }
          };

          const handleCancel = () => {
            finish();
          };

          input.addEventListener('change', handleChange, { once: true });
          input.addEventListener('cancel', handleCancel, { once: true });
          input.click();
        });
      }
      case 'add_files': {
        const files = snapshotFiles(args[1] as FileList | File[]);
        await importFiles(files);
        return null;
      }
      case 'remove': {
        const id = args[1] as string;
        webAudioEngine.removeTrack(id);
        return null;
      }
      case 'reorder': {
        const oldIdx = args[1] as number;
        const newIdx = args[2] as number;
        webAudioEngine.reorderTrack(oldIdx, newIdx);
        return null;
      }
      case 'clear':
        webAudioEngine.clearTracks();
        return null;
      case 'export':
      case 'import':
        // デモ版では未実装
        return null;
      default:
        return null;
    }
  }

  public addEventListener(event: string, callback: EventCallback): string {
    return webAudioEngine.addEventListener(event, callback);
  }

  public removeEventListener(key: string): void {
    webAudioEngine.removeEventListener(key);
  }

  public emitEvent(_event: string, _data: unknown): void {
    // Web 版では backend への送信は不要（ローカル処理）
  }

  public lockOutgoing(id: string, ms = 220): void {
    this.outgoingLockUntil.set(id, Date.now() + ms);
  }

  public addParameterListener(
    id: string,
    callback: (value: number | boolean) => void
  ): string {
    const uniqueKey = `param_${id}_${Date.now()}`;

    const knownToggles = new Set(['LPF_ENABLED', 'TRANSPORT_PLAYING', 'TRANSPORT_LOOP_ENABLED', 'HOST_SYNC_ENABLED', 'HOST_SYNC_CAPABLE']);
    const isToggle = knownToggles.has(id);

    if (isToggle) {
      const state = getToggleState(id);
      if (state) {
        state.valueChangedEvent.addListener(() => {
          const until = this.suppressEchoUntil.get(id) || 0;
          if (Date.now() < until) return;
          callback(state.getValue());
        });
      }
    } else {
      const state = getSliderState(id);
      if (state) {
        state.valueChangedEvent.addListener(() => {
          const until = this.suppressEchoUntil.get(id) || 0;
          if (Date.now() < until) return;
          callback(state.getNormalisedValue());
        });
      }
    }

    return uniqueKey;
  }

  public removeParameterListener(_id: string): void {
    // 簡易実装: リスナー解除（WebParamState のリスナーは永続）
  }
}

export const webBridge = new WebBridgeManager();
