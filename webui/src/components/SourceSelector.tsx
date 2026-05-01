// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
import React, { useEffect, useState } from 'react';
import { Box, ButtonBase } from '@mui/material';
import { getComboBoxState } from 'juce-framework-frontend-mirror';
import { AudioSource } from '../types';
// A/B スイッチ画像をバンドル資産から読み込む
// 画像サイズは 1885x257（横長）。UI 幅に合わせて縮小表示する。
import switchA from '../assets/switch_a.png';
import switchB from '../assets/switch_b.png';

/**
 * SourceSelector
 *  - クリックごとに A/B をトグルするシングルボタン UI。
 *  - A: HOST(DAW) / B: PLAYLIST のマッピング。
 *  - クリック領域は画像全体。ドラッグで画像が選択されないように抑止。
 */
export const SourceSelector: React.FC = () => {
  // JUCE の Choice パラメータを直接バインド（0=Host, 1=Playlist）
  const combo = getComboBoxState('SOURCE_SELECT');
  const [source, setSource] = useState<AudioSource>(() => {
    const ix = combo ? combo.getChoiceIndex() : 0;
    return ix === 1 ? AudioSource.Playlist : AudioSource.Host;
  });

  useEffect(() => {
    if (!combo) return;
    const id = combo.valueChangedEvent.addListener(() => {
      const ix = combo.getChoiceIndex();
      setSource(ix === 1 ? AudioSource.Playlist : AudioSource.Host);
    });
    return () => combo.valueChangedEvent.removeListener(id);
  }, [combo]);

  // 画像クリックでソースをトグル（JUCEへ直接）
  const handleToggle = async () => {
    if (!combo) return;
    const next = source === AudioSource.Host ? AudioSource.Playlist : AudioSource.Host;
    setSource(next); // 楽観的更新
    combo.setChoiceIndex(next === AudioSource.Playlist ? 1 : 0);
  };

  // 現在状態に応じて表示画像を選択
  const imageSrc = source === AudioSource.Host ? switchA : switchB;
  const altText = source === AudioSource.Host ? 'Source A (HOST)' : 'Source B (PLAYLIST)';

  return (
    <Box sx={{ display: 'flex', justifyContent: 'center', mb: 1.5, mt: -2.5 }}>
      {/*
        - maxWidth で横幅の上限を設けつつ、親の余白に応じて拡縮
        - userSelect/drag を無効化して誤操作を防止
      */}
      <ButtonBase
        onClick={handleToggle}
        focusRipple
        aria-label='Toggle audio source'
        // ButtonBase を使うことで MUI の TouchRipple が有効化される。
        // リップル色は基本的に currentColor を使用するため、
        // color に theme.palette.primary.main を割り当てて Primary カラーのリップルにする。
        sx={{
          // レイアウト/ヒット領域設定
          display: 'block',
          width: '100%',
          maxWidth: 365,
          lineHeight: 0, // 画像下の余白を消す
          borderRadius: 1,
          overflow: 'hidden', // リップルを角丸内にクリップ
          // 視覚設定
          color: 'primary.main', // リップル色の基準（currentColor）
          userSelect: 'none',
          cursor: 'pointer',
          '& img': {
            transition: 'filter 0.2s ease',
            filter: 'brightness(1)',
            display: 'block',
            width: '100%',
            height: 'auto',
          },
          '&:hover img': {
            filter: 'brightness(1.15)', // ホバー時に少し明るく
          },
          // 念のため、リップル内部子要素の色も currentColor を強制
          '& .MuiTouchRipple-root .MuiTouchRipple-child': {
            backgroundColor: 'currentColor',
          },
          // リップル自体を画像のアルファでマスクする
          // Chromium(WebView2) では -webkit-mask-* が広く対応
          '& .MuiTouchRipple-root': {
            WebkitMaskImage: `url(${imageSrc})`,
            maskImage: `url(${imageSrc})`,
            WebkitMaskRepeat: 'no-repeat',
            maskRepeat: 'no-repeat',
            WebkitMaskPosition: 'center',
            maskPosition: 'center',
            WebkitMaskSize: '100% 100%',
            maskSize: '100% 100%',
            WebkitMaskComposite: 'source-over',
            maskMode: 'alpha',
          },
        }}
      >
        <img src={imageSrc} alt={altText} draggable={false} />
      </ButtonBase>
    </Box>
  );
};
