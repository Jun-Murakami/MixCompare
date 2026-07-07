// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Jun Murakami
#include "DiagnosticLog.h"

// CMake 未構成時（IntelliSense 等）でも解析できるようフォールバックを持つ
#if __has_include("Version.h")
 #include "Version.h"
#else
 #define MIXCOMPARE_VERSION_STRING "unknown"
#endif

#include <cstdlib>
#include <memory>
#include <mutex>

#if JUCE_LINUX
 #include <dlfcn.h>
 #include <csignal>
#endif

namespace mc3
{

namespace
{

std::unique_ptr<juce::FileLogger>& getLoggerHolder()
{
    static std::unique_ptr<juce::FileLogger> logger;
    return logger;
}

juce::CriticalSection& getLogLock()
{
    static juce::CriticalSection lock;
    return lock;
}

juce::String envToString(const char* name)
{
    if (const char* value = std::getenv(name))
        return juce::String(name) + "=" + juce::String::fromUTF8(value);
    return juce::String(name) + "=(unset)";
}

#if JUCE_LINUX
// 1ライブラリを dlopen して結果を記録する。
// 重要: 成功してもハンドルは dlclose しない（意図的リーク）。GTK/WebKit 系は
// ロード時に glib メインコンテキストへコールバックを登録するため、dlclose で
// アンマップすると後のイベントループで解放済みコードを呼んで SEGV する
// （実際に libwebkitgtk-6.0 の probe→dlclose でクラッシュを確認済み）。
// upstream JUCE の WebKitSymbols も同様にハンドルを保持し続ける設計。
bool probeLibrary(const char* name)
{
    ::dlerror();

    if (::dlopen(name, RTLD_NOW | RTLD_LOCAL) != nullptr)
    {
        DiagnosticLog::log(juce::String("dlopen OK    ") + name);
        return true;
    }

    const char* error = ::dlerror();
    DiagnosticLog::log(juce::String("dlopen FAIL  ") + name
                       + "  (" + (error != nullptr ? error : "no dlerror") + ")");
    return false;
}

// JUCE の WebKitSymbols が試すライブラリ名（バージョン無し＋SONAME フォールバック、
// juce-webview-linux-soname.patch と同一リスト）をプローブし、どの層
// （webkit 本体 / gtk / glib / soup）で欠けているかを記録する。
void logDlopenProbes()
{
    // 4.1 系（libsoup3 世代）
    const bool webkit41 = probeLibrary("libwebkit2gtk-4.1.so")        | probeLibrary("libwebkit2gtk-4.1.so.0");
    const bool jsc41    = probeLibrary("libjavascriptcoregtk-4.1.so") | probeLibrary("libjavascriptcoregtk-4.1.so.0");
    const bool soup3    = probeLibrary("libsoup-3.0.so")              | probeLibrary("libsoup-3.0.so.0");

    // 4.0 系（libsoup2 世代）は 4.1 系が欠けている場合のみプローブする。
    // libsoup2 と libsoup3 を同一プロセスへロードすると libsoup 側の検査で
    // プロセスごと abort されるため（既知の非互換）、soup3 ロード後は soup2 に触れない。
    if (webkit41 && jsc41 && soup3)
    {
        DiagnosticLog::log("dlopen SKIP  libwebkit2gtk-4.0/libsoup-2.4 (4.1 stack present; "
                           "probing both soup generations in one process would abort)");
    }
    else
    {
        probeLibrary("libwebkit2gtk-4.0.so");
        probeLibrary("libwebkit2gtk-4.0.so.37");
        probeLibrary("libjavascriptcoregtk-4.0.so");
        probeLibrary("libjavascriptcoregtk-4.0.so.18");

        if (!soup3)
        {
            probeLibrary("libsoup-2.4.so");
            probeLibrary("libsoup-2.4.so.1");
        }
    }

    probeLibrary("libgtk-3.so");
    probeLibrary("libgtk-3.so.0");
    probeLibrary("libglib-2.0.so");
    probeLibrary("libglib-2.0.so.0");

    // 参考情報: GTK4 系 WebKit（webkitgtk6.0）の有無。GTK4 をこのプロセスへ
    // ロードするのは危険なので dlopen はせず、ファイル存在の確認だけにする。
    for (const char* dir : { "/usr/lib64", "/usr/lib/x86_64-linux-gnu", "/usr/lib", "/usr/local/lib" })
    {
        const auto candidate = juce::File(dir).getChildFile("libwebkitgtk-6.0.so.4");

        if (candidate.exists())
        {
            DiagnosticLog::log("info: " + candidate.getFullPathName()
                               + " exists (GTK4 WebKit — not usable by this plugin)");
            break;
        }
    }
}

void logLinuxEnvironment()
{
    // ディストリ情報（PRETTY_NAME 行のみ）
    const auto osRelease = juce::File("/etc/os-release");
    if (osRelease.existsAsFile())
    {
        juce::StringArray lines;
        lines.addLines(osRelease.loadFileAsString());
        for (const auto& line : lines)
            if (line.startsWith("PRETTY_NAME="))
                DiagnosticLog::log("distro: " + line.fromFirstOccurrenceOf("=", false, false).unquoted());
    }

    DiagnosticLog::log(juce::String("sandbox: flatpak=")
                       + (juce::File("/.flatpak-info").existsAsFile() ? "yes" : "no")
                       + " snap=" + (std::getenv("SNAP") != nullptr ? "yes" : "no"));

    for (const char* name : { "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP", "GDK_BACKEND",
                              "DISPLAY", "WAYLAND_DISPLAY", "LD_LIBRARY_PATH", "LD_PRELOAD",
                              "WEBKIT_DISABLE_DMABUF_RENDERER", "WEBKIT_DISABLE_COMPOSITING_MODE" })
        DiagnosticLog::log("env: " + envToString(name));

    logDlopenProbes();
}
#endif // JUCE_LINUX

} // namespace

void DiagnosticLog::install()
{
    static std::once_flag onceFlag;

    std::call_once(onceFlag, []
    {
        getLogDirectory().createDirectory();

        // 128KB を超えていたら FileLogger が古い前半を刈り込む
        getLoggerHolder() = std::make_unique<juce::FileLogger>(getLogFile(), juce::String(), 128 * 1024);

        log("==== session start ====");
        log(juce::String("plugin: MixCompare ") + MIXCOMPARE_VERSION_STRING
            + " (JUCE " + juce::SystemStats::getJUCEVersion() + ")");
        log("host: " + juce::String(juce::PluginHostType().getHostDescription())
            + " | process: "
            + juce::File::getSpecialLocation(juce::File::hostApplicationPath).getFullPathName());
        log("os: " + juce::SystemStats::getOperatingSystemName());

#if JUCE_LINUX
        // WebView 子プロセスが起動直後に死んだ場合（WebKitGTK 不在等）、親側が死んだ子の
        // パイプへ書き込んで SIGPIPE を受け、既定動作でプロセスごと落ちる（スタンドアロンは
        // 即終了、SIGPIPE を無視しないホストでは DAW ごと落ちる）ことを確認済み。
        // SIG_IGN にして write を EPIPE エラー返しに変える（サーバ/GUI アプリの標準対処）。
        ::signal(SIGPIPE, SIG_IGN);
        log("mitigation: SIGPIPE ignored (dead webview child pipe must not kill the process)");

        logLinuxEnvironment();

        // WebView 子プロセスの stdout/stderr をファイルへ捕捉する
        // （JUCE 側は juce-webview-linux-childlog.patch がこの環境変数を参照する）。
        // ファイルはここで必ず「事前作成」する: ユーザー報告時に
        //   - ファイル自体が無い     → このコード（プラグイン側）が動いていない
        //   - ヘッダ行しか無い       → JUCE 側が一度も書いていない
        //     （= childlog パッチ欠落ビルド、または fork 前に失敗）
        //   - banner 以降の行がある  → 子プロセスの死因がそこに書かれている
        // を一目で区別できるようにするため。
        auto childLog = getWebViewChildLogFile();
        if (childLog.getSize() > 128 * 1024)
            childLog.deleteFile();
        childLog.appendText("[plugin] child log opened by DiagnosticLog ("
                            + juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M:%S")
                            + ", MixCompare " MIXCOMPARE_VERSION_STRING
                            + ") — lines below this one are written by the webview child process\n");
        ::setenv("JUCE_WEBVIEW_CHILD_LOG", childLog.getFullPathName().toRawUTF8(), 1);
        log("webview child log: " + childLog.getFullPathName());

        // 緩和策: NVIDIA / Wayland 環境で WebKit の DMABUF レンダラーが真っ白な描画を
        // 出す既知問題があるため、ユーザーが明示指定していない場合のみ既定で無効化する。
        // 元に戻すには環境変数 WEBKIT_DISABLE_DMABUF_RENDERER=0 を設定して DAW を起動する。
        if (std::getenv("WEBKIT_DISABLE_DMABUF_RENDERER") == nullptr)
        {
            ::setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 0);
            log("mitigation: WEBKIT_DISABLE_DMABUF_RENDERER=1 (beta default)");
        }
#endif
    });
}

void DiagnosticLog::log(const juce::String& message)
{
    auto& logger = getLoggerHolder();

    if (logger == nullptr)
        return;

    const juce::ScopedLock scopedLock(getLogLock());
    logger->logMessage(juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M:%S ") + message);
}

juce::File DiagnosticLog::getLogDirectory()
{
   #if JUCE_MAC
    // CrashHandler::getDumpDirectory と同じ流儀で ~/Library/Logs/MixCompare を構築
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
               .getParentDirectory()
               .getChildFile("Logs")
               .getChildFile("MixCompare");
   #else
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
               .getChildFile("MixCompare")
               .getChildFile("Logs");
   #endif
}

juce::File DiagnosticLog::getLogFile()
{
    return getLogDirectory().getChildFile("MixCompare-diagnostic.log");
}

juce::File DiagnosticLog::getWebViewChildLogFile()
{
    return getLogDirectory().getChildFile("MixCompare-webview-child.log");
}

} // namespace mc3
