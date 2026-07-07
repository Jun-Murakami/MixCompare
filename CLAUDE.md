必ず日本語で回答すること。

## MixCompare 開発用 ルール（AGENTS）

この文書は JUCE + WebView（Vite/React/MUI）構成で「WAV ファイルプレイヤー内蔵の仮想ソースセレクター」を実装するための合意ルールです。開発時の意思決定や PR レビューの基準として用います。

### 目的とスコープ

- **目的**: DAW 最終段に挿して、作業中の Mix とリファレンス音源を素早く切替・比較するプラグインを提供する。
- **対象フォーマット**: VST3 / AU / AAX / Standalone（Windows / macOS、開発は Standalone を主軸に進める）+ VST3 / LV2 / CLAP / Standalone（Linux）
- **最小要件**:
  - プレイリスト: 追加・削除・並べ替え（DnD Kit）、ループ、シーク、Jump、Play/Pause
  - ソース切替: HOST(DAW) / WAV
  - 音量・メータ、LPF（初期バージョンに含む）
- **非目標（初期）**: ストリーミング配信、オフライン波形解析、ラウドネス正規化、自動ゲインマッチ

### 全体アーキテクチャ

- **C++/JUCE（ネイティブ）**: オーディオ処理、ファイル I/O、再生、状態保持（ValueTree/APVTS）
- **WebUI（Vite + React 19 + MUI v7 + DnD Kit）**: プレイリスト UI、トランスポート、メータ、設定
- **ブリッジ**: `WebBrowserComponent` 経由の双方向メッセージ（JS <-> C++）。
  - 高頻度通信（オーディオ周期）は禁止。GUI 更新は 30–60Hz にスロットル。
  - 大容量データを送らず、ID/ハンドルで参照を受け渡し。

### 主要な方針変更（更新）

- `StateManager` への依存を縮小し、整合と通知に責務を限定
- APVTS（`AudioProcessorValueTreeState`）に主要パラメータを集約し、Web*Relay/Attachment で DOM と双方向バインディング
- トランスポート/ソース選択/ゲイン/LPF 等は「パラメータ直結」へ統一し、オートメーション・永続化・同期をフレームワークに委譲
- ネイティブ関数はプレイリスト操作やメータリセット等のコマンド系に限定（例: `playlist_action`, `metering_reset`）
- 高頻度データ（メータ、位置）はイベント駆動で 20–60Hz にスロットル。UI 非表示時は送出停止

### ディレクトリ指針（更新）

- `webui/` Vite アプリ（React/MUI/DnD Kit）
- `webui/public/` WebView 静的資産（最終的に zip 化して埋め込み）
- `webui/src/bridge/juce.ts` JS ブリッジ（ネイティブ関数呼び出し・イベント購読）
- `plugin/src/` C++（`PluginProcessor.*`, `PluginEditor.*`, `audio/*`, `core/*`）
  - C++ 側の `bridge/*` ディレクトリは廃止。ブリッジは `PluginEditor` 内の WebView 構築と Web*Relay/Attachment に一元化
- `cmake/` 補助スクリプト、Web 資産打包
- `docs/` 設計・仕様

### CMake/ビルド方針

- Dev: WebView は `http://localhost:5173`（Vite dev server）を読み込む。
- Prod: `webui build` を zip 化 → `juce_add_binary_data` で埋め込み、ローカル資産を読み込む。
- JUCE の WebView を明示的に有効化（Windows は WebView2、macOS は WKWebView）。

```cmake
# WebView利用のコンパイル定義（抜粋）
target_compile_definitions(MixCompare
    PUBLIC
        JUCE_WEB_BROWSER=1
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        $<$<PLATFORM_ID:Windows>:JUCE_USE_WIN_WEB_VIEW2_WITH_STATIC_LINKING=1>
)
```

### Linux 配布ビルド（WSL2 Ubuntu 24.04 で動作確認）

