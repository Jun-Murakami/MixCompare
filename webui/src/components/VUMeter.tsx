import React, { useEffect, useRef, useState } from 'react';
import { Box, Typography, Tooltip, Button } from '@mui/material';
import { juceBridge } from '../bridge/juce';
import { GainFader } from './GainFader';
import { getComboBoxState } from 'juce-framework-frontend-mirror';
import { type MeterLevels, type MeterUpdateData } from '../types';

// メーターの表示スケールと寸法に関する定数
// 画面をコンパクトにするため、高さを約2/3(140px)へ統一
const METER_HEIGHT = 140; // px（フェーダーのSLIDER_HEIGHTとも合わせる）
// 各バーの横幅（基準値）。実際の描画は親幅に応じて 24～60px で可変にする
const DEFAULT_BAR_WIDTH = 30;
const SCALE_WIDTH = 56; // 中央目盛キャンバスの横幅
// ラベルをキャンバス内に収めつつ、目盛り領域はメーターと一致させる
const SCALE_TOP_PAD = 12; // 上ラベル用の余白
const SCALE_BOTTOM_PAD = 12; // 下ラベル用の余白
const SCALE_CANVAS_HEIGHT = METER_HEIGHT + SCALE_TOP_PAD + SCALE_BOTTOM_PAD;
const MIN_DB = -60; // 表示下限（dBFS）

// 視覚的な非線形スケール。
// dB値(-60..0) → 0..1 に正規化した値 t に対し、u = t^k を用いる。
// 画面中央(u=0.5)が TARGET_DB_AT_MID になるように k を自動算出する。
const TARGET_DB_AT_MID = -24; // 中央を示すdB（ご要望）
const LINEAR_AT_TARGET = (TARGET_DB_AT_MID - MIN_DB) / (0 - MIN_DB); // 0..1（例: -24dB → 0.6）
const VISUAL_EXPONENT = Math.log(0.5) / Math.log(Math.max(1e-6, LINEAR_AT_TARGET));

// dB → [0..1]（下=0, 上=1）へのマッピング（視覚的な冪乗カーブを適用）
const dbToUnit = (db: number): number => {
  const clamped = Math.max(MIN_DB, Math.min(0, db));
  const linear = (clamped - MIN_DB) / (0 - MIN_DB); // 0..1（線形）
  return Math.pow(linear, VISUAL_EXPONENT); // 非線形で見た目を調整
};

// dB → キャンバス座標Y（px, 下が大きい）
const dbToY = (db: number, height: number): number => {
  const u = dbToUnit(db);
  return height - u * height;
};

// ============================
// HiDPI（高DPI）キャンバス補助
// ============================
// - CSS サイズ（論理ピクセル）を入力に、実ピクセルは devicePixelRatio 倍で確保
// - コンテキストにスケールを適用して以降の描画は CSS 座標で行える
// - 戻り値: スケール適用済みの 2D コンテキスト（失敗時 null）
const setupHiDPICanvas = (canvas: HTMLCanvasElement, cssWidth: number, cssHeight: number): CanvasRenderingContext2D | null => {
  // devicePixelRatio は小数(例: 1.25)もそのまま使用し、丸めない
  const dpr = Math.max(1, window.devicePixelRatio || 1);
  canvas.style.width = `${cssWidth}px`;
  canvas.style.height = `${cssHeight}px`;
  canvas.width = Math.round(cssWidth * dpr);
  canvas.height = Math.round(cssHeight * dpr);
  const ctx = canvas.getContext('2d');
  if (!ctx) return null;
  // CSS 座標系で描画できるように変換
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  // 文字のにじみ軽減
  ctx.imageSmoothingEnabled = false;
  return ctx;
};

// 1デバイスピクセル線をクッキリ出すためのオフセット（DPR対応）
const crisp = (v: number, dpr: number): number => Math.round(v * dpr) / dpr + 0.5 / dpr;

interface MeterBarProps {
  level: number;
  label: string;
  width?: number; // 可変バー幅（px）
}

