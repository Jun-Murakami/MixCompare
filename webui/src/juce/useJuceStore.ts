// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
import { useCallback, useSyncExternalStore } from 'react';

// JUCE の State が公開する ListenerList の構造的型（型自体は package から export されない）
export type JuceListenerList = {
  addListener: (fn: () => void) => number;
  removeListener: (id: number) => void;
};

// JUCE の各 State（外部ストア）を useSyncExternalStore で購読する。
// エフェクト内で同期的に setState しないため set-state-in-effect を回避でき、
// State が後から解決される（初期 undefined → 実体）ケースにも追従する。
export function useJuceStore<T>(event: JuceListenerList | undefined, read: () => T): T {
  const subscribe = useCallback(
    (onChange: () => void) => {
      if (!event) return () => {};
      const id = event.addListener(onChange);
      return () => event.removeListener(id);
    },
    [event]
  );
  return useSyncExternalStore(subscribe, read);
}
