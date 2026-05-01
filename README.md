# MixCompare

A professional audio plugin for DAW final stage mixing that provides instant switching between your work-in-progress mix and reference audio sources. Built with JUCE framework and modern web technologies.

## Features

- **Dual Source Switching**: Seamlessly switch between HOST (DAW) and WAV file sources
- **Playlist Management**: Add, remove, and reorder audio files with drag-and-drop support
- **Audio Transport**: Play, pause, seek, loop, and jump controls
- **Real-time Monitoring**: Volume meters and audio level monitoring
- **Low-pass Filter**: Built-in LPF for frequency analysis
- **Modern UI**: Web-based interface using React 19, Material-UI v7, and TypeScript

## Supported Formats

- **VST3**: Compatible with most modern DAWs
- **AU**: Native macOS Audio Unit support
- **AAX**: Protools
- **LV2**: Linux only (autoinstalls to `~/.lv2/`)
- **CLAP**: Linux only (autoinstalls to `~/.clap/`)
- **Standalone**: Independent application for testing and reference

## System Requirements

### Windows
- Windows 10 or later
- Visual Studio 2019 or later (for building)
- CMake 3.22 or later

### macOS
- macOS 10.15 or later
- Xcode 12 or later (for building)
- CMake 3.22 or later

### Linux (WSL2 Ubuntu 24.04 verified)
- gcc 13+ / clang
- CMake 3.22 or later, Ninja
- Required apt packages — see [Linux build instructions](#linux) below

## Building from Source

### Prerequisites

1. **Clone the repository**
   ```bash
   git clone https://github.com/yourusername/MixCompare.git
   cd MixCompare
   ```

2. **Install dependencies**
   - **Windows**: Install Visual Studio with C++ development tools
   - **macOS**: Install Xcode command line tools
   - **Both**: Install Node.js 18+ and npm

3. **AAX SDK Setup** (for AAX plugin builds)
   - Register at [Avid Developer Portal](https://www.avid.com/alliance-partner-program/become-an-audio-developer)
   - Download AAX SDK from the developer portal
   - Place the SDK in one of these directories:
     ```
     MixCompare/
     ├── AAX_SDK/        # Recommended
     ├── aax-sdk/        # Alternative 1
     └── AAX/            # Alternative 2
     ```
   - **Note**: AAX SDK is not included in the repository due to licensing restrictions

### Build Instructions

Both `build_windows.ps1` and `build_macos.zsh` auto-load a `.env` file in the project root (`KEY=VALUE` per line). Existing environment variables are not overwritten, so you can still export values in your shell if you prefer. `.env` is gitignored.

Recommended: drop a single `.env` with the PACE credentials and let both scripts pick them up.

```dotenv
# .env — project root, gitignored
PACE_USERNAME=your-ilok-account
PACE_PASSWORD=your-ilok-password
PACE_ORGANIZATION=XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX   # WCGUID (per-plugin, issued at https://pc2.paceap.com/)
PACE_KEYPASSWORD=your-pfx-password                        # Windows only (matches the PFX export password)
```

#### Windows

1. **Set PACE credentials** — either put the four keys above into `.env`, or export them via `$PROFILE`:
   ```powershell
   $env:PACE_USERNAME    = "your-username"
   $env:PACE_PASSWORD    = "your-password"
   $env:PACE_ORGANIZATION = "your-wcguid"
   $env:PACE_KEYPASSWORD = "your-pfx-password"
   $env:PACE_PFX_PATH    = "D:\path\to\certificate.pfx"   # optional; defaults to project root lookup
   ```

2. **Build the plugin**
   ```powershell
   .\build_windows.ps1
   ```

   If PACE credentials are missing, the build still succeeds — it just emits an unsigned plugin and records the reason in the summary (`certificate_missing` / `credentials_missing` / etc.).

   **AAX Build Options**:
   - If the AAX SDK is not available, AAX plugin builds are skipped automatically
   - VST3 and Standalone builds always work without the AAX SDK

   **Certificate lookup order** (first match wins):
   - `$env:PACE_PFX_PATH` (explicit path)
   - `<project root>\mixcompare-dev.pfx`
   - `%USERPROFILE%\.mixcompare\dev.pfx`
   - `.\certificates\mixcompare-dev.pfx`

#### macOS

1. **Set PACE credentials** — same `.env` keys work here, or export in your shell profile:
   ```bash
   export PACE_USERNAME="your-username"
   export PACE_PASSWORD="your-password"
   export PACE_ORGANIZATION="your-wcguid"
   # PACE_KEYPASSWORD is not used on macOS (wraptool signs via iLok USB, not a PFX)

   # Code signing (optional - will auto-detect from Keychain if not set)
   export CODESIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)"

   # Notarization (choose one method)
   # Method 1: App Store Connect API Key
   export APPLE_API_KEY_PATH="/path/to/AuthKey_XXXXXXXXXX.p8"
   export APPLE_API_KEY_ID="XXXXXXXXXX"
   export APPLE_API_ISSUER="xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"

   # Method 2: Keychain Profile (recommended)
   export NOTARYTOOL_PROFILE="your-profile-name"

   # Method 3: Apple ID (legacy)
   export APPLE_ID="your-apple-id@example.com"
   export APP_PASSWORD="your-app-specific-password"
   export TEAM_ID="XXXXXXXXXX"
   ```

2. **Build the plugin**
   ```bash
   ./build_macos.zsh
   ```

   `build_macos.zsh` reads the same `PACE_USERNAME` / `PACE_PASSWORD` / `PACE_ORGANIZATION` trio from the environment (no hardcoded credentials). If any of the three is missing, the wraptool step is skipped with a `Missing PACE credentials: ...` notice and the build continues as an unsigned developer build. `PACE_WCGUID` is accepted as a fallback name for `PACE_ORGANIZATION` for backwards compatibility.

   **Note**: macOS builds automatically detect code signing certificates from Keychain. If `CODESIGN_IDENTITY` is not set, the script will automatically find and use the first available "Developer ID Application" certificate.

   **AAX Build Options**:
   - If the AAX SDK is not available, AAX plugin builds are skipped automatically
   - VST3, AU, and Standalone builds always work without the AAX SDK

   **Keychain Integration**:
   - Code signing certificates are automatically detected from Keychain
   - Notarization credentials can be stored as Keychain profiles
   - No hardcoded paths or credentials required

#### Linux

Tested on **WSL2 Ubuntu 24.04**, but should work on any modern glibc-based distro with `webkit2gtk-4.1` available.

```bash
sudo apt update
sudo apt install -y \
  build-essential pkg-config cmake ninja-build git \
  libasound2-dev libjack-jackd2-dev libcurl4-openssl-dev \
  libfreetype-dev libfontconfig1-dev \
  libx11-dev libxcomposite-dev libxcursor-dev libxext-dev \
  libxinerama-dev libxrandr-dev libxrender-dev \
  libwebkit2gtk-4.1-dev libglu1-mesa-dev mesa-common-dev libgtk-3-dev

git submodule update --init --recursive   # JUCE + clap-juce-extensions
bash build_linux.sh                        # Release VST3 / LV2 / CLAP / Standalone + zip
```

Output:

- Build artefacts: `build-linux/plugin/MixCompare_artefacts/Release/{VST3,LV2,CLAP,Standalone}/`
- Auto-installed: `~/.vst3/MixCompare.vst3`, `~/.lv2/MixCompare.lv2`, `~/.clap/MixCompare.clap`
- Distribution zip: `releases/<VERSION>/MixCompare_<VERSION>_Linux_VST3_LV2_CLAP_Standalone.zip`

LV2 and CLAP are gated behind `if(UNIX AND NOT APPLE)` in CMake, so existing Windows / macOS release flows are unaffected. AU and AAX are skipped on Linux as expected.

> **Note**: The bundled MACLib (Monkey's Audio) is forced to `POSITION_INDEPENDENT_CODE ON` only when building for Linux, so it can be linked into the shared `.so` / `.clap` modules. Windows uses PE/COFF and macOS makes static libs PIC by default, so this only affects Linux builds.

### Development Build

For development with live web UI updates:

1. **Start the web development server**
   ```bash
   cd webui
   npm install
   npm run dev
   ```

2. **Build the plugin in development mode**
   - The plugin will load the web UI from `http://localhost:5173`
   - Changes to the web UI will be reflected immediately

## Architecture

### Native Layer (C++/JUCE)
- Audio processing and file I/O
- State management with ValueTree/APVTS
- Cross-platform plugin framework

### Web Layer (React/TypeScript)
- Modern user interface
- Drag-and-drop playlist management
- Real-time audio monitoring
- Responsive design with Material-UI

### Bridge Communication
- Bidirectional messaging between native and web layers
- Throttled updates for performance (20-60Hz)
- Command-based API for user interactions

## Usage

1. **Load the plugin** in your DAW's final stage
2. **Add reference files** to the playlist using drag-and-drop
3. **Switch sources** between HOST (your DAW mix) and WAV files
4. **Use transport controls** for playback, seeking, and looping
5. **Monitor levels** with built-in meters and volume controls

## Development

### Project Structure
```
MixCompare/
├── plugin/src/          # C++ plugin source code
├── webui/               # React/TypeScript web interface
└── cmake/               # CMake configuration
```

### Key Technologies
- **JUCE Framework**: Cross-platform audio plugin development
- **React 19**: Modern web UI framework
- **Material-UI v7**: Component library
- **TypeScript**: Type-safe JavaScript
- **Vite**: Fast build tool and dev server
- **DnD Kit**: Drag-and-drop functionality

## Web Demo (WASM Experiment)

This repository includes an experimental **Web SPA version** of MixCompare that runs entirely in the browser using WebAssembly. The plugin's C++ DSP code is compiled to WASM via Emscripten, and the existing React UI is reused with minimal changes through Vite alias swapping.

### Why?

- **Plugin demo site**: Let users try the plugin's core functionality directly in the browser without installing anything
- **Beyond Web Audio API**: The WASM DSP runs custom C++ algorithms (TPT state-variable filter, ITU-R BS.1770-4 LKFS metering) that aren't available as native Web Audio nodes
- **Code reuse**: The same React/MUI/TypeScript frontend serves both the JUCE WebView plugin and the standalone web app

### Architecture

```
┌─ Browser ──────────────────────────────────────────────────┐
│                                                            │
│  Main Thread                  Audio Thread (AudioWorklet)  │
│  ┌─────────────┐             ┌──────────────────────┐      │
│  │ React UI    │  postMsg    │ dsp-processor.js     │      │
│  │ (existing)  │◄──────────►│ (thin wrapper)       │      │
│  │             │             │   │                  │      │
│  │ WebRuntime  │             │   ▼                  │      │
│  │ juce-shim   │             │ ┌──────────────────┐ │      │
│  └─────────────┘             │ │ C++ WASM (34KB)  │ │      │
│        │                     │ │                  │ │      │
│        │ decodeAudioData     │ │ • PCM playback   │ │      │
│        ▼                     │ │ • Transport      │ │      │
│  ┌─────────────┐   PCM      │ │ • Gain (H/PL)   │ │      │
│  │ File picker │────────────►│ │ • Source blend   │ │      │
│  │ / D&D       │  transfer   │ │ • LPF (TPT SVF) │ │      │
│  └─────────────┘             │ │ • Metering (dB)  │ │      │
│                              │ │ • Loop / Seek    │ │      │
│                              │ └──────────────────┘ │      │
│                              └──────────────────────┘      │
└────────────────────────────────────────────────────────────┘
```

**Key design decision**: Audio processing logic (playback position, transport, gain, LPF, metering, source blending) lives entirely in C++ WASM. The JavaScript AudioWorklet is a thin wrapper that calls `dsp_process_block()` and relays state updates to the main thread at ~20Hz.

### How the UI is reused

The existing React components import from `juce-framework-frontend-mirror` and `bridge/juce`. For the web build, Vite aliases swap these to web-compatible shims:

| Plugin build | Web build | Purpose |
|---|---|---|
| `juce-framework-frontend-mirror` | `bridge/web/juce-shim.ts` | Parameter state objects (SliderState, ToggleState, ComboBoxState) |
| `bridge/juce.ts` | `bridge/web/web-juce.ts` | Bridge manager (`juceBridge.callNative`, `addEventListener`, etc.) |

This means **zero changes to component code** — `Playlist.tsx`, `Transport.tsx`, `VUMeter.tsx`, `Controls.tsx`, and `GainFader.tsx` work identically in both environments.

### Directory structure

```
MixCompare/
├── wasm/                          # C++ DSP for WASM (JUCE-free)
│   ├── src/
│   │   ├── dsp_engine.h           # Full audio engine (tracks, transport, DSP)
│   │   ├── svt_filter.h           # StateVariableTPT LPF (Zavalishin)
│   │   ├── smoothed_value.h       # LinearSmoothedValue equivalent
│   │   ├── metering.h             # RMS / Peak / TruePeak / LKFS
│   │   ├── momentary_processor.h  # ITU-R BS.1770-4 K-weighting
│   │   └── wasm_exports.cpp       # extern "C" FFI for WASM
│   └── CMakeLists.txt             # Emscripten build (STANDALONE_WASM)
├── webui/
│   ├── src/bridge/web/            # Web-specific bridge layer
│   │   ├── WebAudioEngine.ts      # AudioContext + Worklet manager
│   │   ├── WebBridgeManager.ts    # juceBridge drop-in replacement
│   │   ├── WebParamState.ts       # SliderState / ToggleState / ComboBoxState
│   │   ├── juce-shim.ts           # juce-framework-frontend-mirror shim
│   │   └── web-juce.ts            # bridge/juce.ts alias target
│   ├── public-web/                # Web-only static assets (not in plugin build)
│   │   ├── worklet/dsp-processor.js
│   │   ├── wasm/mixcompare_dsp.wasm
│   │   └── audio/sample.mp3       # Host demo audio
│   ├── vite.config.web.ts         # Web SPA build config (aliases + mergePublicWeb plugin)
│   └── vite.config.ts             # Plugin build config (unchanged)
```

### Building the Web version

#### Prerequisites

- Node.js 18+
- Emscripten SDK (emsdk)

#### 1. Install Emscripten

```bash
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest
./emsdk activate latest
source emsdk_env.sh  # or emsdk_env.bat on Windows
```

#### 2. Build WASM

```bash
cd wasm
mkdir build && cd build
emcmake cmake .. -DCMAKE_BUILD_TYPE=Release
emmake cmake --build .
cp dist/mixcompare_dsp.wasm ../../webui/public-web/wasm/
```

#### 3. Run the Web version

```bash
cd webui
npm install
npm run dev:web    # → http://127.0.0.1:5174
```

#### 4. Production build & deploy

```bash
npm run build:web  # → dist/
# Deploy dist/ to Firebase Hosting, Vercel, Netlify, etc.
```

### npm scripts

| Script | Description |
|---|---|
| `npm run dev` | Plugin WebView dev server (port 5173) |
| `npm run dev:web` | Web SPA dev server (port 5174) |
| `npm run build` | Plugin WebView production build |
| `npm run build:web` | Web SPA production build → `dist/` |

### Inspiration

This experiment was inspired by [kodama-vst](https://github.com/yuichkun/kodama-vst), which uses a shared Rust DSP core compiled to both native (for JUCE plugin) and WASM (for web). MixCompare takes a different approach — compiling existing C++ DSP directly to WASM via Emscripten, avoiding the need to rewrite in Rust.

## Contributing

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

## License

This project is licensed under the **GNU Affero General Public License v3.0 or later** (AGPL-3.0-or-later) — see the [LICENSE](LICENSE) file for the full text.

It uses [JUCE](https://juce.com/) under the AGPLv3 option of its dual-licensing scheme. Other third-party SDKs (VST3 / AAX / WebView2 / etc.) are governed by their own licenses; the runtime dependency list is shown in the in-app *Licenses* dialog.

## Acknowledgments

- Built with [JUCE Framework](https://juce.com/)
- Web UI powered by [React](https://reactjs.org/) and [Material-UI](https://mui.com/)
- Audio plugin development best practices from the JUCE community

## Support

For issues, feature requests, or questions:
- Open an issue on GitHub