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
  DragOverlay,
  type DragEndEvent,
  type DragMoveEvent,
  type DragStartEvent,
} from '@dnd-kit/core';
import { SortableContext, sortableKeyboardCoordinates, verticalListSortingStrategy } from '@dnd-kit/sortable';
// useSortable, CSS は PlaylistItem に移動
import { restrictToVerticalAxis } from '@dnd-kit/modifiers';
import { Add, Clear, Save } from '@mui/icons-material';
import { Menu, MenuItem } from '@mui/material';
import { Virtualizer, type VirtualizerHandle } from 'virtua';
import { juceBridge } from '../bridge/juce';
import { type PlaylistItem } from '../types';
import PlaylistItemRow, { PlaylistItemOverlay } from './PlaylistItem';

// 上記 SortableItem は PlaylistItem.tsx に分離

// React Compiler(eslint react-hooks/purity) は Date.now() を「描画中に呼ぶと不安定」と
// 判定するが、本コンポーネントでの呼び出しは全てイベントハンドラ/Effect 内で安全。
// 解析対象外のモジュールスコープ関数に隔離して誤検知を避ける。
const nowMs = (): number => Date.now();

export const Playlist: React.FC = () => {
  // ローカル状態（ストアから移行）
  const [playlist, setPlaylist] = React.useState<PlaylistItem[]>([]);
  const [currentPlaylistIndex, setCurrentPlaylistIndex] = React.useState<number>(-1);
  const listContainerRef = useRef<HTMLDivElement>(null);
  const virtualizerRef = useRef<VirtualizerHandle>(null);
  const [anchorEl, setAnchorEl] = React.useState<null | HTMLElement>(null);
  // ドラッグ中のアイテム id（DragOverlay のゴースト描画用）
  const [activeId, setActiveId] = React.useState<string | null>(null);
  // JUCE更新を楽観操作の直後に抑制するための期限
  const suppressUntilRef = React.useRef<number>(0)
  // ドラッグ操作中フラグ。ドラッグ中は JUCE からの 20〜60Hz の playlistUpdate を
  // 適用せず、選択位置への自動スクロールも止める（オートスクロールとの競合＝
  // 「端まで行くと元の位置に戻る」ループを防ぐ）。
  const isDraggingRef = React.useRef<boolean>(false)
  // ドラッグ中の自前オートスクロール。dnd-kit 組み込みのオートスクロールは
  // ドラッグ開始時にキャッシュした矩形を基にスクロール量を決めるため、virtua が
  // 高さを動的補正すると過剰スクロール→巻き戻しのループになる。これを切り、
  // virtua が所有するスクロールコンテナを毎フレーム「現在値 + delta」で直接送る。
  const autoScrollRef = React.useRef<{ speed: number; raf: number }>({ speed: 0, raf: 0 })

  // 仮想化: virtua の Virtualizer を利用（React Compiler 互換）。
  // スクロール要素は下部の listContainerRef、各行はほぼ固定高（約 20px）。

  // Auto-scroll to selected item when index changes (only if out of view)
  useEffect(() => {
    // ドラッグ中は自動スクロールしない（dnd-kit のオートスクロールと scrollTop を
    // 奪い合ってループになるため）。
    if (isDraggingRef.current) return;
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
          virtualizerRef.current?.scrollToIndex(currentPlaylistIndex, {
            align: 'center',
            smooth: true,
          });
        }
      }
    }
  }, [currentPlaylistIndex, playlist.length]);

  // JUCE playlistUpdate / trackChange を購読してローカル更新
  useEffect(() => {
    const playlistId = juceBridge.addEventListener('playlistUpdate', (data: unknown) => {
      const d = data as { items?: PlaylistItem[]; currentIndex?: number; revision?: number };
      // ドラッグ中は一切適用しない。currentIndex を更新すると自動スクロール effect が
      // 走り、items を差し替えると dnd-kit の並べ替え状態がリセットされて競合する。
      if (isDraggingRef.current) return;
      // 抑制期間中でも currentIndex は反映して選択状態をずらさない
      if (typeof d.currentIndex === 'number') setCurrentPlaylistIndex(d.currentIndex);
      // items の差し替えは抑制期間が過ぎてから適用（並べ替えの楽観更新と競合しないように）
      if (Array.isArray(d.items)) {
        if (nowMs() >= suppressUntilRef.current) {
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

  // アンマウント時に自前オートスクロールの RAF を確実に止める
  useEffect(() => {
    return () => {
      if (autoScrollRef.current.raf) cancelAnimationFrame(autoScrollRef.current.raf);
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

  // 自前オートスクロールの RAF ループ。毎フレーム「現在の scrollTop + speed」を書く。
  // virtua が補正してもその時点の scrollTop から続けるだけなので奪い合いにならない。
  const stepAutoScroll = () => {
    const c = listContainerRef.current;
    const { speed } = autoScrollRef.current;
    if (c && speed !== 0) c.scrollTop += speed;
    autoScrollRef.current.raf = requestAnimationFrame(stepAutoScroll);
  };

  const stopAutoScroll = () => {
    autoScrollRef.current.speed = 0;
    if (autoScrollRef.current.raf) cancelAnimationFrame(autoScrollRef.current.raf);
    autoScrollRef.current.raf = 0;
  };

  const handleDragStart = (event: DragStartEvent) => {
    isDraggingRef.current = true;
    setActiveId(String(event.active.id));
    autoScrollRef.current.speed = 0;
    if (!autoScrollRef.current.raf) {
      autoScrollRef.current.raf = requestAnimationFrame(stepAutoScroll);
    }
  };

  const handleDragMove = (event: DragMoveEvent) => {
    const c = listContainerRef.current;
    if (!c) return;
    // 現在のポインタ Y = 押下時の clientY + これまでの移動量
    const activator = event.activatorEvent as PointerEvent;
    const pointerY = (activator?.clientY ?? 0) + event.delta.y;
    const rect = c.getBoundingClientRect();
    const zone = 36; // 端からこの px 内に入ったらスクロール
    const maxSpeed = 10; // px / frame
    let speed = 0;
    if (pointerY < rect.top + zone) {
      const depth = Math.min(1, (rect.top + zone - pointerY) / zone);
      speed = -depth * maxSpeed;
    } else if (pointerY > rect.bottom - zone) {
      const depth = Math.min(1, (pointerY - (rect.bottom - zone)) / zone);
      speed = depth * maxSpeed;
    }
    autoScrollRef.current.speed = speed;
  };

  const handleDragCancel = () => {
    isDraggingRef.current = false;
    setActiveId(null);
    stopAutoScroll();
  };

  const handleDragEnd = async (event: DragEndEvent) => {
    isDraggingRef.current = false;
    setActiveId(null);
    stopAutoScroll();
    const { active, over } = event;

    if (over && active.id !== over.id) {
      const oldIndex = playlist.findIndex((item) => item.id === active.id);
      const newIndex = playlist.findIndex((item) => item.id === over.id);

      // JUCEからの更新を一時的に抑制（300ms）
      suppressUntilRef.current = nowMs() + 300;

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
    suppressUntilRef.current = nowMs() + 200;
    // 直接パラメータにバインド（0..1 正規化）
    const norm = playlist.length > 1 ? index / (playlist.length - 1) : 0;
    getSliderState('PLAYLIST_CURRENT_INDEX_NORM')?.setNormalisedValue(Math.max(0, Math.min(1, norm)));
  };

  const handleRemove = async (id: string) => {
    suppressUntilRef.current = nowMs() + 300;
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
    suppressUntilRef.current = nowMs() + 300;
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
    suppressUntilRef.current = nowMs() + 300;

    await juceBridge.callNative('playlist_action', 'import');
    handleMenuClose();
  };

  const handleMenuOpen = (event: React.MouseEvent<HTMLElement>) => {
    setAnchorEl(event.currentTarget);
  };

  const handleMenuClose = () => {
    setAnchorEl(null);
  };

  // ドラッグ中アイテムの index。virtua の keepMounted に渡し、オートスクロールで
  // 画面外へ出てもアンマウントさせない（dnd-kit のアクティブ要素を失わせない）。
  const activeIndex = activeId ? playlist.findIndex((it) => it.id === activeId) : -1;

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
            // 組み込みオートスクロールは virtua と競合してループするため切り、
            // onDragMove + RAF の自前オートスクロールで代替する。
            autoScroll={false}
            onDragStart={handleDragStart}
            onDragMove={handleDragMove}
            onDragEnd={handleDragEnd}
            onDragCancel={handleDragCancel}
            // virtua は各行を position:absolute の 1 行分ラッパーで包む。
            // restrictToParentElement はドラッグ対象の「親要素」（＝その 1 行分ラッパー）に
            // 移動をクランプするため、行を全く動かせず DnD が壊れる。
            // 仮想化リストでは縦方向固定のみで十分。
            modifiers={[restrictToVerticalAxis]}
          >
            <SortableContext items={playlist.map((item) => item.id)} strategy={verticalListSortingStrategy}>
              {/*
               * virtua の Virtualizer は scrollRef のスクロール要素内で可視範囲のみを描画する。
               * 行の前に他要素は無いので startMargin=0。各行はほぼ固定高なので itemSize=20 をヒントに与える。
               * SortableContext には全 id を渡しているので、未描画行があっても DnD の整合は保たれる。
               * keepMounted: ドラッグ中の行はオートスクロールで画面外に出ても常時マウントし、
               * dnd-kit のアクティブ要素（とその親矩形）を失わせない＝スクロールのループを防ぐ。
               */}
              <Virtualizer
                ref={virtualizerRef}
                scrollRef={listContainerRef}
                startMargin={0}
                itemSize={20}
                bufferSize={100}
                keepMounted={activeIndex >= 0 ? [activeIndex] : undefined}
              >
                {playlist.map((item, index) => (
                  <PlaylistItemRow
                    key={item.id}
                    item={item}
                    index={index}
                    isActive={index === currentPlaylistIndex}
                    onSelect={handleSelect}
                    onRemove={handleRemove}
                  />
                ))}
              </Virtualizer>
            </SortableContext>
            {/*
             * ドラッグ中のゴーストは DragOverlay でポータル描画し、カーソルに追従させる。
             * リスト内の元の行は virtua がスクロールで unmount しうるが、ゴーストは
             * 独立して描画されるので消えない。縦方向のみ追従させる。
             */}
            <DragOverlay modifiers={[restrictToVerticalAxis]} dropAnimation={null}>
              {activeIndex >= 0 ? (
                <PlaylistItemOverlay item={playlist[activeIndex]} isActive={activeIndex === currentPlaylistIndex} />
              ) : null}
            </DragOverlay>
          </DndContext>
        )}
      </Box>
    </Box>
  );
};
