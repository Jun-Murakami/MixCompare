import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import { viteSingleFile } from 'vite-plugin-singlefile'
import { readFileSync } from 'fs'
import { fileURLToPath } from 'url'
import { dirname, resolve as resolvePath } from 'path'

// 現在のモジュールURLから __dirname 相当を構築（ESM では __dirname が存在しないため）
const __filename = fileURLToPath(import.meta.url)
const __dirname = dirname(__filename)

// ルートの VERSION を安全に読む（存在しない場合はフォールバック）
let fullVersion = '0.0.0'
try {
  fullVersion = readFileSync(resolvePath(__dirname, '../VERSION'), 'utf-8').trim()
} catch {
  console.warn('VERSION file not found, using default version')
}

// package.json のバージョンを取得
const packageJson = JSON.parse(readFileSync(resolvePath(__dirname, 'package.json'), 'utf-8'))

// iPlug2 プラグインビルド用 Vite 設定。
//  - juce-framework-frontend-mirror を iPlug2 互換 shim にエイリアスし、既存 React
//    コンポーネントを無改変のまま iPlug2 の WebView ブリッジに載せる。
//  - 本番ビルドは vite-plugin-singlefile で単一 index.html を生成し、
//    plugin/resources/web/ に出力 → main.rc が RCDATA で EXE/DLL に埋め込む。
//  - Web デモ (vite.config.web.ts) はこの設定と独立 (本家 shim 経路を維持)。
// https://vite.dev/config/
export default defineConfig(({ command }) => ({
  plugins: [react(), ...(command === 'build' ? [viteSingleFile()] : [])],
  resolve: {
    alias: {
      'juce-framework-frontend-mirror': resolvePath(
        __dirname,
        'src/bridge/iplug/iplug-shim.ts'
      ),
    },
  },
  define: {
    'import.meta.env.PACKAGE_VERSION': JSON.stringify(packageJson.version),
    'import.meta.env.VITE_APP_VERSION_FULL': JSON.stringify(fullVersion),
    'import.meta.env.VITE_BUILD_DATE': JSON.stringify(new Date().toISOString().split('T')[0]),
  },
  server: {
    port: 5173,
    host: '127.0.0.1',
    cors: true,
    headers: {
      'Access-Control-Allow-Origin': '*',
    },
  },
  build: {
    outDir: '../plugin/resources/web',
    emptyOutDir: true,
    rollupOptions: {
      onwarn(warning, warn) {
        // JUCEライブラリのeval警告を抑制（shim 経由でも保険として残す）
        if (warning.code === 'EVAL' && warning.id?.includes('juce-framework-frontend-mirror')) {
          return
        }
        warn(warning)
      },
    },
  },
}))