- ビルドコマンド: `bash build_linux.sh`
  - 成果物: `releases/<VERSION>/MixCompare_<VERSION>_Linux_VST3_LV2_CLAP_Standalone.zip`。VST3 / LV2 / CLAP / Standalone を同梱
  - 自動インストール先: `~/.vst3/MixCompare.vst3`, `~/.lv2/MixCompare.lv2`, `~/.clap/MixCompare.clap`（VST3 / LV2 は JUCE の `COPY_PLUGIN_AFTER_BUILD`、CLAP は `build_linux.sh` 側で明示コピー）
- LV2 / CLAP は **Linux ビルドでのみ** 有効化（`if(UNIX AND NOT APPLE)` で条件分岐）。Windows / macOS の既存リリース経路には影響させない
- LV2URI: `https://junmurakami.com/plugins/mixcompare`（`plugin/CMakeLists.txt` の `juce_add_plugin` 内）。LV2 規約上 stable な URI 必須なのでバージョンを跨いで変更しない
- CLAP: `clap-juce-extensions` を submodule として取り込み、`clap_juce_extensions_plugin(... CLAP_ID "com.junmurakami.mixcompare" CLAP_FEATURES audio-effect utility analyzer)` を呼ぶ
- **MACLib（Monkey's Audio）の PIC 化**: vendored の `plugin/lib/mac_sdk` は標準では `-fPIC` 無しの static lib としてビルドされるため、Linux 共有ライブラリ（VST3 / LV2 / CLAP）にリンクすると `relocation R_X86_64_PC32 ... can not be used when making a shared object; recompile with -fPIC` で落ちる。ルート `CMakeLists.txt` で `if(UNIX AND NOT APPLE)` 限定で `set_target_properties(MACLib PROPERTIES POSITION_INDEPENDENT_CODE ON)` を付与して解決済み（vendored ソースには手を入れない方針）
- **m4a/AAC 再生（Linux のみ）**: `plugin/src/audio/FFmpegAACFormat.{h,cpp}` で FFmpeg(libav*) 経由の AAC/M4A デコードを実装。JUCE の `registerBasicFormats()` には AAC デコーダが無く、Windows は Media Foundation・Linux は FFmpeg で補う構成。
  - **ビルド時**: 必要なのは dev ヘッダのみ（`pkg_check_modules(FFMPEG ...)` で検出 → `MIXCOMPARE_HAVE_FFMPEG=1` を定義し include パスのみ追加）。共有ライブラリへのリンクはしない。ヘッダ未導入でもビルドは通る（AAC が無効になるだけ）
  - **実行時**: `dlopen` でディストリ提供の `libav*.so.<メジャー>`（ヘッダのメジャー版に一致する versioned soname）を読み込む。見つからなければ `FFmpegAACFormat::isFFmpegAvailable()==false` となり登録をスキップ（プラグイン自体は正常ロード、AAC のみ無効）。`if(UNIX AND NOT APPLE)` 限定で `clap-juce-extensions`/`MediaFoundationAACFormat` と同じ「利用可能なら登録」運用
  - **ライセンス**: 本プロジェクトは AGPL-3.0-or-later。FFmpeg(LGPL/GPL) は AGPL と互換。**同梱せずシステム提供を参照**するため配布 zip にコーデック本体・特許対象を含めない。fdk-aac(non-free) / faad2(GPLv2-only で AGPLv3 と非互換) は使わず FFmpeg ネイティブ AAC デコーダで完結
- 必要 apt パッケージ: `build-essential pkg-config cmake ninja-build git libasound2-dev libjack-jackd2-dev libcurl4-openssl-dev libfreetype-dev libfontconfig1-dev libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev libxrandr-dev libxrender-dev libwebkit2gtk-4.1-dev libglu1-mesa-dev mesa-common-dev libgtk-3-dev`（m4a/AAC 再生を有効化するなら追加で `libavcodec-dev libavformat-dev libavutil-dev libswresample-dev`）

