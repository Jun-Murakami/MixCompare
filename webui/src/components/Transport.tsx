import React, { useState, useEffect } from 'react';
import { Box, IconButton, Typography, Slider, Tooltip, Button, InputBase, Popover, Paper } from '@mui/material';
import { PlayArrow, Pause, SkipPrevious, SkipNext, Repeat, Clear as ClearIcon, Sync } from '@mui/icons-material';
import { juceBridge } from '../bridge/juce';
import { type TransportState, type TrackChangeData } from '../types';
import { getToggleState, getSliderState } from 'juce-framework-frontend-mirror';

const formatTime = (seconds: number): string => {
  const mins = Math.floor(seconds / 60);
  const secs = Math.floor(seconds % 60);
  return `${mins.toString().padStart(2, '0')}:${secs.toString().padStart(2, '0')}`;
};

export const Transport: React.FC = () => {
  // JUCE からの更新イベントを直接購読する軽量ステート
  const [transportState, setTransportState] = useState<TransportState>({
    isPlaying: false,
    position: 0,
    loopStart: 0,
    loopEnd: 0,
    loopEnabled: false,
  });
  const [playlist, setPlaylist] = useState<Array<{ id: string; name: string; duration: number }>>([]);
  const [currentPlaylistIndex, setCurrentPlaylistIndex] = useState<number>(-1);
  const [isDragging, setIsDragging] = useState(false);
  const [tempPosition, setTempPosition] = useState(0);
  const [isSeekPending, setIsSeekPending] = useState(false);
  const isSeekPendingRef = React.useRef(false);
  const tempPositionRef = React.useRef(0);
  // 抑止窓は撤廃。セッションIDで厳密に破棄する
  // セッションID境界（trackChangeで更新）。未満のtransport更新は無視
  const currentSessionIdRef = React.useRef<number>(0);
  // 既存: suppressUntilRef は Playlist 用の抑止。Transport 専用は上記参照。

  // =============================
  // ループ範囲のインライン編集用状態
  // - 右上表示をクリックすると小さなポップオーバーで mm:ss（または秒）入力
  // - レイアウト崩れを避けるため、オーバーレイ表示で既存行を押し広げない
  // =============================
  const [isLoopEditing, setIsLoopEditing] = useState(false);
  const [loopEditAnchor, setLoopEditAnchor] = useState<HTMLElement | null>(null);
  const [loopInText, setLoopInText] = useState('');
  const [loopOutText, setLoopOutText] = useState('');

  // JUCE パラメータ直結トグル（再生/一時停止）
  const transportPlayingToggle = getToggleState('TRANSPORT_PLAYING');
  // ホスト同期トグル
  const hostSyncToggle = getToggleState('HOST_SYNC_ENABLED');
  // Sync capable は ToggleState が無い可能性がある（起動順次第）。なければtrue扱いにしてUIをブロックしない。
  const hostSyncCapableToggle = getToggleState('HOST_SYNC_CAPABLE');
  const isHostSync = !!hostSyncToggle?.getValue();
  // ネイティブのHOST_SYNC_CAPABLEを購読して有効/無効を切替（未到着時は一時的に有効）
  const [isHostSyncCapable, setIsHostSyncCapable] = useState<boolean>(true);
  useEffect(() => {
    const st = hostSyncCapableToggle;
    if (!st) return;
    setIsHostSyncCapable(!!st.getValue());
    const id = st.valueChangedEvent.addListener(() => setIsHostSyncCapable(!!st.getValue()));
    return () => st.valueChangedEvent.removeListener(id);
  }, [hostSyncCapableToggle]);
  const handlePlayPause = async () => {
    // 直前にHOST_SYNCがOFFへ切り替わった瞬間のクリックに備え、
    // 最新の同期状態をハンドラ実行時に直接参照して判定する。
    const hostSyncNow = !!hostSyncToggle?.getValue();
    if (hostSyncNow) return; // 同期中は無効
    const current = !!transportPlayingToggle?.getValue();
    transportPlayingToggle?.setValue(!current);
  };

  // ループのIn点へ即時ジャンプ（|<）
  const handleJumpToLoopStart = async () => {
    if (isHostSync) return; // 同期中は無効
    const target = transportState.loopStart || 0;
    const seekState = getSliderState('TRANSPORT_SEEK_NORM');
    const durationLocal = duration || 0;
    const norm = durationLocal > 0 ? Math.max(0, Math.min(1, target / durationLocal)) : 0;
    seekState?.setNormalisedValue(norm);
  };

  const handlePrevious = async () => {
    const num = playlist.length;
    if (num <= 0) return;
    const nextIndex = Math.max(0, (currentPlaylistIndex ?? 0) - 1);
    const norm = num > 1 ? nextIndex / (num - 1) : 0;
    getSliderState('PLAYLIST_CURRENT_INDEX_NORM')?.setNormalisedValue(Math.max(0, Math.min(1, norm)));
  };

  const handleNext = async () => {
    const num = playlist.length;
    if (num <= 0) return;
    const nextIndex = Math.min(num - 1, (currentPlaylistIndex ?? 0) + 1);
    const norm = num > 1 ? nextIndex / (num - 1) : 0;
    getSliderState('PLAYLIST_CURRENT_INDEX_NORM')?.setNormalisedValue(Math.max(0, Math.min(1, norm)));
  };

  // ループON/OFFのトグル（JUCEへ通知含む）
  // JUCE パラメータ直結トグル（ループON/OFF）
  const loopEnabledToggle = getToggleState('TRANSPORT_LOOP_ENABLED');
  const handleLoopToggle = async () => {
    if (isHostSync) return; // 同期中は無効
    const current = !!loopEnabledToggle?.getValue();
    loopEnabledToggle?.setValue(!current);
  };

  const handleMarkIn = async () => {
    if (isHostSync) return; // 同期中は無効
    const currentPos = transportState.position;
    const durationLocal = duration || 0;
    let loopEnd = transportState.loopEnd;
    if (!loopEnd || loopEnd <= 0) loopEnd = durationLocal > 0 ? durationLocal : currentPos + 10;
    const startNorm = durationLocal > 0 ? Math.max(0, Math.min(1, currentPos / durationLocal)) : 0;
    const endNorm = durationLocal > 0 ? Math.max(0, Math.min(1, loopEnd / durationLocal)) : 1;
    getSliderState('LOOP_START_NORM')?.setNormalisedValue(startNorm);
    getSliderState('LOOP_END_NORM')?.setNormalisedValue(endNorm);
  };

  const handleMarkOut = async () => {
    if (isHostSync) return; // 同期中は無効
    const currentPos = transportState.position;
    const durationLocal = duration || 0;
    const endNorm = durationLocal > 0 ? Math.max(0, Math.min(1, currentPos / durationLocal)) : 1;
    getSliderState('LOOP_END_NORM')?.setNormalisedValue(endNorm);
  };

  const handleClearLoop = async () => {
    if (isHostSync) return; // 同期中は無効
    getSliderState('LOOP_START_NORM')?.setNormalisedValue(0);
    getSliderState('LOOP_END_NORM')?.setNormalisedValue(1);
  };

  // mm:ss または 秒表記を秒数へ変換する簡易パーサ
  const parseTime = (text: string): number | null => {
    const s = (text || '').trim();
    if (/^\d{1,2}:\d{2}$/.test(s)) {
      const [m, sec] = s.split(':');
      const mins = parseInt(m, 10);
      const secs = parseInt(sec, 10);
      if (Number.isFinite(mins) && Number.isFinite(secs)) return Math.max(0, mins * 60 + secs);
      return null;
    }
    if (/^\d+(?:\.\d+)?$/.test(s)) {
      const v = parseFloat(s);
      return Number.isFinite(v) ? Math.max(0, v) : null;
    }
    return null;
  };

  // ループ表示クリックで編集開始
  const handleOpenLoopEdit = (e: React.MouseEvent<HTMLElement>) => {
    setLoopEditAnchor(e.currentTarget);
    setIsLoopEditing(true);
    setLoopInText(formatTime(transportState.loopStart));
    setLoopOutText(formatTime(transportState.loopEnd));
  };

  // 入力確定（Enter/Blur）
  const commitLoopEdit = () => {
    const durationLocal = currentItem?.duration || 0;
    let start = parseTime(loopInText);
    let end = parseTime(loopOutText);
    if (start == null) start = transportState.loopStart || 0;
    if (end == null) end = transportState.loopEnd || durationLocal;

    // 範囲の正規化（0..duration、start<=end）
    start = Math.max(0, Math.min(start, durationLocal));
    end = Math.max(start, Math.min(end, durationLocal));

    // 楽観的更新 + 正規化パラメータへ反映
    const sNorm = durationLocal > 0 ? start / durationLocal : 0;
    const eNorm = durationLocal > 0 ? end / durationLocal : 1;
    getSliderState('LOOP_START_NORM')?.setNormalisedValue(Math.max(0, Math.min(1, sNorm)));
    getSliderState('LOOP_END_NORM')?.setNormalisedValue(Math.max(0, Math.min(1, eNorm)));

    setIsLoopEditing(false);
    setLoopEditAnchor(null);
  };

  // 編集キャンセル（Esc）
  const cancelLoopEdit = () => {
    setIsLoopEditing(false);
    setLoopEditAnchor(null);
  };

  const handleSeekChange = (_: Event, value: number | number[]) => {
    if (isHostSync) return; // 同期中は無効
    const newPosition = value as number;
    setTempPosition(newPosition);
    tempPositionRef.current = newPosition;
    setIsDragging(true);
  };

  const handleSeekCommit = async (_: Event | React.SyntheticEvent, value: number | number[]) => {
    if (isHostSync) return; // 同期中は無効
    const newPosition = value as number;
    setIsDragging(false);
    setIsSeekPending(true);
    isSeekPendingRef.current = true;
    setTempPosition(newPosition);
    tempPositionRef.current = newPosition;

    // 正規化シークパラメータに送信（0..1）
    const durationLocal = duration || 0;
    const norm = durationLocal > 0 ? Math.max(0, Math.min(1, newPosition / durationLocal)) : 0;
    const seekState = getSliderState('TRANSPORT_SEEK_NORM');
    seekState?.setNormalisedValue(norm);
  };

  // 現在の選曲は playlist と currentPlaylistIndex から常に導出
  const currentItem = currentPlaylistIndex >= 0 && currentPlaylistIndex < playlist.length ? playlist[currentPlaylistIndex] : null;
  const duration = currentItem?.duration || 0;
  // プレイリストが空の場合は強制的に0、それ以外は通常の計算
  const displayPosition = !currentItem ? 0 : (isDragging || isSeekPending ? tempPosition : transportState.position);

  useEffect(() => {
    if (!isDragging && !isSeekPending) {
      setTempPosition(transportState.position);
    }
  }, [transportState.position, isDragging, isSeekPending]);

  useEffect(() => {
    isSeekPendingRef.current = isSeekPending;
  }, [isSeekPending]);

  // シーク確定後、バックエンドの位置が追いついたら pending を解除
  useEffect(() => {
    if (isSeekPending) {
      const eps = 0.01;
      if (Math.abs(transportState.position - tempPosition) <= eps) {
        setIsSeekPending(false);
      }
    }
  }, [transportState.position, tempPosition, isSeekPending]);

  // ループ区間のマーク位置を計算
  const loopStartPercent = duration > 0 ? (transportState.loopStart / duration) * 100 : 0;
  const loopEndPercent = duration > 0 ? (transportState.loopEnd / duration) * 100 : 100;

  // JUCE の transportUpdate / trackChange / playlistUpdate を購読してローカル状態を更新
  useEffect(() => {
    const transportId = juceBridge.addEventListener('transportUpdate', (d: unknown) => {
      const data = d as Partial<TransportState> & { sessionId?: number };
      // 古いセッションの更新は無視
      if (typeof data.sessionId === 'number' && data.sessionId < currentSessionIdRef.current) return;
      if (isSeekPendingRef.current && typeof data.position === 'number') {
        const diff = Math.abs(data.position - tempPositionRef.current);
        if (diff > 0.05) return;
      }
      setTransportState((prev) => ({
        isPlaying: data.isPlaying ?? prev.isPlaying,
        position: typeof data.position === 'number' ? data.position : prev.position,
        loopStart: typeof data.loopStart === 'number' ? data.loopStart : prev.loopStart,
        loopEnd: typeof data.loopEnd === 'number' ? data.loopEnd : prev.loopEnd,
        loopEnabled: typeof data.loopEnabled === 'boolean' ? data.loopEnabled : prev.loopEnabled,
      }));
      if (
        isSeekPendingRef.current &&
        typeof data.position === 'number' &&
        Math.abs(data.position - tempPositionRef.current) <= 0.05
      ) {
        setIsSeekPending(false);
      }
    });
    const transportPosId = juceBridge.addEventListener('transportPositionUpdate', (d: unknown) => {
      const data = d as { position?: number; isPlaying?: boolean; duration?: number; sessionId?: number };
      // 古いセッションの更新は無視
      if (typeof data.sessionId === 'number' && data.sessionId < currentSessionIdRef.current) return;
      if (isSeekPendingRef.current && typeof data.position === 'number') {
        const diff = Math.abs(data.position - tempPositionRef.current);
        if (diff > 0.05) return;
      }
      setTransportState((prev) => ({
        isPlaying: typeof data.isPlaying === 'boolean' ? data.isPlaying : prev.isPlaying,
        position: typeof data.position === 'number' ? data.position : prev.position,
        loopStart: prev.loopStart,
        loopEnd: prev.loopEnd,
        loopEnabled: prev.loopEnabled,
      }));
      if (
        isSeekPendingRef.current &&
        typeof data.position === 'number' &&
        Math.abs(data.position - tempPositionRef.current) <= 0.05
      ) {
        setIsSeekPending(false);
      }
    });
    // 初期同期や並べ替え/削除時のプレイリスト更新
    const playlistId = juceBridge.addEventListener('playlistUpdate', (d: unknown) => {
      const data = d as { items?: Array<{ id: string; name: string; duration: number }>; currentIndex?: number };
      if (data.items) {
        setPlaylist(data.items.map((x) => ({ id: x.id, name: x.name, duration: x.duration })));
      }
      if (typeof data.currentIndex === 'number') {
        setCurrentPlaylistIndex(data.currentIndex);
      }
    });
    const trackId = juceBridge.addEventListener('trackChange', (d: unknown) => {
      const data = d as TrackChangeData & { sessionId?: number };
      if (data.items) {
        setPlaylist(data.items.map((x) => ({ id: x.id, name: x.name, duration: x.duration })));
      }
      if (typeof data.currentIndex === 'number') {
        setCurrentPlaylistIndex(data.currentIndex);
      }
      if (typeof data.sessionId === 'number') {
        currentSessionIdRef.current = data.sessionId;
      }
      // トラック切替時は無条件で一時位置とシーク中フラグをリセット
      setTempPosition(0);
      setIsSeekPending(false);
      setIsDragging(false);
      tempPositionRef.current = 0;
      isSeekPendingRef.current = false;
      if (
        typeof data.isPlaying === 'boolean' ||
        typeof data.position === 'number' ||
        typeof data.loopEnabled === 'boolean' ||
        typeof data.loopStart === 'number' ||
        typeof data.loopEnd === 'number'
      ) {
        setTransportState((prev) => ({
          isPlaying: data.isPlaying ?? prev.isPlaying,
          // トラック切替は position=0 を即時反映
          position: typeof data.position === 'number' ? data.position : 0,
          loopStart: typeof data.loopStart === 'number' ? data.loopStart : prev.loopStart,
          loopEnd: typeof data.loopEnd === 'number' ? data.loopEnd : prev.loopEnd,
          loopEnabled: typeof data.loopEnabled === 'boolean' ? data.loopEnabled : prev.loopEnabled,
        }));
      }
    });
    return () => {
      juceBridge.removeEventListener(transportId);
      juceBridge.removeEventListener(transportPosId);
      juceBridge.removeEventListener(playlistId);
      juceBridge.removeEventListener(trackId);
    };
  }, []);

  return (
    <Box>
      {/* 1行目: 左=ファイル名 / 右=時間・ループ情報 */}
      <Box sx={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', mt: 1, height: 24 }}>
        <Typography variant='body2' color='text.secondary' noWrap sx={{ minWidth: 0, pr: 1 }}>
          {currentItem?.name || 'No track selected'}
        </Typography>
        <Box sx={{ display: 'flex', alignItems: 'center', gap: 0.5 }}>
          <Typography variant='caption' sx={{ fontFamily: '"Red Hat Mono", monospace' }} noWrap>
            {formatTime(displayPosition)}
            <span style={{ margin: '0 0.2em' }}>/</span>
            {formatTime(duration)}
          </Typography>
          {transportState.loopEnabled && (
            <Box sx={{ display: 'flex', alignItems: 'center', gap: 0.25 }}>
              {/*
               * ループ編集中はツールチップを即時クローズし、再表示も抑止する。
               * open を isLoopEditing=true のときだけ false（制御モード）にし、
               * それ以外は undefined（非制御）に戻すことで通常のホバー挙動を維持。
               */}
              <Tooltip
                title='Click to edit loop (mm:ss or sec)'
                arrow
                slotProps={{
                  popper: {
                    sx: { display: isLoopEditing ? 'none' : 'block' },
                  },
                }}
              >
                <Typography
                  variant='caption'
                  color='text.secondary'
                  sx={{ fontFamily: '"Red Hat Mono", monospace', cursor: 'text', '&:hover': { textDecoration: 'underline' } }}
                  noWrap
                  onClick={handleOpenLoopEdit}
                >
                  <Repeat fontSize='small' sx={{ color: 'text.secondary', fontSize: 13, mb: -0.3 }} />
                  {formatTime(transportState.loopStart)}
                  <span style={{ letterSpacing: '-0.5em' }}> </span>-<span style={{ letterSpacing: '-0.5em' }}> </span>
                  {formatTime(transportState.loopEnd)}
                </Typography>
              </Tooltip>
              {/* リセットはポップオーバー内へ移動 */}
            </Box>
          )}
        </Box>
      </Box>

      {/* 2行目: シークバー */}
      <Box sx={{ display: 'flex', alignItems: 'center', gap: 0.1, mx: 0.1 }}>
        {/* 残りはシークバー */}
        <Box sx={{ flex: 1, position: 'relative' }}>
          {/* ループ区間の視覚化（トラック中央に揃える） */}
          {transportState.loopEnabled && (
            <Box
              sx={{
                position: 'absolute',
                top: '38%',
                left: `${loopStartPercent}%`,
                right: `${100 - loopEndPercent}%`,
                height: 20,
                transform: 'translateY(-50%)',
                backgroundColor: 'rgba(79,195,247,0.25)',
                borderLeft: 'solid 1.5px rgba(255,255,255,0.5)',
                borderRight: 'solid 1.5px rgba(255,255,255,0.5)',
                borderRadius: 0.25,
                pointerEvents: 'none',
                zIndex: 100,
              }}
            />
          )}
          <Slider
            key={`seek-${currentPlaylistIndex}-${currentItem?.id || 'empty'}`}
            value={displayPosition}
            max={duration}
            onChange={handleSeekChange}
            onChangeCommitted={handleSeekCommit}
            disabled={!currentItem || duration === 0 || isHostSync}
            sx={{
              my: -1,
              height: 6,
              /*
               * サム（thumb）をホバー時以外は非表示にする。
               * - トラック/レール上にマウスが乗ったとき（:hover）は表示
               * - キーボード操作でフォーカスが当たったとき（.Mui-focusVisible）も表示
               * - ドラッグ中（.Mui-active）は常に表示
               * これにより常時の視覚ノイズを抑えつつ、操作時の視認性を確保する。
               */
              '& .MuiSlider-thumb': {
                width: 12,
                height: 12,
                transition: 'opacity 80ms',
                opacity: 0,
              },
              '&:hover .MuiSlider-thumb, & .MuiSlider-thumb.Mui-focusVisible, & .MuiSlider-thumb.Mui-active': {
                opacity: 1,
              },
              '& .MuiSlider-track': { height: 3, transition: 'none', color: isHostSync ? 'secondary.main' : 'primary.main' },
              '& .MuiSlider-rail': { height: 3, opacity: 0.5 },
            }}
          />
        </Box>
      </Box>

      {/* 3行目: 操作ボタン */}
      <Box sx={{ display: 'flex', alignItems: 'center', gap: 0.1, mx: 'auto', justifyContent: 'center', mb: 0.75 }}>
        <Tooltip title='Previous' arrow>
          <IconButton onClick={handlePrevious} size='small' sx={{ height: 28, width: 28 }}>
            <SkipPrevious />
          </IconButton>
        </Tooltip>

        <Tooltip title={transportState.isPlaying ? 'Pause' : 'Play'} arrow>
          <span style={{ display: 'inline-block' }}>
            <IconButton
              onClick={handlePlayPause}
              size='small'
              color={transportState.isPlaying ? 'primary' : 'default'}
              sx={{ '&:hover': { backgroundColor: 'action.hover' }, height: 28, width: 28 }}
              disabled={isHostSync}
            >
              {transportState.isPlaying ? <Pause /> : <PlayArrow />}
            </IconButton>
          </span>
        </Tooltip>

        <Tooltip title='Next' arrow>
          <IconButton onClick={handleNext} size='small' sx={{ height: 28, width: 28 }}>
            <SkipNext />
          </IconButton>
        </Tooltip>

        {/* Stop ボタンは廃止 */}

        {/* Loop コンパクトグループ（枠で囲って省スペース化） */}
        <Box
          sx={{
            display: 'flex',
            alignItems: 'center',
            gap: 0.1,
            pr: 0.5,
            py: 0,
            ml: 0.25,
            mr: 0.75,
            border: '1px solid',
            borderColor: transportState.loopEnabled ? 'primary.main' : 'divider',
            borderRadius: 1,
          }}
        >
          <Tooltip title='Loop On / Off' arrow>
            <span style={{ display: 'inline-block' }}>
              <Button
                onClick={handleLoopToggle}
                size='small'
                startIcon={<Repeat fontSize='small' sx={{ mr: -0.75 }} />}
                sx={{ color: transportState.loopEnabled ? 'primary' : 'text.primary', mr: -0.5 }}
                disabled={isHostSync}
              >
                Loop
              </Button>
            </span>
          </Tooltip>
          {/* ループInへジャンプ（|<） */}
          <Tooltip title='Jump to In' arrow>
            <span style={{ display: 'inline-block' }}>
              <IconButton
                onClick={handleJumpToLoopStart}
                size='small'
                sx={{ p: 0.5 }}
                color={transportState.loopEnabled ? 'primary' : 'default'}
                disabled={!transportState.loopEnabled || isHostSync}
              >
                <SkipPrevious fontSize='small' />
              </IconButton>
            </span>
          </Tooltip>
          <Tooltip title='Mark In' arrow>
            <span style={{ display: 'inline-block' }}>
              <Button
                onClick={handleMarkIn}
                size='small'
                variant='text'
                color={transportState.loopEnabled ? 'primary' : 'inherit'}
                sx={{
                  minWidth: 'auto',
                  px: 0.5,
                  height: 24,
                  textTransform: 'none',
                  lineHeight: 1.2,
                  fontWeight: 600,
                }}
                disabled={!transportState.loopEnabled || isHostSync}
              >
                In
              </Button>
            </span>
          </Tooltip>
          <Tooltip title='Mark Out' arrow>
            <span style={{ display: 'inline-block' }}>
              <Button
                onClick={handleMarkOut}
                size='small'
                variant='text'
                color={transportState.loopEnabled ? 'primary' : 'inherit'}
                sx={{
                  minWidth: 'auto',
                  px: 0.5,
                  height: 24,
                  textTransform: 'none',
                  lineHeight: 1.2,
                  fontWeight: 600,
                }}
                disabled={!transportState.loopEnabled || isHostSync}
              >
                Out
              </Button>
            </span>
          </Tooltip>
        </Box>
        {/* ホストシンクボタン */}
        <Box
          sx={{
            display: 'flex',
            alignItems: 'center',
            gap: 0.1,
            py: 0,
            border: '1px solid',
            borderColor: isHostSync ? 'secondary.main' : 'divider',
            borderRadius: 1,
          }}
        >
          <Tooltip title='Sync to Host for A/B compare' arrow>
            <span style={{ display: 'inline-block' }}>
              <Button
                size='small'
                sx={{
                  py: 0.5,
                  px: 1,
                  color: isHostSync ? 'secondary.main' : 'text.primary',
                  opacity: isHostSyncCapable ? 1 : 0.4,
                }}
                startIcon={<Sync fontSize='small' sx={{ mr: -1 }} />}
                onClick={() => isHostSyncCapable && hostSyncToggle?.setValue(!isHostSync)}
                disabled={!isHostSyncCapable}
              >
                Sync to Host
              </Button>
            </span>
          </Tooltip>
        </Box>
      </Box>

      {/* ループ編集ポップオーバー（小型） */}
      <Popover
        open={isLoopEditing}
        anchorEl={loopEditAnchor}
        onClose={commitLoopEdit}
        anchorOrigin={{ vertical: 'top', horizontal: 'right' }}
        transformOrigin={{ vertical: 'bottom', horizontal: 'right' }}
      >
        <Paper sx={{ display: 'flex', alignItems: 'center', gap: 0.5, p: 1, backgroundColor: 'grey.800' }} elevation={4}>
          <Typography variant='caption'>Loop Range:</Typography>
          {/* In 入力 */}
          <InputBase
            className='block-host-shortcuts'
            value={loopInText}
            onChange={(e) => setLoopInText(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === 'Enter') commitLoopEdit();
              if (e.key === 'Escape') cancelLoopEdit();
            }}
            sx={{
              width: 54,
              px: 0.5,
              py: 0.2,
              fontFamily: '"Red Hat Mono", monospace',
              fontSize: '0.8rem',
              border: '1px solid',
              borderColor: 'divider',
              borderRadius: 0.5,
            }}
            autoFocus
            inputProps={{ 'aria-label': 'Loop In (mm:ss or sec)' }}
          />
          <Typography variant='caption' sx={{ fontFamily: '"Red Hat Mono", monospace' }}>
            -
          </Typography>
          {/* Out 入力 */}
          <InputBase
            className='block-host-shortcuts'
            value={loopOutText}
            onChange={(e) => setLoopOutText(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === 'Enter') commitLoopEdit();
              if (e.key === 'Escape') cancelLoopEdit();
            }}
            onBlur={() => {
              // ポップオーバー内のフォーカス移動で即閉じないよう、短い遅延後に確定
              setTimeout(() => {
                const popover = document.querySelector('[role="dialog"]');
                if (popover && popover.contains(document.activeElement)) return;
                commitLoopEdit();
              }, 10);
            }}
            sx={{
              width: 54,
              px: 0.5,
              py: 0.2,
              fontFamily: '"Red Hat Mono", monospace',
              fontSize: '0.8rem',
              border: '1px solid',
              borderColor: 'divider',
              borderRadius: 0.5,
            }}
            inputProps={{ 'aria-label': 'Loop Out (mm:ss or sec)' }}
          />
          {/* Reset ボタン（ここで範囲を初期化して閉じる） */}
          <Button
            size='small'
            variant='outlined'
            color='inherit'
            onClick={() => {
              handleClearLoop();
              cancelLoopEdit();
            }}
            sx={{ ml: 0.5, height: 28, textTransform: 'none', lineHeight: 1.1, px: 1 }}
            startIcon={<ClearIcon fontSize='small' sx={{ mr: -0.5 }} />}
          >
            Reset
          </Button>
        </Paper>
      </Popover>
    </Box>
  );
};