const MeterBar: React.FC<MeterBarProps> = ({ level, label, width }) => {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const canvasWidth = width ?? DEFAULT_BAR_WIDTH;

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;

    // HiDPI セットアップ（以後は CSS 単位で描画できる）
    const cssW = canvasWidth;
    const cssH = METER_HEIGHT;
    const ctx = setupHiDPICanvas(canvas, cssW, cssH);
    if (!ctx) return;
    const dpr = Math.max(1, window.devicePixelRatio || 1);

    // クリア
    ctx.clearRect(0, 0, cssW, cssH);

    // 背景
    ctx.fillStyle = '#333';
    ctx.fillRect(0, 0, cssW, cssH);

    // グラデーション（下:水色→上:黄→赤）
    const gradient = ctx.createLinearGradient(0, cssH, 0, 0);
    gradient.addColorStop(0, '#4fc3f7');
    gradient.addColorStop(0.6, '#4fc3f7');
    gradient.addColorStop(0.8, '#ffeb3b');
    gradient.addColorStop(1, '#ff5252');

    // レベル（dB）を視覚座標へ変換
    const dbValue = Math.max(MIN_DB, Math.min(0, level));
    const yTop = dbToY(dbValue, cssH);
    ctx.fillStyle = gradient;
    ctx.fillRect(0, yTop, cssW, cssH - yTop);

    // 目盛の補助線（バー自身の左側に短い線）
    ctx.strokeStyle = '#666';
    ctx.lineWidth = 0.5; // CSS 1px（DPRを掛けた実ピクセル幅）
    const marks = [0, -6, -12, -18, -24, -30, -40, -50, -60];
    marks.forEach((db) => {
      const y = dbToY(db, cssH);
      ctx.beginPath();
      const ya = crisp(y, dpr);
      ctx.moveTo(0, ya);
      ctx.lineTo(cssW * 0.25, ya);
      ctx.stroke();
    });
  }, [level, canvasWidth]);

  return (
    <Box sx={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 0.5 }}>
      <canvas
        ref={canvasRef}
        width={canvasWidth}
        height={METER_HEIGHT}
        style={{ borderRadius: 2, border: '1px solid #333', width: canvasWidth, height: METER_HEIGHT }}
      />
      <Typography variant='caption' sx={{ fontSize: '9px', color: 'text.secondary', fontWeight: 500 }}>
        {label}
      </Typography>
    </Box>
  );
};

// 中央の目盛（両メーター共有）。Cubase風に上部を広げる非線形カーブでラベル配置。
const CenterScale: React.FC<{ width?: number }> = ({ width }) => {
  const canvasRef = useRef<HTMLCanvasElement>(null);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const cssW = width ?? SCALE_WIDTH;
    const cssH = SCALE_CANVAS_HEIGHT;
    const ctx = setupHiDPICanvas(canvas, cssW, cssH);
    if (!ctx) return;
    const dpr = Math.max(1, window.devicePixelRatio || 1);

    ctx.clearRect(0, 0, cssW, cssH);
    // 背景・枠・センター線は描画しない（透明のまま）

    // 目盛値（dB）。上ほど密、下は疎になる非線形配置。
    const majorMarks = [0, -6, -12, -18, -24, -30, -40, -50, -60];

    // 目盛り領域（上/下のパディングを除いた部分）を定義
    const contentTop = SCALE_TOP_PAD;
    const contentBottom = cssH - SCALE_BOTTOM_PAD;
    const contentHeight = contentBottom - contentTop;

    // 大目盛 + ラベル
    ctx.strokeStyle = '#bdbdbd';
    ctx.fillStyle = '#e0e0e0';
    ctx.font = '10px sans-serif';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    majorMarks.forEach((db) => {
      const u = dbToUnit(db);
      const y = contentBottom - u * contentHeight;

      // 幅いっぱいに太めの線
      ctx.beginPath();
      ctx.lineWidth = 1; // CSS 1px
      const ya = crisp(y, dpr);
      // ラベル幅を考慮して中央にギャップを設ける
      const label = db === 0 ? '0' : `${-db}`;
      const centerX = cssW / 2;
      const labelWidth = Math.ceil(ctx.measureText(label).width);
      const gap = Math.max(20, labelWidth + 10); // 最低20px、ラベル幅+余白
      // 左側セグメント
      ctx.moveTo(0, ya);
      ctx.lineTo(centerX - gap / 2, ya);
      ctx.stroke();
      // 右側セグメント
      ctx.beginPath();
      ctx.moveTo(centerX + gap / 2, ya);
      ctx.lineTo(cssW, ya);
      ctx.stroke();

      // ラベル（0, 6, 12... は正値で表示、-0は0）
      ctx.fillText(label, Math.round(centerX), Math.round(y));
    });
  }, [width]);

  return (
    <Box sx={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 0, mt: -1.4 }}>
      <canvas
        ref={canvasRef}
        width={width ?? SCALE_WIDTH}
        height={SCALE_CANVAS_HEIGHT}
        style={{
          borderRadius: 2,
          border: 'none',
          background: 'transparent',
          width: width ?? SCALE_WIDTH,
          height: SCALE_CANVAS_HEIGHT,
        }}
      />
    </Box>
  );
};