- **Linux の DPI/ウィンドウスケール（埋め込みプラグイン）**: `PluginEditor::applyDisplayScale` が担う。ホストの宣言スケール（VST3 `setContentScaleFactor` / CLAP `guiSetScale`。Bitwig は分数スケール 150% を 200% と誤判定する）には依存せず、**WebView が OS から拾う真のディスプレイ倍率 `devicePixelRatio`（`apply_layout` 経由で受信）**を基準に `transform = devicePixelRatio / peerScale` を適用する。`peerScale` は「プラットフォームが既に窓を拡大しているか」を表し、これで「not-dpi-aware ホスト(KDE Bitwig, peerScale=1.0)では自前拡大／DPI-aware なホスト・コンポジタ(GNOME Mutter, peerScale=2.0)では二重適用回避」を1式で吸収する。**Standalone は対象外**（DPI-aware top-level で OS/コンポジタが拡大するため `wrapperType` で除外。transform を掛けると巨大化する）。transform を WebView ロード後に適用するため、`settleReconcileDone` 再武装の 1px ジグルで WebView ネイティブ子窓を新 transform に追従させる（怠ると灰色余白）。Ubuntu Studio26/KDE・Debian13/GNOME × Standalone/埋め込みの全4ケースで確認済み。**かつての globalScale 実測補正＋ディスクキャッシュ方式はプロセスグローバル汚染で破綻したため完全撤去**（`setGlobalScaleFactor` は使わない）。

- **build-in-docker.sh（リポジトリ親 `JUCE/`）**: `./build-in-docker.sh [Config] [Repo ...]` で単体/複数リポをビルド可（例 `./build-in-docker.sh MixCompare`）。Ubuntu 22.04 ベース(glibc 2.35)で Debian 12/13 互換バイナリを生成。Docker は `HOME=/tmp/home` で走るためホストの `~/.clap` 等は更新されない → ローカル実機テストは成果物を明示コピーする。

### JUCE パッチ機構（Linux WebView 修正）

- `patches/juce-webview-linux-{utf8,ldpath,soname,childlog}.patch` を `cmake/ApplyJucePatch.cmake` が configure 時に冪等適用する（`git apply --reverse --check` で適用済みを判定し、未適用のみ apply）。**適用失敗・パッチ欠落は FATAL**（かつて WARNING で握りつぶし、パッチ無しバイナリを黙って出荷する事故があった）。失敗時は **touched files を `git checkout HEAD --` で pristine 化して全パッチを再適用する自動リカバリ**を持つ（旧版が焼き込まれた stale ツリーを回復）。
  - utf8: 非ASCII文字化け修正（＋LV2 HiDPI） / ldpath: WebView 子プロセスの `LD_LIBRARY_PATH` サニタイズ＋`GDK_BACKEND=x11` 固定 / soname: `dlopen` をバージョン付き SONAME（`.so.0` 等）へフォールバック / childlog: 子プロセスの stdout/stderr をログへ dup2。**この順序でのみ適用可**（childlog は soname が追加した行を文脈に含む）。
- **JUCE の C/C++ ソースは CRLF**（`git show HEAD:*.cpp` の生バイトが `\r\n`。autocrlf 副作用ではなくリポジトリの blob 自体）。よってパッチも **CRLF 必須**。`.gitattributes` の `patches/*.patch text eol=crlf` で、コミットしたマシンの改行設定に依存せず**チェックアウト時に必ず CRLF を実体化**する（`-text` はバイト保全なので、どこかのマシンで LF 保存されると劣化し configure が FATAL する）。
- **マスター同期**: 親 `../patches`（= `JUCE/patches`、**マシンローカルで git 非管理**の共有プール）があれば、configure 時にローカル `patches/` へ**生ファイルコピーで上書き同期**される（`configure_file COPYONLY`＝gitattributes を貫通）。**パッチを編集したら必ず `../patches` にもコピー**し、マスターは CRLF・最新に保つこと。**同期を忘れてマスターだけ旧版に取り残されると、ホストビルドで新パッチが黙って旧版へ退行する**（Docker は container に `../patches` が無いので無関係）。
- 姉妹6リポ（MixCompare / TinyVU / ZeroComp / ZeroEQ / ZeroLimit / TestTone）で同一機構。JUCE submodule commit が揃っていればパッチは共有可。

