// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
import React from 'react';
import { Box, IconButton, Typography } from '@mui/material';
import { Clear, DragIndicator } from '@mui/icons-material';
import { useSortable } from '@dnd-kit/sortable';
import { CSS } from '@dnd-kit/utilities';
import { type PlaylistItem as PlaylistItemType } from '../types';

// プレイリストの1行（ソート可能アイテム）を表示するコンポーネント
// - DnD Kit の useSortable を用いてドラッグ可能にする
// - 欠落ファイルの場合は選択不可/スタイルを減光
export interface PlaylistItemProps {
  item: PlaylistItemType;
  index: number;
  isActive: boolean;
  onSelect: (index: number) => void;
  onRemove: (id: string) => void;
}

const PlaylistItem: React.FC<PlaylistItemProps> = ({ item, index, isActive, onSelect, onRemove }) => {
  const { attributes, listeners, setNodeRef, transform, transition, isDragging } = useSortable({ id: item.id });

  // 秒数を mm:ss 表示に整形
  const formatDuration = (seconds: number): string => {
    const mins = Math.floor(seconds / 60);
    const secs = Math.floor(seconds % 60);
    return `${mins}:${secs.toString().padStart(2, '0')}`;
  };

  // 欠落ファイルは選択不可にする（onClickを無効化）
  const handleItemClick = (e: React.MouseEvent) => {
    // ドラッグ中はクリックを無視
    if (isDragging) {
      e.preventDefault();
      return;
    }
    if (item.exists === false) return; // 欠落時は無視
    onSelect(index);
  };

  return (
    <Box
      ref={setNodeRef}
      style={{
        transform: CSS.Transform.toString(transform),
        transition,
      }}
      sx={{
        display: 'flex',
        alignItems: 'center',
        padding: '2px 4px',
        backgroundColor: isActive ? 'primary.dark' : 'transparent',
        borderRadius: 1,
        cursor: isDragging ? 'grabbing' : item.exists === false ? 'not-allowed' : 'pointer',
        userSelect: 'none',
        opacity: isDragging ? 0.5 : 1,
        zIndex: isDragging ? 1000 : 'auto',
        '&:hover': {
          backgroundColor: isActive ? 'primary.dark' : 'action.hover',
        },
      }}
      onClick={handleItemClick}
      {...attributes}
      {...listeners}
    >
      <Box
        sx={{
          cursor: isDragging ? 'grabbing' : 'grab',
          minWidth: 24,
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'center',
          flexShrink: 0,
        }}
      >
        <DragIndicator sx={{ fontSize: 14 }} />
      </Box>

      <Box sx={{ flexGrow: 1, minWidth: 0, height: 16, display: 'flex', alignItems: 'center' }}>
        {/**
         * ファイル名はリスト項目の中央エリアに配置し、右側の「時間」「削除ボタン」を
         * 邪魔しない範囲で縮むようにする。ellipsis を効かせるために以下が重要：
         * - 親(Box)に minWidth: 0 を指定（Flex の暗黙最小幅を解除）
         * - Typography に overflow:hidden / textOverflow:ellipsis / whiteSpace:nowrap を付与
         * - Typography をブロック要素として扱い、必要幅が超えたら省略表示
         */}
        <Typography
          variant='caption'
          sx={{
            lineHeight: 1,
            letterSpacing: '0.05em',
            overflow: 'hidden', // 省略記号の前提
            textOverflow: 'ellipsis', // はみ出たテキストを … に
            whiteSpace: 'nowrap', // 折り返さない
            color: item.exists === false ? 'text.disabled' : isActive ? '#fff' : 'text.primary',
            display: 'block', // ellipsis の動作を安定させる
            opacity: item.exists === false ? 0.6 : 1,
          }}
          noWrap
        >
          {(() => {
            // 拡張子を薄色で表示し、ベース名は通常色で表示
            const displayName = item.name;
            const lastDot = displayName.lastIndexOf('.');
            const nameWithoutExt = lastDot > 0 ? displayName.substring(0, lastDot) : displayName;
            const extension = lastDot > 0 ? displayName.substring(lastDot).toLowerCase() : '';

            if (item.exists === false) {
              return (
                <>
                  (Missing){nameWithoutExt}
                  {extension && <span style={{ color: 'rgba(255, 255, 255, 0.5)', marginLeft: '2px' }}>{extension}</span>}
                </>
              );
            }

            return (
              <>
                {nameWithoutExt}
                {extension && <span style={{ color: 'rgba(255, 255, 255, 0.5)', marginLeft: '2px' }}>{extension}</span>}
              </>
            );
          })()}
        </Typography>
      </Box>

      {/**
       * 右側の時間表示は固定幅・非縮小にして、左側テキストを優先的に省略させる。
       * 2桁分の分表示(例: 12:34)を想定して 6ch を確保。右揃えで視認性を高める。
       */}
      <Box
        sx={{
          height: 16,
          p: 0,
          display: 'flex',
          alignItems: 'center',
          ml: 1,
          width: '4ch',
          flexShrink: 0,
          justifyContent: 'flex-end',
        }}
      >
        <Typography
          variant='caption'
          color={item.exists === false ? 'text.disabled' : isActive ? 'text.primary' : 'text.secondary'}
          sx={{
            lineHeight: 1,
            textAlign: 'right',
            width: '100%',
            fontFamily: '"Red Hat Mono", monospace',
            letterSpacing: '-0.0005em',
            opacity: item.exists === false ? 0.6 : 1,
          }}
        >
          {item.exists === false ? '--:--' : formatDuration(item.duration)}
        </Typography>
      </Box>

      <IconButton
        edge='end'
        onClick={(e) => {
          e.stopPropagation();
          onRemove(item.id);
        }}
        size='small'
        sx={{
          height: 16,
          width: 16,
          p: 0,
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'center',
          ml: 0.5,
          flexShrink: 0,
          color: item.exists === false ? 'text.disabled' : isActive ? 'text.primary' : 'text.secondary',
          '&:hover': { color: 'text.primary' },
        }}
      >
        <Clear sx={{ fontSize: 14 }} />
      </IconButton>
    </Box>
  );
};

export default PlaylistItem;


