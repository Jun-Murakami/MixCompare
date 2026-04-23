import React, { useState, useEffect, useRef } from 'react';
import { Box, Typography, Slider, Switch, FormControlLabel, Input } from '@mui/material';
import { getSliderState, getToggleState } from 'juce-framework-frontend-mirror';
import { useFineAdjustPointer } from '../hooks/useFineAdjustPointer';
import { useNumberInputAdjust } from '../hooks/useNumberInputAdjust';

export const Controls: React.FC = () => {
  // JUCE 直接バインディング（LPF周り）
  const sliderState = getSliderState('LPF_FREQ');
  const toggleState = getToggleState('LPF_ENABLED');

  // ローカル表示用状態（Hz と 有効フラグ）
  const [lpfFreq, setLpfFreq] = useState<number>(() => (sliderState ? sliderState.getScaledValue() : 20000));
  const [lpfEnabled, setLpfEnabled] = useState<boolean>(() => (toggleState ? toggleState.getValue() : false));
  const [inputValue, setInputValue] = useState<string>('');
  const [isEditing, setIsEditing] = useState(false);
  // JUCE ジェスチャーの整合を保つためのドラッグ状態フラグ
  const [isDragging, setIsDragging] = useState(false);
  // ホイール操作で preventDefault を確実に効かせるため、非パッシブなネイティブ wheel リスナーを使う
  const lpfWheelAreaRef = useRef<HTMLDivElement | null>(null);
  // ホイール中に最新の周波数へアクセスするための参照
  const lpfFreqRef = useRef<number>(lpfFreq);
  useEffect(() => {
    lpfFreqRef.current = lpfFreq;
  }, [lpfFreq]);

  // 0-1のスライダー位置から周波数への変換（中央500Hz）
  const sliderToFreq = (sliderValue: number): number => {
    const minLog = Math.log(20);
    const midLog = Math.log(500); // 中央を500Hzに変更
    const maxLog = Math.log(20000);

    if (sliderValue <= 0.5) {
      // 0.0～0.5: 20Hz～500Hz
      const freqLog = minLog + sliderValue * 2 * (midLog - minLog);
      return Math.exp(freqLog);
    } else {
      // 0.5～1.0: 500Hz～20kHz
      const freqLog = midLog + (sliderValue - 0.5) * 2 * (maxLog - midLog);
      return Math.exp(freqLog);
    }
  };

  // 周波数から0-1のスライダー位置への変換（中央500Hz）
  const freqToSlider = (freq: number): number => {
    const minLog = Math.log(20);
    const midLog = Math.log(500); // 中央を500Hzに変更
    const maxLog = Math.log(20000);
    const freqLog = Math.log(freq);

    if (freq <= 500) {
      // 20Hz～500Hz: 0.0～0.5
      return (0.5 * (freqLog - minLog)) / (midLog - minLog);
    } else {
      // 500Hz～20kHz: 0.5～1.0
      return 0.5 + (0.5 * (freqLog - midLog)) / (maxLog - midLog);
    }
  };

  const handleLpfFreqChange = (_: Event, value: number | number[]) => {
    const sliderValue = value as number; // 0..1（UIのカスタム対数スケール位置）
    const freq = sliderToFreq(sliderValue);
    setLpfFreq(freq);
    // JUCE パラメータへは線形正規化で反映（20Hz..20kHz）
    const normLinear = (freq - 20) / (20000 - 20);
    sliderState?.setNormalisedValue(Math.max(0, Math.min(1, normLinear)));
  };

  // MUI Slider のコミット時にドラッグ終了を通知（開始している場合のみ）
  const handleLpfChangeCommitted = (_: Event | React.SyntheticEvent, value: number | number[]) => {
    // 最終値も JUCE 側に反映しておく（線形正規化で再送）
    const sliderValue = value as number;
    const freq = sliderToFreq(sliderValue);
    const normLinear = (freq - 20) / (20000 - 20);
    sliderState?.setNormalisedValue(Math.max(0, Math.min(1, normLinear)));
    if (isDragging) {
      setIsDragging(false);
      sliderState?.sliderDragEnded();
    }
  };

  const handleLpfEnabledChange = (event: React.ChangeEvent<HTMLInputElement>) => {
    const enabled = event.target.checked;
    setLpfEnabled(enabled);
    toggleState?.setValue(enabled);
  };

  // 共通の値適用（UI と JUCE へ反映）
  const applyFreq = (freq: number) => {
    const clamped = Math.max(20, Math.min(20000, freq));
    setLpfFreq(clamped);
    const normLinear = (clamped - 20) / (20000 - 20);
    sliderState?.setNormalisedValue(Math.max(0, Math.min(1, normLinear)));
  };

  // 周波数帯域に応じた wheel 1tick の増分を算出。fine フラグで 1/10 刻みへ。
  //  境界をまたぐ場合は次の帯域の最小刻みへスナップして自然な段階感を出す。
  const wheelNextFreq = (current: number, direction: 1 | -1, fine: boolean): number => {
    const round = (v: number, unit: number) => Math.round(v / unit) * unit;
    if (current <= 300) {
      if (fine) return current + direction;
      const next = round(current + 10 * direction, 10);
      return next > 300 && direction > 0 ? 320 : next;
    }
    if (current <= 500) {
      if (fine) return current + 2 * direction;
      const next = round(current + 20 * direction, 20);
      if (next <= 300 && direction < 0) return 300;
      if (next > 500 && direction > 0) return 550;
      return next;
    }
    if (current <= 1000) {
      if (fine) return round(current + 5 * direction, 5);
      const next = round(current + 50 * direction, 50);
      if (next <= 500 && direction < 0) return 500;
      if (next > 1000 && direction > 0) return 1100;
      return next;
    }
    if (current <= 2000) {
      if (fine) return round(current + 10 * direction, 10);
      const next = round(current + 100 * direction, 100);
      if (next <= 1000 && direction < 0) return 1000;
      if (next > 2000 && direction > 0) return 2500;
      return next;
    }
    if (current <= 5000) {
      if (fine) return round(current + 50 * direction, 50);
      const next = round(current + 500 * direction, 500);
      if (next <= 2000 && direction < 0) return 2000;
      if (next > 5000 && direction > 0) return 6000;
      return next;
    }
    if (fine) return round(current + 100 * direction, 100);
    const next = round(current + 1000 * direction, 1000);
    if (next <= 5000 && direction < 0) return 5000;
    return next;
  };

  // LPFスライダー用のネイティブ wheel リスナー（passive: false）
  useEffect(() => {
    const el = lpfWheelAreaRef.current;
    if (!el) return;
    const handleWheelNative = (event: WheelEvent) => {
      event.preventDefault();
      const current = Math.round(lpfFreqRef.current);
      const direction: 1 | -1 = -event.deltaY > 0 ? 1 : -1;
      const fine = event.shiftKey || event.ctrlKey || event.metaKey || event.altKey;
      applyFreq(wheelNextFreq(current, direction, fine));
    };
    el.addEventListener('wheel', handleWheelNative, { passive: false });
    return () => {
      el.removeEventListener('wheel', handleWheelNative as EventListener);
    };
  }, [sliderState]);

  // 修飾キー + ポインタ操作：
  //  Ctrl/Cmd + クリック      → デフォルト値 120 Hz へリセット
  //  (Ctrl/Cmd/Shift) + ドラッグ → 微調整モード（log 空間で 1px = 0.0015 norm）
  //  修飾キーなし              → MUI Slider の通常ドラッグに委譲
  //
  // スライダー本体は 0..1 のカスタム log 位置にマップされているため、
  // 元の位置を freqToSlider で取得して pixel delta を足し、sliderToFreq で値域へ戻す。
  const fineDragStartSliderRef = useRef<number>(0);
  const handleLpfPointerDownCapture = useFineAdjustPointer({
    orientation: 'horizontal',
    onReset: () => applyFreq(120),
    onDragStart: () => {
      fineDragStartSliderRef.current = freqToSlider(lpfFreqRef.current);
      sliderState?.sliderDragStarted();
    },
    onDragDelta: (deltaPx) => {
      // 1px = 0.0015（スライダー 0..1 の正規化位置）。log 軸全域（20Hz..20kHz）を
      // 約 660px で横断。微調整の意図で丁度良い感度。
      const newSliderPos = Math.max(0, Math.min(1, fineDragStartSliderRef.current + deltaPx * 0.0015));
      applyFreq(sliderToFreq(newSliderPos));
    },
    onDragEnd: () => sliderState?.sliderDragEnded(),
  });

  // 数値入力欄（Hz）のホイール / 縦ドラッグ
  const inputElRef = useRef<HTMLInputElement | null>(null);
  const inputDragStartSliderRef = useRef<number>(0);
  useNumberInputAdjust(inputElRef, {
    onWheelStep: (direction, fine) => {
      const current = Math.round(lpfFreqRef.current);
      applyFreq(wheelNextFreq(current, direction, fine));
    },
    onDragStart: () => {
      inputDragStartSliderRef.current = freqToSlider(lpfFreqRef.current);
      sliderState?.sliderDragStarted();
    },
    onDragDelta: (deltaY, fine) => {
      const step = fine ? 0.0015 : 0.005;
      const newSliderPos = Math.max(0, Math.min(1, inputDragStartSliderRef.current + deltaY * step));
      applyFreq(sliderToFreq(newSliderPos));
    },
    onDragEnd: () => sliderState?.sliderDragEnded(),
  });

  // 入力値の初期化と同期
  useEffect(() => {
    if (!isEditing) {
      if (lpfFreq >= 1000) {
        const kValue = Math.floor(lpfFreq / 100) / 10;
        setInputValue(kValue.toFixed(1) + 'k');
      } else {
        setInputValue(Math.floor(lpfFreq).toString());
      }
    }
  }, [lpfFreq, isEditing]);

  // JUCE からの値更新を購読してローカル表示へ反映
  useEffect(() => {
    if (sliderState) {
      const vId = sliderState.valueChangedEvent.addListener(() => {
        // JUCE パラメータのスケール値（Hz）をそのまま表示に反映
        setLpfFreq(sliderState.getScaledValue());
      });
      return () => {
        sliderState.valueChangedEvent.removeListener(vId);
      };
    }
  }, [sliderState]);

  useEffect(() => {
    if (toggleState) {
      const vId = toggleState.valueChangedEvent.addListener(() => {
        setLpfEnabled(toggleState.getValue());
      });
      return () => {
        toggleState.valueChangedEvent.removeListener(vId);
      };
    }
  }, [toggleState]);

  // 数値入力のハンドラー
  const handleInputChange = (event: React.ChangeEvent<HTMLInputElement>) => {
    setInputValue(event.target.value);
  };

  // 入力確定時の処理
  const handleInputBlur = () => {
    setIsEditing(false);

    // 入力値をパース（kHz表記も対応）
    let value = inputValue.trim().toLowerCase();
    let freq: number;

    if (value.endsWith('k') || value.endsWith('khz')) {
      // kHz表記の場合
      value = value.replace(/khz?$/, '');
      freq = parseFloat(value) * 1000;
    } else if (value.endsWith('hz')) {
      // Hz表記の場合
      value = value.replace(/hz$/, '');
      freq = parseFloat(value);
    } else {
      // 数値のみの場合はHzとして扱う
      freq = parseFloat(value);
    }

    // 有効な範囲にクランプ + JUCEへ反映
    if (!isNaN(freq)) {
      freq = Math.max(20, Math.min(20000, freq));
      setLpfFreq(freq);
      const normLinear = (freq - 20) / (20000 - 20);
      sliderState?.setNormalisedValue(Math.max(0, Math.min(1, normLinear)));
    }

    // 表示を更新（Hz単位は別表示なので数値のみ）
    if (lpfFreq >= 1000) {
      const kValue = Math.floor(lpfFreq / 100) / 10;
      setInputValue(kValue.toFixed(1) + 'k');
    } else {
      setInputValue(Math.floor(lpfFreq).toString());
    }
  };

  // Enterキーで確定
  const handleInputKeyDown = (event: React.KeyboardEvent<HTMLInputElement>) => {
    if (event.key === 'Enter') {
      (event.target as HTMLInputElement).blur();
    }
  };

  // フォーカス時の処理
  const handleInputFocus = () => {
    setIsEditing(true);
    // フォーカス時は数値のみを表示（単位なし）
    setInputValue(Math.round(lpfFreq).toString());
  };

  return (
    // 横幅は親の余白に追従。最小は従来の見た目を保つため 360px。
    <Box
      sx={{
        width: '100%',
        minWidth: 360,
        maxWidth: '100%',
        mt: 2,
        border: '1px solid',
        borderColor: 'text.secondary',
        borderRadius: 1,
        p: 1,
      }}
    >
      <Box sx={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
        <FormControlLabel
          control={<Switch checked={lpfEnabled} onChange={handleLpfEnabledChange} size='small' />}
          label='Low Pass Filter'
          sx={{
            m: 0,
            color: lpfEnabled ? 'text.primary' : 'text.secondary',
            '& .MuiFormControlLabel-label': { fontSize: '0.875rem' },
          }}
        />
        <Box sx={{ display: 'flex', alignItems: 'center' }}>
          {/* フィルターOFF時でも編集可能にするため、disabledは設定しない */}
          <Input
            className='block-host-shortcuts'
            inputRef={inputElRef}
            value={inputValue}
            onChange={handleInputChange}
            onBlur={handleInputBlur}
            onFocus={handleInputFocus}
            onKeyDown={handleInputKeyDown}
            size='small'
            sx={{
              width: 60,
              fontFamily: '"Red Hat Mono", monospace',
              fontSize: '0.875rem',
              '& input': {
                textAlign: 'right',
                padding: '2px 4px',
              },
              '&:before': {
                borderBottom: 'none',
              },
              '&:hover:not(.Mui-disabled):before': {
                borderBottom: '1px solid rgba(255, 255, 255, 0.42)',
              },
              '&:after': {
                borderBottom: '1px solid',
                borderColor: 'primary.main',
              },
            }}
          />
          <Typography
            variant='body2'
            sx={{
              ml: 0.5,
            }}
          >
            Hz
          </Typography>
        </Box>
      </Box>
      {/* スライダーは枠の内側いっぱいに広がる */}
      <Box sx={{ px: 1 }} ref={lpfWheelAreaRef} onPointerDownCapture={handleLpfPointerDownCapture}>
        {/* フィルターOFF時でも周波数スライダーを操作可能にするため、disabledは設定しない */}
        <Slider
          value={freqToSlider(lpfFreq)}
          onChange={handleLpfFreqChange}
          onMouseDown={() => {
            if (!isDragging) {
              setIsDragging(true);
              sliderState?.sliderDragStarted();
            }
          }}
          onChangeCommitted={handleLpfChangeCommitted}
          min={0}
          max={1}
          step={0.001}
          valueLabelDisplay='off'
          sx={{
            // シークバーと同様のビジュアルに統一
            // - サム（thumb）はホバー/フォーカス/ドラッグ時のみ表示
            // - レール/トラックの高さ調整
            // - 余白もシークバーに合わせて微調整
            mb: -0.9,
            height: 6,
            '& .MuiSlider-thumb': {
              width: 12,
              height: 12,
              transition: 'opacity 80ms',
              opacity: 0,
            },
            '&:hover .MuiSlider-thumb, & .MuiSlider-thumb.Mui-focusVisible, & .MuiSlider-thumb.Mui-active': {
              opacity: 1,
            },
            '& .MuiSlider-track': { height: 3, transition: 'none' },
            '& .MuiSlider-rail': { height: 3, opacity: 0.5 },
            // 目盛りラベルは従来通りの小さい表示を維持
            '& .MuiSlider-markLabel': { fontSize: '0.7rem', mt: -1 },
          }}
          marks={[
            { value: 0, label: '20Hz' },
            { value: freqToSlider(100), label: '100' },
            { value: 0.5, label: '500' }, // 中央が500Hz
            { value: freqToSlider(2000), label: '2k' },
            { value: freqToSlider(10000), label: '10k' },
            { value: 1, label: '20k' },
          ]}
        />
      </Box>
    </Box>
  );
};
