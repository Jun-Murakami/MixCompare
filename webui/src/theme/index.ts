// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
import { createTheme } from '@mui/material/styles';
import '@fontsource/red-hat-mono/400.css';
import '@fontsource/jost/400.css';
import '@fontsource/jost/600.css';
import '@fontsource/jost/700.css';

export const darkTheme = createTheme({
  palette: {
    mode: 'dark',
    primary: {
      main: '#4fc3f7',
      light: '#8bf6ff',
      dark: '#0093c4',
    },
    secondary: {
      main: '#ffab00',
    },
    background: {
      default: '#606F77',
      paper: '#252525',
    },
    text: {
      primary: '#e0e0e0',
      secondary: '#a0a0a0',
    },
  },
  typography: {
    // 欧文は Jost、和文は欧文フォントに無いため次のファミリへグリフ単位でフォールバックする。
    // Linux/WebKitGTK では generic 'sans-serif' が CJK へ解決されず豆腐(□)になることがあるため、
    // OS 標準の日本語フォントを明示列挙する（プラグインへのフォント埋め込みはしない）。
    // 列挙順: 欧文(Jost他) → 和文(mac→Windows→Linux標準) → generic。
    // 存在しないファミリはブラウザが読み飛ばすため、全OS分を並べても害はない。
    fontFamily: [
      '"Jost"',
      '-apple-system',
      'BlinkMacSystemFont',
      '"Segoe UI"',
      'Roboto',
      '"Helvetica Neue"',
      'Arial',
      // --- macOS 標準和文 ---
      '"Hiragino Sans"',
      '"Hiragino Kaku Gothic ProN"',
      // --- Windows 標準和文 ---
      '"Yu Gothic UI"',
      '"Yu Gothic"',
      'Meiryo',
      '"MS PGothic"',
      // --- Linux 標準和文 ---
      '"Noto Sans CJK JP"',
      '"Noto Sans JP"',
      '"IPAexGothic"',
      '"VL Gothic"',
      '"BIZ UDGothic"',
      'sans-serif',
    ].join(', '),
    h6: {
      fontSize: '1.1rem',
      fontWeight: 500,
    },
  },
  shape: {
    borderRadius: 8,
  },
  components: {
    MuiButton: {
      styleOverrides: {
        root: {
          textTransform: 'none',
          fontWeight: 500,
        },
      },
    },
    MuiPaper: {
      styleOverrides: {
        root: {
          backgroundImage: 'none',
        },
      },
    },
    MuiSlider: {
      styleOverrides: {
        root: {
          height: 4,
          '& .MuiSlider-thumb': {
            width: 16,
            height: 16,
            '&:hover': {
              boxShadow: '0px 0px 0px 8px rgba(79, 195, 247, 0.16)',
            },
          },
          '& .MuiSlider-rail': {
            opacity: 0.3,
          },
        },
      },
    },
  },
});