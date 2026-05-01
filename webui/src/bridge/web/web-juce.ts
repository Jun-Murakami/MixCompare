// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
/**
 * bridge/juce.ts のドロップイン置換。
 * Vite エイリアスでコンポーネントの `import { juceBridge } from '../bridge/juce'` が
 * このファイルに解決される。
 */

export { webBridge as juceBridge } from './WebBridgeManager';

// openUrl も互換で提供
export async function openUrl(url: string): Promise<boolean> {
  window.open(url, '_blank');
  return true;
}
