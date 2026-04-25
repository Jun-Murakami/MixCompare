/**
 * Web Audio API + WASM AudioWorklet のマネージャ。
 * トランスポート・再生位置・ループは全て C++ WASM 側が管理。
 * この層はファイルデコード、worklet ↔ main thread の中継、UI イベント発行のみ。
 */

type EventCallback = (data: unknown) => void;

interface TrackInfo {
  id: string;
  name: string;
  duration: number;
  sampleRate: number;
  isLoaded: boolean;
  exists: boolean;
}

export class WebAudioEngine {
  private audioContext: AudioContext | null = null;
  private workletNode: AudioWorkletNode | null = null;
  private listeners = new Map<string, EventCallback>();
  private nextListenerId = 1;

  // プレイリスト（UI 表示用のメタデータのみ。PCM は WASM が保持）
  private tracks: TrackInfo[] = [];
  private currentTrackIndex = -1;

  // C++ から通知される最新状態（~20Hz 更新）
  private position = 0;
  private duration = 0;
  private isPlaying = false;

  // ループ状態（JS 側でもトラッキング。C++ が管理するが UI 通知用に保持）
  private loopEnabled = false;
  private loopStart = 0;
  private loopEnd = 0;

  // Host サンプルは C++ WASM 側で管理。JS 側は表示用に直近のメタ情報のみ保持。
  private currentHostSource: { name: string; duration: number } | null = null;

  // Host 専用 transport の最新状態（C++ から ~20Hz で更新）
  private hostPosition = 0;
  private hostDuration = 0;
  private hostIsPlaying = false;
  private hostLoopEnabled = true;

  private initialized = false;
  private initResolvers: Array<() => void> = [];

  async initialize(): Promise<void> {
    if (this.initialized) return;
    try {
      this.audioContext = new AudioContext({ sampleRate: 48000 });
      await this.audioContext.audioWorklet.addModule('/worklet/dsp-processor.js');

      this.workletNode = new AudioWorkletNode(this.audioContext, 'dsp-processor', {
        numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
      });
      this.workletNode.connect(this.audioContext.destination);
      this.workletNode.port.onmessage = (e) => this.handleWorkletMessage(e.data);

      // WASM ロード
      const resp = await fetch('/wasm/mixcompare_dsp.wasm');
      if (resp.ok) {
        const bytes = await resp.arrayBuffer();
        this.workletNode.port.postMessage({ type: 'init-wasm', wasmBytes: bytes }, [bytes]);
        await new Promise<void>((resolve, reject) => {
          const t = setTimeout(() => reject(new Error('WASM init timeout')), 10000);
          this.initResolvers.push(() => { clearTimeout(t); resolve(); });
        });
      } else {
        console.warn('[WebAudioEngine] WASM not found');
      }
    } catch (err) {
      console.warn('[WebAudioEngine] Init error:', err);
    }
    this.initialized = true;
  }

  isInitialized(): boolean { return this.initialized; }

  async ensureAudioContext(): Promise<void> {
    if (this.audioContext?.state === 'suspended') await this.audioContext.resume();
  }

  // ====== イベント ======

  addEventListener(event: string, callback: EventCallback): string {
    const id = `web_${this.nextListenerId++}`;
    this.listeners.set(`${event}:${id}`, callback);
    return `${event}:${id}`;
  }

  removeEventListener(key: string): void { this.listeners.delete(key); }

  private emit(event: string, data: unknown): void {
    this.listeners.forEach((cb, key) => { if (key.startsWith(`${event}:`)) cb(data); });
  }

  // ====== Worklet メッセージ ======

