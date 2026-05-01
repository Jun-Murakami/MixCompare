// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
/**
 * juce-framework-frontend-mirror の SliderState / ToggleState / ComboBoxState の
 * Web 互換実装。JUCE WebView が無い環境でも同じインターフェースで動作する。
 */

type ListenerFn = () => void;

class SimpleEventEmitter {
  private listeners = new Map<number, ListenerFn>();
  private nextId = 1;

  addListener(fn: ListenerFn): number {
    const id = this.nextId++;
    this.listeners.set(id, fn);
    return id;
  }

  removeListener(id: number): void {
    this.listeners.delete(id);
  }

  emit(): void {
    this.listeners.forEach((fn) => fn());
  }
}

// ---------- SliderState ----------

export interface WebSliderStateOptions {
  defaultValue?: number; // normalised 0..1
  min?: number;
  max?: number;
  /** dB/Hz 等のスケール値を正規化値から変換する関数 */
  toScaled?: (norm: number) => number;
  /** スケール値を正規化値に変換する関数 */
  fromScaled?: (scaled: number) => number;
}

export class WebSliderState {
  private normValue: number;
  private min: number;
  private max: number;
  private toScaled: (n: number) => number;
  private fromScaled: (s: number) => number;
  public readonly valueChangedEvent = new SimpleEventEmitter();

  constructor(opts: WebSliderStateOptions = {}) {
    this.normValue = opts.defaultValue ?? 0.5;
    this.min = opts.min ?? 0;
    this.max = opts.max ?? 1;
    this.toScaled =
      opts.toScaled ?? ((n: number) => this.min + n * (this.max - this.min));
    this.fromScaled =
      opts.fromScaled ?? ((s: number) => (s - this.min) / (this.max - this.min));
  }

  getNormalisedValue(): number {
    return this.normValue;
  }

  setNormalisedValue(norm: number): void {
    this.normValue = Math.max(0, Math.min(1, norm));
    this.valueChangedEvent.emit();
  }

  getScaledValue(): number {
    return this.toScaled(this.normValue);
  }

  setScaledValue(scaled: number): void {
    this.normValue = Math.max(0, Math.min(1, this.fromScaled(scaled)));
    this.valueChangedEvent.emit();
  }

  /** JUCE 互換スタブ */
  sliderDragStarted(): void {}
  sliderDragEnded(): void {}
}

// ---------- ToggleState ----------

export class WebToggleState {
  private value: boolean;
  public readonly valueChangedEvent = new SimpleEventEmitter();

  constructor(initial = false) {
    this.value = initial;
  }

  getValue(): boolean {
    return this.value;
  }

  setValue(v: boolean): void {
    this.value = v;
    this.valueChangedEvent.emit();
  }
}

// ---------- ComboBoxState ----------

export class WebComboBoxState {
  private index: number;
  private numItems: number;
  public readonly valueChangedEvent = new SimpleEventEmitter();

  constructor(initial = 0, numItems = 2) {
    this.index = initial;
    this.numItems = numItems;
  }

  getChoiceIndex(): number {
    return this.index;
  }

  setChoiceIndex(ix: number): void {
    this.index = Math.max(0, Math.min(this.numItems - 1, ix));
    this.valueChangedEvent.emit();
  }

  getNumItems(): number {
    return this.numItems;
  }
}
