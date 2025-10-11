import React, { useState, useEffect, useRef } from 'react';
import { Box, Typography, Slider, Switch, FormControlLabel, Input } from '@mui/material';
import { getSliderState, getToggleState } from 'juce-framework-frontend-mirror';

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

  // LPFスライダー用のネイティブ wheel リスナー（passive: false）
  useEffect(() => {
    const el = lpfWheelAreaRef.current;
    if (!el) return;
    const handleWheelNative = (event: WheelEvent) => {
      // フィルターOFF時でも編集可能にするため、早期 return はしない
      event.preventDefault();
      const delta = -event.deltaY;
      const direction = delta > 0 ? 1 : -1;
      // 現在の周波数値（小数点以下を丸める）
      const currentFreq = Math.round(lpfFreqRef.current);
      let newFreq: number;

      // 周波数帯域に応じた刻み幅で調整
      if (currentFreq <= 300) {
        // 20Hz～300Hz: 10Hz刻み
        if (event.shiftKey) {
          // Shift: ±1Hz（細かい調整）
          newFreq = currentFreq + direction;
        } else {
          // 通常: ±10Hz
          newFreq = currentFreq + 10 * direction;
          newFreq = Math.round(newFreq / 10) * 10; // 10Hz単位に丸める

          // 境界処理: 300を超えたら次の帯域の刻み幅にスナップ
          if (newFreq > 300 && direction > 0) {
            newFreq = 320; // 300の次は320（20Hz刻みの最初）
          }
        }
      } else if (currentFreq <= 500) {
        // 300Hz超～500Hz: 20Hz刻み
        if (event.shiftKey) {
          // Shift: ±2Hz（細かい調整）
          newFreq = currentFreq + 2 * direction;
        } else {
          // 通常: ±20Hz
          newFreq = currentFreq + 20 * direction;
          newFreq = Math.round(newFreq / 20) * 20; // 20Hz単位に丸める

          // 境界処理
          if (newFreq <= 300 && direction < 0) {
            newFreq = 300; // 300以下になったら300でスナップ
          } else if (newFreq > 500 && direction > 0) {
            newFreq = 550; // 500の次は550（50Hz刻みの最初）
          }
        }
      } else if (currentFreq <= 1000) {
        // 500Hz超～1000Hz: 50Hz刻み
        if (event.shiftKey) {
          // Shift: ±5Hz（細かい調整）
          newFreq = currentFreq + 5 * direction;
          newFreq = Math.round(newFreq / 5) * 5; // 5Hz単位に丸める
        } else {
          // 通常: ±50Hz
          newFreq = currentFreq + 50 * direction;
          newFreq = Math.round(newFreq / 50) * 50; // 50Hz単位に丸める

          // 境界処理
          if (newFreq <= 500 && direction < 0) {
            newFreq = 500; // 500以下になったら500でスナップ
          } else if (newFreq > 1000 && direction > 0) {
            newFreq = 1100; // 1000の次は1100（100Hz刻みの最初）
          }
        }
      } else if (currentFreq <= 2000) {
        // 1000Hz超～2000Hz: 100Hz刻み
        if (event.shiftKey) {
          // Shift: ±10Hz（細かい調整）
          newFreq = currentFreq + 10 * direction;
          newFreq = Math.round(newFreq / 10) * 10; // 10Hz単位に丸める
        } else {
          // 通常: ±100Hz
          newFreq = currentFreq + 100 * direction;
          newFreq = Math.round(newFreq / 100) * 100; // 100Hz単位に丸める

          // 境界処理
          if (newFreq <= 1000 && direction < 0) {
            newFreq = 1000; // 1000以下になったら1000でスナップ
          } else if (newFreq > 2000 && direction > 0) {
            newFreq = 2500; // 2000の次は2500（500Hz刻みの最初）
          }
        }
      } else if (currentFreq <= 5000) {
        // 2000Hz超～5000Hz: 500Hz刻み
        if (event.shiftKey) {
          // Shift: ±50Hz（細かい調整）
          newFreq = currentFreq + 50 * direction;
          newFreq = Math.round(newFreq / 50) * 50; // 50Hz単位に丸める
        } else {
          // 通常: ±500Hz
          newFreq = currentFreq + 500 * direction;
          newFreq = Math.round(newFreq / 500) * 500; // 500Hz単位に丸める

          // 境界処理
          if (newFreq <= 2000 && direction < 0) {
            newFreq = 2000; // 2000以下になったら2000でスナップ
          } else if (newFreq > 5000 && direction > 0) {
            newFreq = 6000; // 5000の次は6000（1000Hz刻みの最初）
          }
        }
      } else {
        // 5000Hz超～20kHz: 1000Hz刻み
        if (event.shiftKey) {
          // Shift: ±100Hz（細かい調整）
          newFreq = currentFreq + 100 * direction;
          newFreq = Math.round(newFreq / 100) * 100; // 100Hz単位に丸める
        } else {
          // 通常: ±1000Hz
          newFreq = currentFreq + 1000 * direction;
          newFreq = Math.round(newFreq / 1000) * 1000; // 1000Hz単位に丸める

          // 境界処理
          if (newFreq <= 5000 && direction < 0) {
            newFreq = 5000; // 5000以下になったら5000でスナップ
          }
        }
      }

      // 範囲制限（20Hz～20000Hz）
      newFreq = Math.max(20, Math.min(20000, newFreq));

      // 周波数を設定
      setLpfFreq(newFreq);
      // JUCE 側へは線形正規化で送信
      const normLinear = (newFreq - 20) / (20000 - 20);
      sliderState?.setNormalisedValue(Math.max(0, Math.min(1, normLinear)));
    };
    el.addEventListener('wheel', handleWheelNative, { passive: false });
    return () => {
      el.removeEventListener('wheel', handleWheelNative as EventListener);
    };
  }, [sliderState]);

  // LPFスライダーのCtrl/Cmd+クリックでリセット
  const handleLpfClick = (event: React.MouseEvent) => {
    if (event.ctrlKey || event.metaKey) {
      event.preventDefault();
      event.stopPropagation();

      // デフォルト値（120Hz）にリセット
      const defaultFreq = 120;
      setLpfFreq(defaultFreq);
      const normLinear = (defaultFreq - 20) / (20000 - 20);
      sliderState?.setNormalisedValue(Math.max(0, Math.min(1, normLinear)));
    }
  };

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
      <Box sx={{ px: 1 }} ref={lpfWheelAreaRef}>
        {/* フィルターOFF時でも周波数スライダーを操作可能にするため、disabledは設定しない */}
        <Slider
          value={freqToSlider(lpfFreq)}
          onChange={handleLpfFreqChange}
          // Ctrl/Cmd+クリックでの即時リセットと、通常ドラッグの開始を両立
          onMouseDown={(e) => {
            // Ctrl/Cmd+クリック時はリセットのみ（ドラッグ開始は送らない）
            if (e.ctrlKey || e.metaKey) {
              handleLpfClick(e);
              return;
            }
            // 通常ドラッグ開始: JUCE へ開始ジェスチャーを送る
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