  private handleWorkletMessage(msg: Record<string, unknown>) {
    switch (msg.type) {
      case 'wasm-ready':
        this.initResolvers.forEach((r) => r());
        this.initResolvers = [];
        break;

      case 'wasm-error':
        this.emit('errorNotification', { severity: 'error', message: 'WASM init failed', details: String(msg.error) });
        break;

      case 'state-update': {
        this.position = msg.position as number;
        this.duration = msg.duration as number;
        this.isPlaying = msg.isPlaying as boolean;

        // Host 独立 transport の最新値
        this.hostPosition = (msg.hostPosition as number) ?? 0;
        this.hostDuration = (msg.hostDuration as number) ?? 0;
        this.hostIsPlaying = (msg.hostIsPlaying as boolean) ?? false;

        // 位置・トランスポート通知（playlist）
        this.emit('transportPositionUpdate', {
          position: this.position,
          duration: this.duration,
          isPlaying: this.isPlaying,
        });

        // Host 用通知（バーが購読）
        this.emit('hostTransportPositionUpdate', {
          position: this.hostPosition,
          duration: this.hostDuration,
          isPlaying: this.hostIsPlaying,
        });

        // 曲末自然停止 (playlist)
        if (msg.stoppedAtEnd) {
          this.isPlaying = false;
          this.emit('transportUpdate', {
            isPlaying: false,
            position: this.position,
            duration: this.duration,
            loopStart: 0,
            loopEnd: this.duration,
            loopEnabled: false,
          });
        }

        // Host が loop 無効で末尾に到達した場合
        if (msg.hostStoppedAtEnd) {
          this.hostIsPlaying = false;
          this.emit('hostTransportUpdate', {
            isPlaying: false,
            position: this.hostPosition,
            duration: this.hostDuration,
            loopEnabled: this.hostLoopEnabled,
          });
        }

        // メータ（C++ から dB 変換済みで受信、プラグインと同フォーマット）
        this.emit('meterUpdate', {
          meteringMode: msg.meteringMode,
          host: msg.host,
          playlist: msg.playlist,
          output: msg.output,
        });
        break;
      }
    }
  }

  // ====== ファイル読み込み ======

  async addFiles(files: readonly File[]): Promise<void> {
    if (!this.audioContext) return;
    for (const file of files) {
      try {
        const ab = await file.arrayBuffer();
        const audioBuf = await this.audioContext.decodeAudioData(ab);
        const leftCopy = new Float32Array(audioBuf.getChannelData(0));
        const rightCopy = new Float32Array(
          audioBuf.numberOfChannels >= 2 ? audioBuf.getChannelData(1) : audioBuf.getChannelData(0)
        );

        const index = this.tracks.length;
        this.tracks.push({
          id: `track_${Date.now()}_${index}`,
          name: file.name.replace(/\.[^.]+$/, ''),
          duration: audioBuf.duration,
          sampleRate: audioBuf.sampleRate,
          isLoaded: true, exists: true,
        });

        // Worklet → WASM にPCM転送
        this.workletNode?.port.postMessage({
          type: 'load-track', index,
          left: leftCopy.buffer, right: rightCopy.buffer,
          numSamples: leftCopy.length, trackSampleRate: audioBuf.sampleRate,
        }, [leftCopy.buffer, rightCopy.buffer]);

        if (this.currentTrackIndex < 0) {
          this.currentTrackIndex = 0;
          this.duration = audioBuf.duration;
          this.workletNode?.port.postMessage({ type: 'select-track', index: 0 });
        }
      } catch (err) {
        this.emit('errorNotification', { severity: 'error', message: `Decode failed: ${file.name}`, details: String(err) });
      }
    }
    this.emitPlaylistUpdate();
  }

  removeTrack(id: string): void {
    const idx = this.tracks.findIndex((t) => t.id === id);
    if (idx < 0) return;
    this.tracks.splice(idx, 1);
    this.workletNode?.port.postMessage({ type: 'remove-track', index: idx });
    if (this.currentTrackIndex >= this.tracks.length)
      this.currentTrackIndex = Math.max(0, this.tracks.length - 1);
    this.emitPlaylistUpdate();
  }

  reorderTrack(oldIndex: number, newIndex: number): void {
    if (oldIndex < 0 || oldIndex >= this.tracks.length) return;
    if (newIndex < 0 || newIndex >= this.tracks.length) return;
    const [item] = this.tracks.splice(oldIndex, 1);
    this.tracks.splice(newIndex, 0, item);
    if (this.currentTrackIndex === oldIndex) this.currentTrackIndex = newIndex;
    else if (oldIndex < this.currentTrackIndex && newIndex >= this.currentTrackIndex) this.currentTrackIndex--;
    else if (oldIndex > this.currentTrackIndex && newIndex <= this.currentTrackIndex) this.currentTrackIndex++;
    this.emitPlaylistUpdate();
  }

