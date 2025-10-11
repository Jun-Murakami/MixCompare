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

#### Windows

1. **Set up environment variables** (optional, for signed builds)
   Add to your PowerShell profile (`$PROFILE`):
   ```powershell
   # PACE Anti-Piracy signing credentials (for developers only)
   $env:PACE_USERNAME = "your-username"
   $env:PACE_PASSWORD = "your-password"
   $env:PACE_ORGANIZATION = "your-org-guid"
   $env:PACE_KEYPASSWORD = "your-key-password"
   
   # PFX certificate path (optional - will fallback to project root if not set)
   $env:PACE_PFX_PATH = "D:\path\to\your\certificate.pfx"
   ```

2. **Build the plugin**
   ```powershell
   .\build_windows_release.ps1
   ```

   **Note**: If PACE credentials are not set, the build will create unsigned plugins suitable for development and testing.
   
   **AAX Build Options**:
   - If AAX SDK is not available, AAX plugin builds will be skipped automatically
   - VST3 and Standalone builds will still work without AAX SDK
   
   **Certificate Options**:
   - Set `PACE_PFX_PATH` environment variable to specify custom certificate location
   - Place `mixcompare-dev.pfx` in project root directory
   - Place certificate in `%USERPROFILE%\.mixcompare\dev.pfx`
   - Place certificate in `.\certificates\mixcompare-dev.pfx`

#### macOS

1. **Set up environment variables** (optional, for signed builds)
   Add to your shell profile:
   ```bash
   # PACE Anti-Piracy signing credentials (for developers only)
   export PACE_USERNAME="your-username"
   export PACE_PASSWORD="your-password"
   export PACE_ORGANIZATION="your-org-guid"
   export PACE_KEYPASSWORD="your-key-password"
   
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
   ./build_local_macos.zsh
   ```

   **Note**: macOS builds automatically detect code signing certificates from Keychain. If `CODESIGN_IDENTITY` is not set, the script will automatically find and use the first available "Developer ID Application" certificate.
   
   **AAX Build Options**:
   - If AAX SDK is not available, AAX plugin builds will be skipped automatically
   - VST3, AU, and Standalone builds will still work without AAX SDK
   
   **Keychain Integration**:
   - Code signing certificates are automatically detected from Keychain
   - Notarization credentials can be stored as Keychain profiles
   - No hardcoded paths or credentials required

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

## Contributing

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- Built with [JUCE Framework](https://juce.com/)
- Web UI powered by [React](https://reactjs.org/) and [Material-UI](https://mui.com/)
- Audio plugin development best practices from the JUCE community

## Support

For issues, feature requests, or questions:
- Open an issue on GitHub