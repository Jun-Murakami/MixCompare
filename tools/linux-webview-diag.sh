#!/bin/bash
# ------------------------------------------------------------------------------
# MixCompare / TestTone / TinyVU / ZeroEQ / ZeroComp / ZeroLimit
# Linux WebView 診断スクリプト
#
# 使い方（ユーザーに依頼する手順）:
#   bash linux-webview-diag.sh > webview-diag.txt 2>&1
#   → 生成された webview-diag.txt を開発者に送付してください。
#
# スタンドアロン版の起動ログも採る場合はバイナリのパスを引数に:
#   bash linux-webview-diag.sh /path/to/MixCompare > webview-diag.txt 2>&1
#
# 収集するのは環境情報のみ（個人情報・プロジェクトデータは含まれません）。
# ------------------------------------------------------------------------------
echo "=== diag version: 2026-07-05 ==="
date

echo; echo "=== distro ==="
cat /etc/os-release 2>/dev/null | head -6

echo; echo "=== session ==="
echo "XDG_SESSION_TYPE=$XDG_SESSION_TYPE XDG_CURRENT_DESKTOP=$XDG_CURRENT_DESKTOP"
echo "flatpak-sandbox: $([ -f /.flatpak-info ] && echo YES || echo no)"

echo; echo "=== glibc ==="
ldd --version 2>/dev/null | head -1

echo; echo "=== webkit/gtk libraries in linker cache ==="
ldconfig -p 2>/dev/null | grep -E "webkit2gtk|javascriptcoregtk|libsoup|libgtk-3\.so|libglib-2\.0" || echo "(none found)"

echo; echo "=== dlopen tests (JUCE が試すライブラリ名) ==="
for lib in \
    libwebkit2gtk-4.1.so libwebkit2gtk-4.1.so.0 \
    libjavascriptcoregtk-4.1.so libjavascriptcoregtk-4.1.so.0 \
    libsoup-3.0.so libsoup-3.0.so.0 \
    libgtk-3.so libgtk-3.so.0 \
    libglib-2.0.so libglib-2.0.so.0
do
    python3 - "$lib" <<'EOF' 2>&1 || true
import ctypes, sys
name = sys.argv[1]
try:
    ctypes.CDLL(name)
    print(f"OK    {name}")
except OSError as e:
    print(f"FAIL  {name}")
EOF
done

echo; echo "=== related packages ==="
if command -v rpm >/dev/null 2>&1; then
    rpm -qa 2>/dev/null | grep -iE "webkit|libsoup|^gtk3|^glib2" | sort
fi
if command -v dpkg >/dev/null 2>&1; then
    dpkg -l 2>/dev/null | awk '/webkit|libsoup|libgtk-3|libglib2/ {print $2, $3}'
fi

if [ -n "$1" ] && [ -x "$1" ]; then
    echo; echo "=== standalone run (15s): $1 ==="
    timeout 15 "$1" 2>&1 | head -60 &
    APP_PID=$!
    sleep 8
    echo "--- webview child process after 8s (居れば WebView は起動できている):"
    ps -eo pid,args | grep "gtkwebkitfork[-]child" || echo "(no webview child process — WebView が起動できていない)"
    wait $APP_PID 2>/dev/null
fi

echo; echo "=== end of diagnostics ==="