  clearTracks(): void {
    this.tracks = [];
    this.currentTrackIndex = -1;
    this.isPlaying = false;
    this.position = 0;
    this.duration = 0;
    this.workletNode?.port.postMessage({ type: 'clear-tracks' });
    this.emitPlaylistUpdate();
    this.emitTransportUpdate();
  }

  selectTrack(index: number): void {
    if (index < 0 || index >= this.tracks.length) return;
    this.currentTrackIndex = index;
    this.position = 0;
    this.duration = this.tracks[index].duration;
    this.loopEnabled = false;
    this.loopStart = 0;
    this.loopEnd = this.duration;
    this.workletNode?.port.postMessage({ type: 'select-track', index });
    this.emitPlaylistUpdate();
    this.emitTrackChange();
  }

  // ====== トランスポート → WASM 直送 ======

  async play(): Promise<void> {
    await this.ensureAudioContext();
    this.workletNode?.port.postMessage({ type: 'set-playing', value: true });
    this.isPlaying = true;
    this.emitTransportUpdate();
  }

  pause(): void {
    this.workletNode?.port.postMessage({ type: 'set-playing', value: false });
    this.isPlaying = false;
    this.emitTransportUpdate();
  }

  seek(positionSec: number): void {
    this.position = positionSec;
    this.workletNode?.port.postMessage({ type: 'seek', position: positionSec });
    // 位置確認は C++ worklet の ~20Hz state-update に委ねる。
    // ここで即時 emit すると React のバッチ処理と干渉して seekPending が解除されない。
  }

  setLoop(enabled: boolean, startSec?: number, endSec?: number): void {
    this.loopEnabled = enabled;
    this.loopStart = startSec ?? this.loopStart;
    this.loopEnd = endSec ?? this.loopEnd;
    // ループ範囲が未設定なら全体
    if (this.loopEnabled && this.loopEnd <= this.loopStart && this.duration > 0) {
      this.loopStart = 0;
      this.loopEnd = this.duration;
    }
    this.workletNode?.port.postMessage({
      type: 'set-loop', enabled: this.loopEnabled,
      start: this.loopStart, end: this.loopEnd,
    });
    this.emitTransportUpdate();
  }

  // ====== Host 独立トランスポート（playlist の play/pause/seek/setLoop と別系統） ======

  async hostPlay(): Promise<void> {
    await this.ensureAudioContext();
    this.workletNode?.port.postMessage({ type: 'host-set-playing', value: true });
    this.hostIsPlaying = true;
    this.emit('hostTransportUpdate', {
      isPlaying: true,
      position: this.hostPosition,
      duration: this.hostDuration,
      loopEnabled: this.hostLoopEnabled,
    });
  }

  hostPause(): void {
    this.workletNode?.port.postMessage({ type: 'host-set-playing', value: false });
    this.hostIsPlaying = false;
    this.emit('hostTransportUpdate', {
      isPlaying: false,
      position: this.hostPosition,
      duration: this.hostDuration,
      loopEnabled: this.hostLoopEnabled,
    });
  }

  hostSeek(positionSec: number): void {
    this.hostPosition = positionSec;
    this.workletNode?.port.postMessage({ type: 'host-seek', position: positionSec });
  }

  hostSetLoop(enabled: boolean): void {
    this.hostLoopEnabled = enabled;
    this.workletNode?.port.postMessage({ type: 'host-set-loop', enabled });
    this.emit('hostTransportUpdate', {
      isPlaying: this.hostIsPlaying,
      position: this.hostPosition,
      duration: this.hostDuration,
      loopEnabled: enabled,
    });
  }

  getHostIsPlaying(): boolean { return this.hostIsPlaying; }
  getHostLoopEnabled(): boolean { return this.hostLoopEnabled; }

  // ====== DSP パラメータ → WASM 直送 ======

  setHostGain(db: number): void { this.workletNode?.port.postMessage({ type: 'set-param', param: 'host_gain', value: db }); }
  setPlaylistGain(db: number): void { this.workletNode?.port.postMessage({ type: 'set-param', param: 'playlist_gain', value: db }); }
  setLpfEnabled(enabled: boolean): void { this.workletNode?.port.postMessage({ type: 'set-param', param: 'lpf_enabled', value: enabled }); }
  setLpfFrequency(hz: number): void { this.workletNode?.port.postMessage({ type: 'set-param', param: 'lpf_frequency', value: hz }); }
  setMeteringMode(mode: number): void { this.workletNode?.port.postMessage({ type: 'set-param', param: 'metering_mode', value: mode }); }
  resetMeteringMomentary(): void { this.workletNode?.port.postMessage({ type: 'set-param', param: 'reset_momentary', value: true }); }