export const VUMeter: React.FC = () => {
  // メーターレベルは引き続きイベントで更新（既存の useJuceEvents が store を更新している）
  // ここではゲインフェーダーのみ JUCE 直接バインドを使う
  const [hostMeterLevels, setHostMeterLevels] = useState<MeterLevels>({ left: -60, right: -60 });
  const [playlistMeterLevels, setPlaylistMeterLevels] = useState<MeterLevels>({ left: -60, right: -60 });
  // SOURCE_SELECT は Choice パラメータ（0=Host, 1=Playlist）。トグルではなく ComboBox を参照する
  const sourceCombo = getComboBoxState('SOURCE_SELECT');
  const [isHostActive, setIsHostActive] = useState<boolean>(() => (sourceCombo ? sourceCombo.getChoiceIndex() !== 1 : true));
  const [isPlaylistActive, setIsPlaylistActive] = useState<boolean>(() =>
    sourceCombo ? sourceCombo.getChoiceIndex() === 1 : false
  );

  // ============================
  // メーター表示モード
  // ============================
  // METERING_MODE を JUCE ComboBox に直結（0=Peak,1=RMS,2=Momentary）
  const meteringCombo = getComboBoxState('METERING_MODE');
  const [meteringModeIndex, setMeteringModeIndex] = useState<number>(() => (meteringCombo ? meteringCombo.getChoiceIndex() : 0));

  const meterDisplayMode = meteringModeIndex === 0 ? 'peak' : meteringModeIndex === 1 ? 'rms' : 'momentary';

  const toggleMeterMode = () => {
    const newIndex = (meteringModeIndex + 1) % 3;
    setMeteringModeIndex(newIndex);
    // JUCE ComboBox へ直接反映（APVTSに自動伝播 → DAWオートメーションも同期）
    meteringCombo?.setChoiceIndex(newIndex);
  };

  // JUCEへの更新をスロットリング用のref
  // 直バインドに伴い、スロットリング用の参照は不要

  // ドラッグ中のループバック抑制用フラグと最新値保持
  const isHostDraggingRef = useRef<boolean>(false);
  const isPlaylistDraggingRef = useRef<boolean>(false);

  // フェーダーは JUCE 直バインドのため onChange は不要
  // no-op

  // ドラッグ開始/終了ハンドラでフラグ管理とフラッシュ
  const onHostDragStart = () => {
    isHostDraggingRef.current = true;
  };

  const onHostDragEnd = () => {
    isHostDraggingRef.current = false;
  };

  const onPlaylistDragStart = () => {
    isPlaylistDraggingRef.current = true;
  };

  const onPlaylistDragEnd = () => {
    isPlaylistDraggingRef.current = false;
  };

  // ============================
  // ピークホールド（数値表示用）
  // ============================
  // - TruePeak（受信時は truePeakLeft/Right）を基準に数値をホールド表示
  // - 受信がない場合は通常レベルにフォールバック
  // - リセットはラベルのクリックで LR 同時に -∞（MIN_DB）へ戻す
  const clampDb = (db: number): number => Math.max(MIN_DB, Math.min(0, db));
  const formatDb = (db: number): string => (db <= MIN_DB ? '-∞' : clampDb(db).toFixed(1));

  // HOST・PLAYLIST それぞれの L/R 保持値
  const [hostPeak, setHostPeak] = useState<{ left: number; right: number }>({ left: MIN_DB, right: MIN_DB });
  const [playlistPeak, setPlaylistPeak] = useState<{ left: number; right: number }>({ left: MIN_DB, right: MIN_DB });

  // Momentaryホールド（表示用） - 現在はMeterLevelsから直接取得するため未使用
  // const [hostMomentaryHold, setHostMomentaryHold] = useState<number>(-70.0);
  // const [playlistMomentaryHold, setPlaylistMomentaryHold] = useState<number>(-70.0);

  // 入力レベルが上がった時のみ保持値を更新（dB は 0 に近いほど大きい）
  useEffect(() => {
    const l = meterDisplayMode === 'peak' ? hostMeterLevels.truePeakLeft ?? hostMeterLevels.left : hostMeterLevels.left;
    const r = meterDisplayMode === 'peak' ? hostMeterLevels.truePeakRight ?? hostMeterLevels.right : hostMeterLevels.right;
    setHostPeak((prev) => {
      const nextLeft = Math.max(prev.left, clampDb(l));
      const nextRight = Math.max(prev.right, clampDb(r));
      if (nextLeft === prev.left && nextRight === prev.right) return prev;
      return { left: nextLeft, right: nextRight };
    });
  }, [
    meterDisplayMode,
    hostMeterLevels.left,
    hostMeterLevels.right,
    hostMeterLevels.truePeakLeft,
    hostMeterLevels.truePeakRight,
  ]);

  useEffect(() => {
    const l =
      meterDisplayMode === 'peak' ? playlistMeterLevels.truePeakLeft ?? playlistMeterLevels.left : playlistMeterLevels.left;
    const r =
      meterDisplayMode === 'peak' ? playlistMeterLevels.truePeakRight ?? playlistMeterLevels.right : playlistMeterLevels.right;
    setPlaylistPeak((prev) => {
      const nextLeft = Math.max(prev.left, clampDb(l));
      const nextRight = Math.max(prev.right, clampDb(r));
      if (nextLeft === prev.left && nextRight === prev.right) return prev;
      return { left: nextLeft, right: nextRight };
    });
  }, [
    meterDisplayMode,
    playlistMeterLevels.left,
    playlistMeterLevels.right,
    playlistMeterLevels.truePeakLeft,
    playlistMeterLevels.truePeakRight,
  ]);

  // JUCE からのメーター更新を直接購読
  useEffect(() => {
    const id = juceBridge.addEventListener('meterUpdate', (data: unknown) => {
      const m = data as MeterUpdateData;
      if (typeof m.meteringMode === 'number') setMeteringModeIndex(m.meteringMode);
      if (m.host) {
        if (m.meteringMode === 2 && m.host.momentary !== undefined) {
          setHostMeterLevels({
            left: m.host.momentary ?? -60,
            right: m.host.momentary ?? -60,
            truePeakLeft: m.host.truePeakLeft,
            truePeakRight: m.host.truePeakRight,
            momentary: m.host.momentary,
            momentaryHold: m.host.momentaryHold,
          });
        } else if (m.meteringMode === 0) {
          setHostMeterLevels({
            left: m.host.truePeakLeft ?? -60,
            right: m.host.truePeakRight ?? -60,
            truePeakLeft: m.host.truePeakLeft,
            truePeakRight: m.host.truePeakRight,
          });
        } else {
          setHostMeterLevels({
            left: m.host.rmsLeft ?? -60,
            right: m.host.rmsRight ?? -60,
            truePeakLeft: m.host.truePeakLeft,
            truePeakRight: m.host.truePeakRight,
          });
        }
      }
      if (m.playlist) {
        if (m.meteringMode === 2 && m.playlist.momentary !== undefined) {
          setPlaylistMeterLevels({
            left: m.playlist.momentary ?? -60,
            right: m.playlist.momentary ?? -60,
            truePeakLeft: m.playlist.truePeakLeft,
            truePeakRight: m.playlist.truePeakRight,
            momentary: m.playlist.momentary,
            momentaryHold: m.playlist.momentaryHold,
          });
        } else if (m.meteringMode === 0) {
          setPlaylistMeterLevels({
            left: m.playlist.truePeakLeft ?? -60,
            right: m.playlist.truePeakRight ?? -60,
            truePeakLeft: m.playlist.truePeakLeft,
            truePeakRight: m.playlist.truePeakRight,
          });
        } else {
          setPlaylistMeterLevels({
            left: m.playlist.rmsLeft ?? -60,
            right: m.playlist.rmsRight ?? -60,
            truePeakLeft: m.playlist.truePeakLeft,
            truePeakRight: m.playlist.truePeakRight,
          });
        }
      }
    });
    return () => {
      juceBridge.removeEventListener(id);
    };
  }, []);

  // METERING_MODE ComboBox の変更を購読（DAW/他UIからの更新をUI状態へ反映）
  useEffect(() => {
    if (!meteringCombo) return;
    const id = meteringCombo.valueChangedEvent.addListener(() => {
      setMeteringModeIndex(meteringCombo.getChoiceIndex());
    });
    return () => meteringCombo.valueChangedEvent.removeListener(id);
  }, [meteringCombo]);

  // SOURCE_SELECT (ComboBox) を購読してアクティブ表示を更新（0=Host, 1=Playlist）
  useEffect(() => {
    if (!sourceCombo) return;
    const onChange = () => {
      const ix = sourceCombo.getChoiceIndex();
      setIsPlaylistActive(ix === 1);
      setIsHostActive(ix !== 1);
    };
    const id = sourceCombo.valueChangedEvent.addListener(onChange);
    return () => sourceCombo.valueChangedEvent.removeListener(id);
  }, [sourceCombo]);

  // クリックで LR 同時リセット
  const resetHostPeak = () => {
    if (meterDisplayMode === 'momentary') {
      // Momentaryホールドのリセットはネイティブ側に委譲する
      // 注意: withNativeFunction はスラッシュを含む名前を想定していないため、
      //       'metering_reset' に統一する（C++側に対応ハンドラを追加済み）。
      juceBridge
        .callNative('metering_reset', 'momentary')
        .catch((error) => console.error('Error resetting momentary hold:', error));
    } else {
      // Peak/RMS モードはフロント側の保持値のみ即時リセット
      setHostPeak({ left: MIN_DB, right: MIN_DB });
    }
  };
  const resetPlaylistPeak = () => {
    if (meterDisplayMode === 'momentary') {
      // Momentaryホールドのリセットはネイティブ側に委譲（HOST/PLAYLIST/OUTPUT 全面）
      juceBridge
        .callNative('metering_reset', 'momentary')
        .catch((error) => console.error('Error resetting momentary hold:', error));
    } else {
      setPlaylistPeak({ left: MIN_DB, right: MIN_DB });
    }
  };

  // メータリングリセットイベントの受信（モード切替時）
  useEffect(() => {
    const handleMeteringReset = () => {
      // C++ 側からのリセット通知
      // - Momentary モード: C++ の MomentaryProcessor 側でホールド値が更新されるため、
      //   フロントは何もしない（次のメータ更新で UI が更新される）
      // - Peak/RMS モード: フロント保持のホールド値をクリアする
      if (meterDisplayMode !== 'momentary') {
        setHostPeak({ left: MIN_DB, right: MIN_DB });
        setPlaylistPeak({ left: MIN_DB, right: MIN_DB });
      }
    };

    const listenerId = juceBridge.addEventListener('meteringReset', handleMeteringReset);

    return () => {
      juceBridge.removeEventListener(listenerId);
    };
  }, [meterDisplayMode]);

  // 中央メーター群の可用幅を観測してバー幅を自動調整
  const centerRef = useRef<HTMLDivElement | null>(null);
  const [centerWidth, setCenterWidth] = useState<number>(0);

  useEffect(() => {
    if (!centerRef.current) return;
    const el = centerRef.current;
    const ro = new ResizeObserver((entries) => {
      for (const entry of entries) {
        const w = entry.contentRect.width;
        // 幅が同じ場合はステート更新をスキップし、不要な再レンダーを抑制
        setCenterWidth((prev) => (prev !== w ? w : prev));
      }
    });
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  // 中央スケールは固定幅、残りを4本のバーで分配
  const GAP_PX = 4; // ギャップをさらに縮小（MUI gap ≈ 0.25 → 2px 想定の概算×2）
  const availableForBars = Math.max(0, centerWidth - SCALE_WIDTH - GAP_PX * 6);
  // 最大幅の上限は設けない。視認性確保のため下限のみ 24px。
  const computedBarWidth = Math.max(24, Math.floor(availableForBars / 4));

  return (
    <Box
      sx={{ mt: 1, display: 'grid', gridTemplateColumns: 'auto 1fr auto', alignItems: 'flex-start', gap: 0.25, width: '100%' }}
    >
      {/* HOSTフェーダー（左側） */}
      <GainFader
        parameterId={'HOST_GAIN'}
        onDragStart={onHostDragStart}
        onDragEnd={onHostDragEnd}
        label='HOST'
        color='primary'
        active={isHostActive}
        showLabel={false}
        defaultValue={0} // 0dB（Unity Gain）
      />

      {/* メーター部分（中央・可変幅） */}
      <Box
        ref={centerRef}
        sx={{ display: 'flex', justifyContent: 'center', alignItems: 'flex-start', gap: 0.25, width: '100%', minWidth: 0 }}
      >
        {/* HOST メーター */}
        <Box sx={{ display: 'flex', flexDirection: 'column', alignItems: 'center' }}>
          {meterDisplayMode === 'momentary' ? (
            // Momentaryモード: 単一バー
            <>
              <Box sx={{ display: 'flex', gap: 0.25 }}>
                <MeterBar
                  level={hostMeterLevels.momentary ?? -70}
                  label='LKFS'
                  width={computedBarWidth * 2 + 4} // 両バー分の幅
                />
              </Box>
              <Tooltip title='Reset Hold'>
                <Box onClick={resetHostPeak} sx={{ mt: -0.525, cursor: 'pointer', userSelect: 'none' }}>
                  <Typography variant='caption' sx={{ color: 'contrastText', fontSize: '10px', textAlign: 'center' }}>
                    {formatDb(hostMeterLevels.momentaryHold ?? -70)}
                  </Typography>
                </Box>
              </Tooltip>
            </>
          ) : (
            // Peak/RMSモード: L/Rバー
            <>
              <Box sx={{ display: 'flex', gap: 0.25 }}>
                <MeterBar level={hostMeterLevels.left} label='L' width={computedBarWidth} />
                <MeterBar level={hostMeterLevels.right} label='R' width={computedBarWidth} />
              </Box>
              {/* 非Momentary時は常にホールド値（最大値）を表示し、クリックでリセット */}
              <Tooltip title='Reset Hold'>
                <Box onClick={resetHostPeak} sx={{ mt: 0.25, display: 'flex', gap: 0.25, cursor: 'pointer', userSelect: 'none' }}>
                  <Typography
                    variant='caption'
                    sx={{ color: 'contrastText', fontSize: '10px', width: computedBarWidth, textAlign: 'center' }}
                  >
                    {formatDb(hostPeak.left)}
                  </Typography>
                  <Typography
                    variant='caption'
                    sx={{ color: 'contrastText', fontSize: '10px', width: computedBarWidth, textAlign: 'center' }}
                  >
                    {formatDb(hostPeak.right)}
                  </Typography>
                </Box>
              </Tooltip>
            </>
          )}
        </Box>

        {/* 中央スケール（対数カーブ風） + 表示モードボタン（UIのみ） */}
        <Box sx={{ display: 'flex', flexDirection: 'column', alignItems: 'center' }}>
          <CenterScale width={SCALE_WIDTH} />
          {/*
           * 「VU / Peak」切替（UIのみ、未配線）
           * - 通常のボタン。クリックでラベルだけが切り替わる
           * - size="small" でコンパクトにし、ピーク値ラベルの間・中央にレイアウト
           */}
          {/* Add Files ボタンと同一スケール/パディングに合わせる（見た目統一） */}
          <Tooltip title='Meter display mode' arrow>
            <Button
              onClick={toggleMeterMode}
              size='small'
              variant='contained'
              sx={{
                mt: 0.25,
                mx: meterDisplayMode === 'momentary' ? -2 : 0,
                textTransform: 'none',
                minWidth: 'auto',
                px: 1,
                py: meterDisplayMode === 'momentary' ? 0.2 : 1,
                height: 24,
                border: '2px #fff solid',
                borderColor: 'divider',
                backgroundColor: 'transparent',
                color: 'text.primary',
                '&:hover': { backgroundColor: 'grey.600' },
              }}
              aria-label='meter display mode'
            >
              {meterDisplayMode === 'peak' ? 'Peak' : meterDisplayMode === 'rms' ? 'RMS' : 'Momentary'}
            </Button>
          </Tooltip>
        </Box>

        {/* PLAYLIST メーター */}
        <Box sx={{ display: 'flex', flexDirection: 'column', alignItems: 'center' }}>
          {meterDisplayMode === 'momentary' ? (
            // Momentaryモード: 単一バー
            <>
              <Box sx={{ display: 'flex', gap: 0.25 }}>
                <MeterBar
                  level={playlistMeterLevels.momentary ?? -70}
                  label='LKFS'
                  width={computedBarWidth * 2 + 4} // 両バー分の幅
                />
              </Box>
              <Tooltip title='Reset Hold'>
                <Box onClick={resetPlaylistPeak} sx={{ mt: -0.525, cursor: 'pointer', userSelect: 'none' }}>
                  <Typography variant='caption' sx={{ color: 'contrastText', fontSize: '10px', textAlign: 'center' }}>
                    {formatDb(playlistMeterLevels.momentaryHold ?? -70)}
                  </Typography>
                </Box>
              </Tooltip>
            </>
          ) : (
            // Peak/RMSモード: L/Rバー
            <>
              <Box sx={{ display: 'flex', gap: 0.25 }}>
                <MeterBar level={playlistMeterLevels.left} label='L' width={computedBarWidth} />
                <MeterBar level={playlistMeterLevels.right} label='R' width={computedBarWidth} />
              </Box>
              {/* 非Momentary時は常にホールド値（最大値）を表示し、クリックでリセット */}
              <Tooltip title='Reset Hold'>
                <Box
                  onClick={resetPlaylistPeak}
                  sx={{ mt: 0.25, display: 'flex', gap: 0.25, cursor: 'pointer', userSelect: 'none' }}
                >
                  <Typography
                    variant='caption'
                    sx={{ color: 'contrastText', fontSize: '10px', width: computedBarWidth, textAlign: 'center' }}
                  >
                    {formatDb(playlistPeak.left)}
                  </Typography>
                  <Typography
                    variant='caption'
                    sx={{ color: 'contrastText', fontSize: '10px', width: computedBarWidth, textAlign: 'center' }}
                  >
                    {formatDb(playlistPeak.right)}
                  </Typography>
                </Box>
              </Tooltip>
            </>
          )}
        </Box>
      </Box>

      {/* PLAYLISTフェーダー（右側） */}
      <GainFader
        parameterId={'PLAYLIST_GAIN'}
        onDragStart={onPlaylistDragStart}
        onDragEnd={onPlaylistDragEnd}
        label='PLAYLIST'
        color='primary'
        active={isPlaylistActive}
        showLabel={false}
        defaultValue={0} // 0dB（Unity Gain）
      />
    </Box>
  );
};