### ブリッジ・メッセージ設計（更新）

- パラメータは APVTS に直結（Web*Relay/Attachment と `getSliderState/getToggleState/getComboBoxState`）。JS→C++ の明示メッセージは不要
- JS → C++（コマンド系のみ）:
  - `playlist_action`（追加/削除/並べ替え/選択 などを引数で指定）
  - `metering_reset`（Momentary/TruePeak のリセット）
- C++ → JS（イベント系、スロットル 20–60Hz）:
  - 初期化・状態: 初期パラメータ、プレイリスト、トランスポート状態
  - メータ更新: 表示モードに応じたレベルデータ
  - エラー通知: 非ブロッキングな通知用
- 形式は `{ type: string, payload: any, nonce?: string }` を必要箇所のみ。大量データは送らない（ID 参照）

### オーディオスレッド原則（重要）

- `processBlock` 内ではメモリ確保・ファイル I/O・ロック禁止。
- 原子変数・ロックフリーキューを使用。
- シーク/ソース切替はオーディオ境界でスナップ、状態はダブルバッファでスワップ。
- デコード/プリフェッチはワーカースレッド。短尺はメモリ展開、長尺はストリーミング。

### プレイリスト/ファイル I/O

- パスは OS ネイティブで保持。Web へは短い ID のみを渡す。
- 永続化は `ValueTree` + `PropertiesFile`（または APVTS の非自動化値）で行い、DAW プロジェクト保存に追従。
- 並べ替えは ID 配列の更新のみを JS→C++へ送信。

### UI/UX 原則

- A/B 切替はワンクリック、極力ゼロレイテンシ（クロスフェード/ゼロクロスは初期無効）。
- 一般的なシークバー、キーボードショートカット（Space, ↑↓, ←→, 1/2/3…）。
- ログ/エラーは非ブロッキングなスナックバーで通知。

### コーディング規約（C++）

- 明示的な型、早期 return、2 段以上の深いネスト回避。
- 例外は原則不使用。戻り値でエラー伝搬。
- 関数は動詞、変数は意味的名詞。長くても可読性優先。
- コメントは「なぜ」を中心に要点のみ。

### コーディング規約（Web）

- TypeScript 必須。any 型は禁止。受信 Payload は `zod` 等でバリデーション。
- ESLint + Prettier。コンポーネントは疎結合・小さく。
- MUI テーマはダーク優先。リサイズ依存の過描画は避ける。

### デバッグ運用（VSCode）

- Standalone を基本に F5 起動、`processBlock` 冒頭とブリッジ境界にブレークポイント。
- VST3/AU は必要時にホストを起動してアタッチ。
- `DBG()` とリングバッファを使い、GUI 更新は 60Hz 上限。

### ブランチ/コミット

- `main` 安定、`feat/*`・`fix/*`・`chore/*` 運用。
- Conventional Commits 準拠。小さめ PR を心掛ける。

### セキュリティ/配布

- 開発中は自動署名/公証を無効化。外部 URL の読み込みは禁止（Dev 時 `localhost:5173` のみ）。

### マイルストーン（更新）

- M1: WebView 最小連携（Vite 直読み、Play/Stop/Seek）
- M2: プレイリスト（追加/削除/並べ替え）+ 永続化
- M3: メータ・ボリューム・LPF + A/B 切替最適化（APVTS 直結）
- M4: Prod パッケージ（web 資産 zip 埋め込み）+ VST3/AU 動作確認
