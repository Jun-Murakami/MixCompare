import { ThemeProvider } from '@mui/material/styles';
import CssBaseline from '@mui/material/CssBaseline';
import { Box, Paper, Typography, Button, Menu, MenuItem } from '@mui/material';
import { useRef, useEffect, useState } from 'react';
import { juceBridge } from './bridge/juce';
import { darkTheme } from './theme';
import { SourceSelector } from './components/SourceSelector';
import { Transport } from './components/Transport';
import { Playlist } from './components/Playlist';
import { VUMeter } from './components/VUMeter';
import { Controls } from './components/Controls';
import { useHostShortcutForwarding } from './hooks/useHostShortcutForwarding';
import { GlobalDialog } from './components/GlobalDialog';
import { showErrorDialog, showWarningDialog, showInfoDialog } from './store/dialogStore';
import LicenseDialog from './components/LicenseDialog';
import './App.css';

function App() {
  // JUCEイベントリスナーを初期化
  useHostShortcutForwarding();
  const dragState = useRef<{ startX: number; startY: number; startW: number; startH: number } | null>(null);

  // 開発用：エラーテストメニュー
  const [errorMenuAnchor, setErrorMenuAnchor] = useState<null | HTMLElement>(null);
  const isDebugMode = import.meta.env.DEV; // Vite開発モードでのみ表示
  const handleStyle: React.CSSProperties = {
    position: 'fixed',
    right: 0,
    bottom: 0,
    width: 24,
    height: 24,
    cursor: 'nwse-resize',
    zIndex: 2147483647,
    backgroundColor: 'transparent',
    backgroundRepeat: 'no-repeat',
    backgroundSize: '24px 24px',
    backgroundPosition: 'bottom right',
  };

  // WebUI右下にリサイズハンドルを設置し、ドラッグで window_action.resizeTo を呼ぶ
  const onDragStart: React.PointerEventHandler<HTMLDivElement> = (e) => {
    dragState.current = { startX: e.clientX, startY: e.clientY, startW: window.innerWidth, startH: window.innerHeight };
    e.currentTarget.setPointerCapture(e.pointerId);
  };
  const onDrag: React.PointerEventHandler<HTMLDivElement> = (e) => {
    if (!dragState.current) return;
    const dx = e.clientX - dragState.current.startX;
    const dy = e.clientY - dragState.current.startY;
    const w = Math.max(392, dragState.current.startW + dx);
    const h = Math.max(610, dragState.current.startH + dy);
    // 高頻度呼び出しをrAFでスロットル
    if (!window.__resizeRAF) {
      window.__resizeRAF = requestAnimationFrame(() => {
        window.__resizeRAF = 0;
        juceBridge.callNative('window_action', 'resizeTo', w, h);
      });
    }
  };
  const onDragEnd: React.PointerEventHandler<HTMLDivElement> = () => {
    dragState.current = null;
  };

  // テストエラー発生関数
  const triggerTestError = (errorType: string) => {
    juceBridge.callNative('system_action', 'test_error', errorType);
    setErrorMenuAnchor(null);
  };

  // ============================
  // コンテキストメニュー（右クリック）と選択の全体無効化
  // ============================
  // - オーディオプラグインのWebUIでは、意図しないコンテキストメニューやテキスト選択を防ぐ
  // - ただし、ユーザー入力が必要な要素は例外（input/textarea/select/contenteditable/.allow-*）
  // - 開発モードでは右クリックを許可（開発者ツール用）
  useEffect(() => {
    // 開発モードかどうかを判定
    // - Vite の import.meta.env.DEV に加えて、WebView で dev サーバ (127.0.0.1/localhost:5173) を読み込んでいる場合も許可
    const isDevelopment = import.meta.env.DEV || location.hostname === '127.0.0.1' || location.hostname === 'localhost';

    juceBridge.whenReady(() => {
      juceBridge.callNative('system_action', 'ready');
    });

    // 右クリック（contextmenu）を全体で抑止し、許可対象は通す
    const onContextMenu = (e: MouseEvent) => {
      // 開発モードでは右クリックを許可
      if (isDevelopment) return;

      const t = e.target as HTMLElement | null;
      if (!t) return;
      if (t.closest('input, textarea, select, [contenteditable="true"], .allow-contextmenu')) return;
      e.preventDefault();
    };

    // 選択開始を抑止（CSSのuser-select: noneの保険）
    const onSelectStart = (e: Event) => {
      const t = e.target as HTMLElement | null;
      if (!t) return;
      if (t.closest('input, textarea, select, [contenteditable="true"], .allow-selection')) return;
      e.preventDefault();
    };

    window.addEventListener('contextmenu', onContextMenu, { capture: true });
    document.addEventListener('selectstart', onSelectStart, { capture: true });
    return () => {
      const captureOptions: AddEventListenerOptions = { capture: true };
      window.removeEventListener('contextmenu', onContextMenu, captureOptions);
      document.removeEventListener('selectstart', onSelectStart, captureOptions);
    };
  }, []);

  // エラー通知（backend -> frontend）を購読してグローバルダイアログを表示
  useEffect(() => {
    const listenerId = juceBridge.addEventListener('errorNotification', (d: unknown) => {
      // JUCE 側から送られるエラー情報を受け取り、可能ならば失敗したファイル情報を詳細に付与する
      // - ErrorInfo { severity, message, details, filePath } を想定
      const data = d as { severity?: string; message?: string; details?: string; filePath?: string };
      const sev = (data?.severity || 'error').toLowerCase();
      const msg = data?.message || 'An error occurred';
      const det = data?.details || '';
      const fp = (data?.filePath || '').toString();
      // details にファイルパス情報を追記（存在する場合）。
      // - 1行目: 既存 details
      // - 2行目: File: <ファイル名>
      // - 3行目: Path: <フルパス>
      const fileName = fp ? fp.split(/[/\\]/).pop() || fp : '';
      const enrichedDetails = fp ? [det, `File: ${fileName}`, `Path: ${fp}`].filter(Boolean).join('\n') : det;

      if (sev === 'warning') {
        showWarningDialog(msg, enrichedDetails);
      } else if (sev === 'info') {
        showInfoDialog(msg, enrichedDetails);
      } else {
        // 'error' / 'critical' / その他はエラー扱い
        showErrorDialog(msg, enrichedDetails);
      }
    });
    return () => juceBridge.removeEventListener(listenerId);
  }, []);

  // 独立ダイアログの開閉
  const [licenseOpen, setLicenseOpen] = useState(false);
  const openLicenseDialog = () => setLicenseOpen(true);
  const closeLicenseDialog = () => setLicenseOpen(false);

  return (
    <ThemeProvider theme={darkTheme}>
      <CssBaseline />
      {/* コーナーマークを ::after で描画（ドット3つを斜めに並べる） */}
      <style>{`
        #resizeHandle::after {
          content: '';
          position: absolute;
          right: 4px;
          top: 8px;
          width: 2px;
          height: 2px;
          background: rgba(79, 195, 247, 1);
          border-radius: 1px;
          pointer-events: none;
          box-shadow:
            -4px 4px 0 0 rgba(79, 195, 247, 1),
            -8px 8px 0 0 rgba(79, 195, 247, 1),
            -1px 7px 0 0 rgba(79, 195, 247, 1);
        }

        /* グローバルで選択禁止（保険としてCSSも適用） */
        html, body, #root {
          -webkit-user-select: none;
          -ms-user-select: none;
          user-select: none;
        }
        /* 入力要素と許可クラスでは選択可能 */
        input, textarea, select, [contenteditable="true"], .allow-selection {
          -webkit-user-select: text !important;
          -ms-user-select: text !important;
          user-select: text !important;
          caret-color: auto;
        }
      `}</style>
      <Box sx={{ flexGrow: 1, height: '100vh', display: 'flex', flexDirection: 'column', overflow: 'hidden', p: 2, pt: 0 }}>
        <Box sx={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', px: 1, py: 0.5 }}>
          <Typography
            variant='body2'
            component='div'
            sx={{ flexGrow: 1, color: 'primary.main', fontWeight: 600, cursor: 'pointer' }}
            onClick={openLicenseDialog}
            title='Licenses'
          >
            MixCompare
          </Typography>
          {/* 右側の表記を "by Jun Murakami" に変更し、クリックでダイアログ */}
          <Typography
            variant='caption'
            color='text.secondary'
            onClick={openLicenseDialog}
            sx={{ cursor: 'pointer' }}
            title='Licenses'
          >
            by Jun Murakami
          </Typography>
        </Box>

        {/**
         * メインの可変領域。
         * - 親（App 直下の Box）は縦方向フレックス
         * - この Paper 自体を flex:1 にして、下のメータ/コントロールの高さを差し引いた残りを埋める
         * - minHeight:0 を入れることで、内部のスクロール領域が正しく縮む（Flexbox の既定 min-content 回避）
         */}
        <Paper elevation={2} sx={{ p: 1, mb: 2, display: 'flex', flexDirection: 'column', flex: 1, minHeight: 0 }}>
          {/**
           * プレイリスト部分は可変。内部でさらにスクロールするため、
           * ここでも flex:1 と minHeight:0 を指定して高さ配分の起点にする。
           */}
          <Box sx={{ flex: 1, minHeight: 0, overflow: 'hidden' }}>
            <Playlist />
          </Box>
          {/** トランスポートは固定行として下に配置 */}
          <Transport />
        </Paper>

        <SourceSelector />

        <Box sx={{ width: '100%', display: 'flex', justifyContent: 'center' }}>
          <Box sx={{ width: '100%', maxWidth: 500 }}>
            <VUMeter />
            <Controls />
          </Box>
        </Box>
        {/* 右下リサイズハンドル。WebView前面で操作できる */}
        <div
          id='resizeHandle'
          onPointerDown={onDragStart}
          onPointerMove={onDrag}
          onPointerUp={onDragEnd}
          style={handleStyle}
          title='Resize'
        />
      </Box>

      {/* 開発用：エラーテストボタン（ホバーで表示） */}
      {isDebugMode && (
        <>
          <Button
            variant='contained'
            size='small'
            color='error'
            onClick={(e) => setErrorMenuAnchor(e.currentTarget)}
            sx={{
              position: 'fixed',
              top: 8,
              right: 8,
              zIndex: 9999,
              minWidth: 'auto',
              padding: '4px 8px',
              fontSize: '0.75rem',
              opacity: 0,
              transition: 'opacity 0.2s',
              '&:hover': {
                opacity: 1,
              },
            }}
          >
            Test Errors
          </Button>
          <Menu anchorEl={errorMenuAnchor} open={Boolean(errorMenuAnchor)} onClose={() => setErrorMenuAnchor(null)}>
            <MenuItem onClick={() => triggerTestError('file_not_found')}>File Not Found</MenuItem>
            <MenuItem onClick={() => triggerTestError('format_not_supported')}>Format Not Supported</MenuItem>
            <MenuItem onClick={() => triggerTestError('file_corrupted')}>File Corrupted</MenuItem>
            <MenuItem onClick={() => triggerTestError('file_too_large')}>File Too Large</MenuItem>
            <MenuItem onClick={() => triggerTestError('memory')}>Out of Memory</MenuItem>
            <MenuItem divider />
            <MenuItem onClick={() => triggerTestError('warning')}>Warning (Sample Rate)</MenuItem>
            <MenuItem onClick={() => triggerTestError('info')}>Info Message</MenuItem>
            <MenuItem onClick={() => triggerTestError('critical')}>Critical Error</MenuItem>
          </Menu>
        </>
      )}

      <LicenseDialog open={licenseOpen} onClose={closeLicenseDialog} />
      <GlobalDialog />
    </ThemeProvider>
  );
}

export default App;