  setSourceSelect(source: number): void { this.workletNode?.port.postMessage({ type: 'set-source', value: source }); }

  // ====== Host サンプル（C++ WASM に PCM を送信） ======

  async loadHostSample(url: string): Promise<void> {
    if (!this.audioContext) return;
    try {
      const r = await fetch(url);
      if (!r.ok) return;
      const audioBuf = await this.audioContext.decodeAudioData(await r.arrayBuffer());
      const name = url.split('/').pop() || url;
      this.sendHostBufferToWasm(audioBuf, name);
    } catch { /* ignore */ }
  }

  // ユーザがアップロードしたファイルを HOST 入力に差し替える（Web デモ専用）。
  async loadHostFromFile(file: File): Promise<void> {
    if (!this.audioContext) return;
    try {
      await this.ensureAudioContext();
      const audioBuf = await this.audioContext.decodeAudioData(await file.arrayBuffer());
      this.sendHostBufferToWasm(audioBuf, file.name);
    } catch (err) {
      this.emit('errorNotification', {
        severity: 'error',
        message: `Decode failed: ${file.name}`,
        details: String(err),
      });
    }
  }

  private sendHostBufferToWasm(audioBuf: AudioBuffer, displayName: string): void {
    const leftCopy = new Float32Array(audioBuf.getChannelData(0));
    const rightCopy = new Float32Array(
      audioBuf.numberOfChannels >= 2 ? audioBuf.getChannelData(1) : audioBuf.getChannelData(0)
    );
    this.workletNode?.port.postMessage({
      type: 'load-host-track',
      left: leftCopy.buffer, right: rightCopy.buffer,
      numSamples: leftCopy.length, trackSampleRate: audioBuf.sampleRate,
    }, [leftCopy.buffer, rightCopy.buffer]);
    this.currentHostSource = { name: displayName, duration: audioBuf.duration };
    this.emit('sourceLoaded', { name: displayName, duration: audioBuf.duration });
  }

  // バー側で初期状態を一発取得するため（イベント取りこぼし対策）。
  getCurrentHostSource(): { name: string; duration: number } | null {
    return this.currentHostSource;
  }

  // ====== 状態取得 ======

  getTracks(): TrackInfo[] { return [...this.tracks]; }
  getCurrentTrackIndex(): number { return this.currentTrackIndex; }
  getTrackCount(): number { return this.tracks.length; }
  getIsPlaying(): boolean { return this.isPlaying; }
  getPosition(): number { return this.position; }
  getDuration(): number { return this.duration; }

  // ====== イベント発行 ======

  private emitPlaylistUpdate(): void {
    this.emit('playlistUpdate', {
      items: this.tracks.map(t => ({ id: t.id, name: t.name, duration: t.duration, isLoaded: t.isLoaded, exists: t.exists })),
      currentIndex: this.currentTrackIndex,
    });
  }

  private emitTransportUpdate(): void {
    // position を含めない → Transport.tsx の seekPending チェックを回避し、
    // loopEnabled 等の状態変更が巻き添えで拒否されるのを防ぐ。
    // position は C++ worklet の ~20Hz state-update で別途配信される。
    this.emit('transportUpdate', {
      isPlaying: this.isPlaying,
      loopStart: this.loopStart,
      loopEnd: this.loopEnd,
      loopEnabled: this.loopEnabled,
      duration: this.duration,
    });
  }

  private emitTrackChange(): void {
    this.emit('trackChange', {
      items: this.tracks.map(t => ({ id: t.id, name: t.name, duration: t.duration, isLoaded: t.isLoaded, exists: t.exists })),
      currentIndex: this.currentTrackIndex,
      isPlaying: this.isPlaying,
      position: 0,
      duration: this.duration,
      loopEnabled: this.loopEnabled,
      loopStart: this.loopStart,
      loopEnd: this.loopEnd,
    });
  }
}

export const webAudioEngine = new WebAudioEngine();
