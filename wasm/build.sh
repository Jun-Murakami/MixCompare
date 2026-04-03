#!/bin/bash
# Emscripten で WASM DSP モジュールをビルド
# 前提: emsdk がインストール・アクティベート済み
#   source /path/to/emsdk/emsdk_env.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
DIST_DIR="${SCRIPT_DIR}/dist"

# ビルドディレクトリ
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# Emscripten ツールチェインで CMake 構成
emcmake cmake "${SCRIPT_DIR}" \
    -DCMAKE_BUILD_TYPE=Release

# ビルド
emmake make -j$(nproc 2>/dev/null || echo 4)

# dist に成果物をコピー
mkdir -p "${DIST_DIR}"
cp -f "${BUILD_DIR}/dist/mixcompare_dsp.js" "${DIST_DIR}/"
cp -f "${BUILD_DIR}/dist/mixcompare_dsp.wasm" "${DIST_DIR}/"

echo ""
echo "Build complete!"
echo "  JS:   ${DIST_DIR}/mixcompare_dsp.js"
echo "  WASM: ${DIST_DIR}/mixcompare_dsp.wasm"
echo ""
echo "Copy these to webui/public/wasm/ for development."
