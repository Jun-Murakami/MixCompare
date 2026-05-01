// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
import React, { useEffect, useRef } from 'react';
import { getSliderState } from 'juce-framework-frontend-mirror';
import { Box, IconButton, Typography, Button, Tooltip } from '@mui/material';
import {
  DndContext,
  closestCenter,
  KeyboardSensor,
  PointerSensor,
  useSensor,
  useSensors,
  type DragEndEvent,
} from '@dnd-kit/core';
import { SortableContext, sortableKeyboardCoordinates, verticalListSortingStrategy } from '@dnd-kit/sortable';
// useSortable, CSS は PlaylistItem に移動
import { restrictToVerticalAxis, restrictToParentElement } from '@dnd-kit/modifiers';
import { Add, Clear, Save } from '@mui/icons-material';
import { Menu, MenuItem } from '@mui/material';
import { useVirtualizer } from '@tanstack/react-virtual';
import { juceBridge } from '../bridge/juce';
import { type PlaylistItem } from '../types';
import PlaylistItemRow from './PlaylistItem';

// 上記 SortableItem は PlaylistItem.tsx に分離

export const Playlist: React.FC = () => {
  // ローカル状態（ストアから移行）
  const [playlist, setPlaylist] = React.useState<PlaylistItem[]>([]);
  const [currentPlaylistIndex, setCurrentPlaylistIndex] = React.useState<number>(-1);
  const listContainerRef = useRef<HTMLDivElement>(null);
  const [anchorEl, setAnchorEl] = React.useState<null | HTMLElement>(null);
  // JUCE更新を楽観操作の直後に抑制するための期限
  const suppressUntilRef = React.useRef<number>(0)

  // Setup virtualizer
  const virtualizer = useVirtualizer({
    count: playlist.length,
    getScrollElement: () => listContainerRef.current,
    estimateSize: () => 20, // Estimated height of each item
    overscan: 5, // Number of items to render outside of the visible area
  });

  // Auto-scroll to selected item when index changes (only if out of view)
  useEffect(() => {
    if (currentPlaylistIndex >= 0 && currentPlaylistIndex < playlist.length) {
      const container = listContainerRef.current;
      if (!container) return;

      // Get container's visible bounds
      const containerHeight = container.clientHeight;
      const scrollTop = container.scrollTop;
      const scrollBottom = scrollTop + containerHeight;

      // Calculate the position of the selected item
      const itemHeight = 20; // Same as estimateSize
      const itemTop = currentPlaylistIndex * itemHeight;
      const itemBottom = itemTop + itemHeight;

      // Check if item is fully visible (not just in virtual items which includes overscan)
      const isFullyVisible = itemTop >= scrollTop && itemBottom <= scrollBottom;
      const isPartiallyVisible = itemTop < scrollBottom && itemBottom > scrollTop;

      if (!isFullyVisible) {
        // Determine scroll position to center the item
        if (!isPartiallyVisible || itemTop < scrollTop || itemBottom > scrollBottom) {
          // Item is out of view or only partially visible, scroll to center it
          virtualizer.scrollToIndex(currentPlaylistIndex, {
            align: 'center',
            behavior: 'smooth',
          });
        }
      }
    }
  }, [currentPlaylistIndex, virtualizer, playlist.length]);

  // JUCE playlistUpdate / trackChange を購読してローカル更新
  useEffect(() => {
    const playlistId = juceBridge.addEventListener('playlistUpdate', (data: unknown) => {
      const d = data as { items?: PlaylistItem[]; currentIndex?: number; revision?: number };
      // 抑制期間中でも currentIndex は反映して選択状態をずらさない
      if (typeof d.currentIndex === 'number') setCurrentPlaylistIndex(d.currentIndex);
      // items の差し替えは抑制期間が過ぎてから適用（並べ替えの楽観更新と競合しないように）
      if (Array.isArray(d.items)) {
        if (Date.now() >= suppressUntilRef.current) {
          setPlaylist(d.items);
        }
      }
    });
    const trackId = juceBridge.addEventListener('trackChange', (data: unknown) => {
      const d = data as { items?: PlaylistItem[]; currentIndex?: number };
      // トラック切替は常に即時反映（抑制しない）
      if (Array.isArray(d.items)) setPlaylist(d.items);
      if (typeof d.currentIndex === 'number') setCurrentPlaylistIndex(d.currentIndex);
    });
    return () => {
      juceBridge.removeEventListener(playlistId);
      juceBridge.removeEventListener(trackId);
    };
  }, []);

  const sensors = useSensors(
    useSensor(PointerSensor, {
      activationConstraint: {
        distance: 8, // 8px以上の移動でドラッグ開始
      },
    }),
    useSensor(KeyboardSensor, {
      coordinateGetter: sortableKeyboardCoordinates,
    })
  );

  const handleDragEnd = async (event: DragEndEvent) => {
    const { active, over } = event;

    if (over && active.id !== over.id) {
      const oldIndex = playlist.findIndex((item) => item.id === active.id);
      const newIndex = playlist.findIndex((item) => item.id === over.id);

      // JUCEからの更新を一時的に抑制（300ms）
      suppressUntilRef.current = Date.now() + 300;

      // 楽観的更新：即座にUIを更新
      setPlaylist((prev) => {
        const items = [...prev];
        const [moved] = items.splice(oldIndex, 1);
        items.splice(newIndex, 0, moved);
        // 選択インデックスを更新
        setCurrentPlaylistIndex((ix) => {
          if (ix === oldIndex) return newIndex;
          if (oldIndex < ix && newIndex >= ix) return ix - 1;
          if (oldIndex > ix && newIndex <= ix) return ix + 1;
          return ix;
        });
        return items;
      });

      // バックグラウンドでJUCEに通知
      juceBridge.callNative('playlist_action', 'reorder', oldIndex, newIndex);
    }
  };

  const handleSelect = async (index: number) => {
    // JUCEからの更新を一時的に抑制（200ms）
    suppressUntilRef.current = Date.now() + 200;
    // 直接パラメータにバインド（0..1 正規化）
    const norm = playlist.length > 1 ? index / (playlist.length - 1) : 0;
    getSliderState('PLAYLIST_CURRENT_INDEX_NORM')?.setNormalisedValue(Math.max(0, Math.min(1, norm)));
  };

  const handleRemove = async (id: string) => {
    suppressUntilRef.current = Date.now() + 300;
    // 楽観的更新
    setPlaylist((prev) => {
      const ix = prev.findIndex((x) => x.id === id);
      const next = prev.filter((x) => x.id !== id);
      setCurrentPlaylistIndex((curr) => {
        if (ix === curr) return Math.min(curr, next.length - 1);
        if (ix < curr) return Math.max(0, curr - 1);
        return curr;
      });
      return next;
    });
    // 通知
    juceBridge.callNative('playlist_action', 'remove', id);
  };

  const handleAddFiles = async () => {
    await juceBridge.callNative('playlist_action', 'add');
  };

  const handleClearPlaylist = async () => {
    suppressUntilRef.current = Date.now() + 300;
    setPlaylist([]);
    // 選択は維持せず -1 にする
    setCurrentPlaylistIndex(-1);
    await juceBridge.callNative('playlist_action', 'clear');
  };

  const handleExportPlaylist = async () => {
    await juceBridge.callNative('playlist_action', 'export');
    handleMenuClose();
  };

  const handleImportPlaylist = async () => {
    // JUCEからの更新を一時的に抑制（300ms）
    suppressUntilRef.current = Date.now() + 300;

    await juceBridge.callNative('playlist_action', 'import');
    handleMenuClose();
  };

  const handleMenuOpen = (event: React.MouseEvent<HTMLElement>) => {
    setAnchorEl(event.currentTarget);
  };

  const handleMenuClose = () => {
    setAnchorEl(null);
  };

  return (
    // ルートは親の割り当て高いっぱいを使う
    // - height: '100%': 親(Box)から受け取った高さを占有
    // - minHeight: 0: 内部のスクロール領域が正しく縮むように設定（Flexbox 既定の最小サイズ対策）
    <Box sx={{ height: '100%', minHeight: 0, display: 'flex', flexDirection: 'column' }}>
      <Box sx={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
        <Typography variant='body2'>Playlist</Typography>
        <Box sx={{ display: 'flex', gap: 0.5, alignItems: 'center' }}>
          <Button
            variant='contained'
            startIcon={<Add sx={{ mr: -0.5 }} />}
            onClick={handleAddFiles}
            size='small'
            sx={{ textTransform: 'none', minWidth: 'auto', px: 1, height: 24 }}
          >
            Add Files
          </Button>
          <Tooltip title='Import/Export playlist' arrow>
            <IconButton onClick={handleMenuOpen} size='small' sx={{ width: 24, height: 24 }}>
              <Save sx={{ fontSize: 18 }} />
            </IconButton>
          </Tooltip>
          <Tooltip title='Clear playlist' arrow>
            <span style={{ display: 'inline-block' }}>
              <IconButton
                onClick={handleClearPlaylist}
                size='small'
                disabled={playlist.length === 0}
                sx={{ width: 24, height: 24 }}
              >
                <Clear sx={{ fontSize: 18 }} />
              </IconButton>
            </span>
          </Tooltip>
        </Box>
      </Box>

      {/* Import/Export メニュー */}
      {/*
       * MenuList を dense にして既定の垂直パディングを縮める。
       * さらに .MuiMenuItem-root に対して最小高さ/パディングを明示上書きし、
       * コンパクトな項目高さ（約 28px）に統一する。
       */}
      <Menu
        anchorEl={anchorEl}
        open={Boolean(anchorEl)}
        onClose={handleMenuClose}
        slotProps={{
          list: { dense: true },
          paper: { sx: { backgroundColor: 'grey.800' } },
        }}
        anchorOrigin={{
          vertical: 'bottom',
          horizontal: 'right',
        }}
        transformOrigin={{
          vertical: 'top',
          horizontal: 'right',
        }}
        sx={{
          py: 0.5,
          '& .MuiMenuItem-root': {
            // MUI のデフォルト(48px / dense 36px程度)よりさらに低くする
            minHeight: 28,
            py: 0.25, // 垂直方向の余白を詰める
            my: 0,
          },
        }}
      >
        <MenuItem
          onClick={handleImportPlaylist}
          sx={{
            typography: 'body2',
            lineHeight: 1.2, // コンパクト表示時に行間を詰める
          }}
        >
          Import Playlist
        </MenuItem>
        <MenuItem
          onClick={handleExportPlaylist}
          disabled={playlist.length === 0}
          sx={{
            typography: 'body2',
            lineHeight: 1.2,
          }}
        >
          Export Playlist
        </MenuItem>
      </Menu>

      {/* 一覧部は可変。縦に伸縮し、必要に応じて内部スクロール */}
      <Box
        ref={listContainerRef}
        sx={{
          flex: 1,
          minHeight: 0,
          overflowY: 'scroll', // 'auto'から'scroll'に変更して常時表示
          overflowX: 'hidden', // 横スクロールは不要
          mt: 1,
          border: '1px solid',
          borderColor: 'divider',
          height: '100%',
        }}
      >
        {playlist.length === 0 ? (
          <Box
            sx={{
              height: '100%',
              display: 'flex',
              alignItems: 'center',
              justifyContent: 'center',
              color: 'text.secondary',
            }}
          >
            <Typography>No files in playlist</Typography>
          </Box>
        ) : (
          <DndContext
            sensors={sensors}
            collisionDetection={closestCenter}
            onDragEnd={handleDragEnd}
            modifiers={[restrictToVerticalAxis, restrictToParentElement]}
          >
            <SortableContext items={playlist.map((item) => item.id)} strategy={verticalListSortingStrategy}>
              <Box sx={{ width: '100%', position: 'relative' }}>
                {/* 仮想化: 上部のパディング */}
                {virtualizer.getVirtualItems().length > 0 && (
                  <div style={{ height: virtualizer.getVirtualItems()[0]?.start || 0 }} />
                )}

                {/* 仮想化されたアイテム */}
                {virtualizer.getVirtualItems().map((virtualItem) => {
                  const item = playlist[virtualItem.index];
                  if (!item) return null;

                  return (
                    <PlaylistItemRow
                      key={item.id}
                      item={item}
                      index={virtualItem.index}
                      isActive={virtualItem.index === currentPlaylistIndex}
                      onSelect={handleSelect}
                      onRemove={handleRemove}
                    />
                  );
                })}

                {/* 仮想化: 下部のパディング */}
                {virtualizer.getVirtualItems().length > 0 && (
                  <div
                    style={{
                      height: virtualizer.getTotalSize() - (virtualizer.getVirtualItems()[virtualizer.getVirtualItems().length - 1]?.end || 0)
                    }}
                  />
                )}
              </Box>
            </SortableContext>
          </DndContext>
        )}
      </Box>
    </Box>
  );
};
