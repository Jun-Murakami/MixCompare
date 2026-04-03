/**
 * MixCompare DSP AudioWorkletProcessor — 薄いラッパー版
 *
 * 全てのオーディオ処理（PCM 再生・トランスポート・ゲイン・LPF・メータ）は
 * C++ WASM の dsp_process_block() が一括実行する。
 * この JS は WASM 呼び出しとメインスレッドへの通知だけを担当。
 */

class DspProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.wasm = null;
    this.wasmReady = false;
    this.wasmMemory = null;

    // WASM ヒープポインタ
    this.outLPtr = 0;
    this.outRPtr = 0;
    this.meterBufPtr = 0; // 24 floats for meter data

    // メータ/位置の送信間隔
    this.updateCounter = 0;

    this.port.onmessage = (e) => this.handleMessage(e.data);
  }

  handleMessage(msg) {
    switch (msg.type) {
      case 'init-wasm':
        this.initWasm(msg.wasmBytes);
        break;

      case 'load-track': {
        if (!this.wasm) break;
        const { index, left, right, numSamples, trackSampleRate } = msg;
        const leftArr = new Float32Array(left);
        const rightArr = new Float32Array(right);

        // WASM ヒープにコピー
        const lPtr = this.wasm.dsp_alloc_buffer(numSamples);
        const rPtr = this.wasm.dsp_alloc_buffer(numSamples);
        const heap = new Float32Array(this.wasmMemory.buffer);
        heap.set(leftArr, lPtr / 4);
        heap.set(rightArr, rPtr / 4);

        // C++ に登録（内部でコピーされる）
        this.wasm.dsp_load_track(index, lPtr, rPtr, numSamples, trackSampleRate);

        // 一時バッファ解放
        this.wasm.dsp_free_buffer(lPtr);
        this.wasm.dsp_free_buffer(rPtr);
        break;
      }

      case 'remove-track':
        if (this.wasm) this.wasm.dsp_remove_track(msg.index);
        break;

      case 'clear-tracks':
        if (this.wasm) this.wasm.dsp_clear_tracks();
        break;

      case 'select-track':
        if (this.wasm) this.wasm.dsp_select_track(msg.index);
        break;

      case 'set-playing':
        if (this.wasm) this.wasm.dsp_set_playing(msg.value ? 1 : 0);
        break;

      case 'seek':
        if (this.wasm) this.wasm.dsp_seek(msg.position);
        break;

      case 'set-loop':
        if (this.wasm) this.wasm.dsp_set_loop(msg.enabled ? 1 : 0, msg.start, msg.end);
        break;

      case 'load-host-track': {
        if (!this.wasm) break;
        const left = new Float32Array(msg.left);
        const right = new Float32Array(msg.right);
        const lP = this.wasm.dsp_alloc_buffer(msg.numSamples);
        const rP = this.wasm.dsp_alloc_buffer(msg.numSamples);
        const h = new Float32Array(this.wasmMemory.buffer);
        h.set(left, lP / 4);
        h.set(right, rP / 4);
        this.wasm.dsp_load_host_track(lP, rP, msg.numSamples, msg.trackSampleRate);
        this.wasm.dsp_free_buffer(lP);
        this.wasm.dsp_free_buffer(rP);
        break;
      }

      case 'set-source':
        if (this.wasm) this.wasm.dsp_set_source_select(msg.value);
        break;

      case 'set-param':
        if (!this.wasm) break;
        if (msg.param === 'host_gain') this.wasm.dsp_set_host_gain(msg.value);
        else if (msg.param === 'playlist_gain') this.wasm.dsp_set_playlist_gain(msg.value);
        else if (msg.param === 'lpf_enabled') this.wasm.dsp_set_lpf_enabled(msg.value ? 1 : 0);
        else if (msg.param === 'lpf_frequency') this.wasm.dsp_set_lpf_frequency(msg.value);
        else if (msg.param === 'metering_mode') this.wasm.dsp_set_metering_mode(msg.value);
        else if (msg.param === 'reset_true_peak') this.wasm.dsp_reset_true_peak();
        else if (msg.param === 'reset_momentary') this.wasm.dsp_reset_momentary_hold();
        break;
    }
  }

  async initWasm(wasmBytes) {
    try {
      const module = await WebAssembly.compile(wasmBytes);
      const importObject = {
        env: { emscripten_notify_memory_growth: () => {} },
      };
      const instance = await WebAssembly.instantiate(module, importObject);
      if (instance.exports._initialize) instance.exports._initialize();

      this.wasm = instance.exports;
      this.wasmMemory = instance.exports.memory;

      // DSP 初期化
      this.wasm.dsp_init(sampleRate, 128);

      // 出力バッファ + メーターバッファ確保
      this.outLPtr = this.wasm.dsp_alloc_buffer(128);
      this.outRPtr = this.wasm.dsp_alloc_buffer(128);
      this.meterBufPtr = this.wasm.dsp_alloc_buffer(24);

      this.wasmReady = true;
      this.port.postMessage({ type: 'wasm-ready' });
    } catch (err) {
      this.port.postMessage({ type: 'wasm-error', error: String(err) });
    }
  }

  process(inputs, outputs) {
    if (!this.wasmReady) return true;

    const output = outputs[0];
    if (!output || output.length < 2) return true;
    const outL = output[0];
    const outR = output[1];
    const numSamples = outL.length;

    // C++ に全処理を委譲
    this.wasm.dsp_process_block(this.outLPtr, this.outRPtr, numSamples);

    // WASM ヒープから出力をコピー
    const heap = new Float32Array(this.wasmMemory.buffer);
    outL.set(heap.subarray(this.outLPtr / 4, this.outLPtr / 4 + numSamples));
    outR.set(heap.subarray(this.outRPtr / 4, this.outRPtr / 4 + numSamples));

    // ~20Hz でメータ・位置・状態をメインスレッドに通知
    const interval = Math.max(1, Math.round(sampleRate / (numSamples * 20)));
    if (++this.updateCounter >= interval) {
      this.updateCounter = 0;

      const stoppedAtEnd = this.wasm.dsp_consume_stopped_at_end();

      // C++ から dB 変換済みメーター値を一括取得
      this.wasm.dsp_get_meter_data(this.meterBufPtr);
      const mh = new Float32Array(this.wasmMemory.buffer);
      const mo = this.meterBufPtr / 4;
      // レイアウト: [0]=mode, [1..8]=host, [9..16]=playlist, [17..20]=output

      this.port.postMessage({
        type: 'state-update',
        position: this.wasm.dsp_get_position(),
        duration: this.wasm.dsp_get_duration(),
        isPlaying: !!this.wasm.dsp_is_playing(),
        stoppedAtEnd: !!stoppedAtEnd,
        meteringMode: mh[mo],
        host: {
          left: mh[mo+1], right: mh[mo+2],
          truePeakLeft: mh[mo+3], truePeakRight: mh[mo+4],
          rmsLeft: mh[mo+5], rmsRight: mh[mo+6],
          momentary: mh[mo+7], momentaryHold: mh[mo+8],
        },
        playlist: {
          left: mh[mo+9], right: mh[mo+10],
          truePeakLeft: mh[mo+11], truePeakRight: mh[mo+12],
          rmsLeft: mh[mo+13], rmsRight: mh[mo+14],
          momentary: mh[mo+15], momentaryHold: mh[mo+16],
        },
        output: {
          left: mh[mo+17], right: mh[mo+18],
          momentary: mh[mo+19], momentaryHold: mh[mo+20],
        },
      });
    }

    return true;
  }
}

registerProcessor('dsp-processor', DspProcessor);
