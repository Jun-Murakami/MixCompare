# MixCompare Windows Release Build Script (iPlug2)
# WebUI 本番ビルド → CMake configure → VST3 / CLAP / Standalone / AAX コンパイル
# → Authenticode 署名 (Azure Key Vault) / AAX 署名 (PACE Eden) → ZIP 梱包
# → Inno Setup インストーラ生成。
#
# JUCE 版からの移植: ターゲット名は iPlug2 形式 (MixCompare-vst3 等)、成果物は
# build/out/MixCompare.* に出る。AAX SDK は iPlug2/Dependencies/IPlug/AAX_SDK。
# 署名資格情報は環境変数 (.env または User env) から読む (シークレットは非ハードコード)。

param(
    [string]$Configuration = "Release",
    [switch]$SkipCodeSign,
    [switch]$SkipWebUI
)

$ScriptDir = $PSScriptRoot
if (-not $ScriptDir) { $ScriptDir = (Get-Location).Path }
$RootDir = $ScriptDir
$VersionFile = "$RootDir\VERSION"

if (Test-Path $VersionFile) {
    $Version = (Get-Content $VersionFile -Raw).Trim()
} else {
    Write-Error "VERSION file not found at: $VersionFile"
    exit 1
}

$ErrorActionPreference = "Stop"

function Write-Header { param([string]$Text)
    Write-Host ""
    Write-Host "============================================" -ForegroundColor Cyan
    Write-Host "   $Text" -ForegroundColor Cyan
    Write-Host "============================================" -ForegroundColor Cyan
    Write-Host ""
}
function Write-Step    { param([string]$Text) Write-Host ">> $Text" -ForegroundColor Yellow }
function Write-Success { param([string]$Text) Write-Host "[OK] $Text" -ForegroundColor Green }
function Write-Fail    { param([string]$Text) Write-Host "[FAIL] $Text" -ForegroundColor Red }

Write-Header "MixCompare $Version Build Script (iPlug2)"

# Load .env (KEY=VALUE)
$EnvFilePath = "$RootDir\.env"
if (Test-Path $EnvFilePath) {
    Write-Host "Loading environment variables from .env ..." -ForegroundColor Gray
    Get-Content $EnvFilePath | ForEach-Object {
        $line = $_.Trim()
        if ($line -and -not $line.StartsWith("#")) {
            $eqIdx = $line.IndexOf("=")
            if ($eqIdx -gt 0) {
                $key   = $line.Substring(0, $eqIdx).Trim()
                $value = $line.Substring($eqIdx + 1).Trim().Trim('"').Trim("'")
                if (-not (Get-Item "env:$key" -ErrorAction SilentlyContinue)) {
                    [Environment]::SetEnvironmentVariable($key, $value, "Process")
                }
            }
        }
    }
}

# ----------------------------------------------------------------------------
# Windows Authenticode 署名 (Azure Key Vault 経由の azuresigntool)
# ----------------------------------------------------------------------------
# 配布する PE (VST3 本体 DLL / CLAP / Standalone exe / インストーラ) を Azure Key
# Vault の証明書で署名する。認証は -kvm (DefaultAzureCredential): 環境変数
# AZURE_CLIENT_ID / AZURE_TENANT_ID / AZURE_CLIENT_SECRET、または az login セッション。
# AAX は PACE Eden wraptool が wrap と一体で署名する (後追い署名は HashMismatch)。
$AzureKeyVaultUrl = if ($env:AZURE_KEYVAULT_URL) { $env:AZURE_KEYVAULT_URL } else { 'https://jun-codesign-kv.vault.azure.net/' }
$AzureCertName    = if ($env:AZURE_CERT_NAME)    { $env:AZURE_CERT_NAME }    else { 'jun-codesigning-2026' }
$TimestampUrl     = if ($env:CODESIGN_TIMESTAMP_URL) { $env:CODESIGN_TIMESTAMP_URL } else { 'http://timestamp.digicert.com' }
$CodeSigningStatus = "unsigned"

function Invoke-AuthenticodeSign {
    param([Parameter(Mandatory = $true)][string[]]$Paths)

    if ($SkipCodeSign) {
        Write-Host "Code signing skipped (-SkipCodeSign)" -ForegroundColor Yellow
        $script:CodeSigningStatus = "skipped"
        return $false
    }

    $existing = @($Paths | Where-Object { $_ -and (Test-Path $_) })
    if ($existing.Count -eq 0) {
        Write-Host "Code signing: no target files found" -ForegroundColor Yellow
        return $false
    }

    if (-not (Get-Command azuresigntool -ErrorAction SilentlyContinue)) {
        Write-Host "Warning: azuresigntool not found on PATH - skipping code signing" -ForegroundColor Yellow
        Write-Host "  Install with: dotnet tool install --global AzureSignTool" -ForegroundColor Gray
        $script:CodeSigningStatus = "tool_missing"
        return $false
    }

    foreach ($f in $existing) { Write-Step "Signing: $f" }

    $signArgs = @(
        "sign",
        "-kvu", $AzureKeyVaultUrl,
        "-kvc", $AzureCertName,
        "-kvm",
        "-tr", $TimestampUrl,
        "-td", "sha256",
        "-fd", "sha256"
    ) + $existing

    & azuresigntool @signArgs
    if ($LASTEXITCODE -eq 0) {
        Write-Success "Code signing succeeded ($($existing.Count) file(s))"
        $script:CodeSigningStatus = "signed"
        return $true
    } else {
        Write-Host "Warning: code signing failed (exit $LASTEXITCODE)" -ForegroundColor Yellow
        $script:CodeSigningStatus = "signing_failed"
        return $false
    }
}

$BuildDate = Get-Date -Format "yyyy-MM-dd"
$WebUIDir   = "$RootDir\webui"
$BuildDir   = "$RootDir\build"
$OutputDir  = "$RootDir\releases\$Version"

# ----------------------------------------------------------------------------
# AAX SDK: iPlug2 は iPlug2/Dependencies/IPlug/AAX_SDK 固定で参照する
# (AAX.cmake にパスがハードコードされている)。リポジトリ同梱の aax-sdk/ が
# あればそこへコピーしておく (= JUCE 版は $RootDir\aax-sdk を直接使っていた)。
# ----------------------------------------------------------------------------
$AAXSDKPath = "$RootDir\iPlug2\Dependencies\IPlug\AAX_SDK"
$AAXSourceDir = "$RootDir\aax-sdk"
if (-not (Test-Path "$AAXSDKPath\Interfaces\AAX.h") -and (Test-Path "$AAXSourceDir\Interfaces\AAX.h")) {
    Write-Step "Copying aax-sdk into iPlug2/Dependencies/IPlug/AAX_SDK ..."
    New-Item -ItemType Directory -Force -Path $AAXSDKPath | Out-Null
    Copy-Item -Path (Join-Path $AAXSourceDir '*') -Destination $AAXSDKPath -Recurse -Force
    Write-Success "AAX SDK staged"
}

Write-Step "Checking AAX SDK..."
if (Test-Path "$AAXSDKPath\Interfaces\AAX.h") {
    Write-Success "AAX SDK found - AAX will be built"
    $BuildAAX = $true

    # iPlug2 同梱の AAX SDK は古い cmake_minimum_required + ソースツリーを指す PUBLIC
    # include path を持ち、CMake 4.x が INTERFACE_INCLUDE_DIRECTORIES のツリー内パスを
    # 拒否する。PUBLIC の 2 行を $<BUILD_INTERFACE:...> でラップする。
    $AaxLibCMake = "$AAXSDKPath\Libs\AAXLibrary\CMakeLists.txt"
    if ((Test-Path $AaxLibCMake) -and -not (Select-String -Path $AaxLibCMake -SimpleMatch -Pattern 'BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../Interfaces' -Quiet)) {
        Write-Step "Patching AAX SDK CMakeLists.txt for CMake 4.x compatibility..."
        $content = Get-Content -Path $AaxLibCMake -Raw
        $content = $content -replace '(?m)^    \$\{CMAKE_CURRENT_SOURCE_DIR\}/\.\./\.\./Interfaces$', '    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../Interfaces>'
        $content = $content -replace '(?m)^    \$\{CMAKE_CURRENT_SOURCE_DIR\}/\.\./\.\./Interfaces/ACF$', '    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../Interfaces/ACF>'
        Set-Content -Path $AaxLibCMake -Value $content -NoNewline
        Write-Success "AAX SDK patched"
    }
} else {
    Write-Host "AAX SDK not found at: $AAXSDKPath - AAX will be skipped" -ForegroundColor Yellow
    $BuildAAX = $false
}

Write-Step "Creating output directories..."
New-Item -ItemType Directory -Force -Path "$OutputDir\Windows" | Out-Null
Write-Success "Output directories created"

# ----------------------------------------------------------------------------
# Step 1: WebUI production build (→ plugin/resources/web、main.rc が RCDATA 埋め込み)
# ----------------------------------------------------------------------------
Write-Header "Step 1: Building WebUI for production"

$WebOutDir = "$RootDir\plugin\resources\web"
if ($SkipWebUI) {
    Write-Step "Skipping WebUI build (-SkipWebUI)"
    if (-not (Test-Path "$WebOutDir\index.html")) {
        Write-Fail "-SkipWebUI set but no existing WebUI build at $WebOutDir"
        exit 1
    }
} else {
    if (Test-Path $WebOutDir) {
        Write-Step "Cleaning previous WebUI build..."
        Remove-Item -Path $WebOutDir -Recurse -Force
        Write-Success "Previous build cleaned"
    }

    Set-Location $WebUIDir
    if (-not (Test-Path "node_modules")) {
        Write-Step "Installing npm dependencies..."
        npm install
        if ($LASTEXITCODE -ne 0) { Write-Fail "npm install failed"; exit 1 }
    }

    Write-Step "Building WebUI..."
    npm run build
    if ($LASTEXITCODE -ne 0) { Write-Fail "WebUI build failed"; exit 1 }
    Write-Success "WebUI built successfully"

    if (-not (Test-Path "$WebOutDir\index.html")) {
        Write-Fail "WebUI build output not found at $WebOutDir"
        exit 1
    }
}

# ----------------------------------------------------------------------------
# Step 2: Native plugin build (VST3 / CLAP / Standalone / AAX)
# ----------------------------------------------------------------------------
if ($BuildAAX) {
    Write-Header "Step 2: Building VST3, CLAP, Standalone, and AAX"
} else {
    Write-Header "Step 2: Building VST3, CLAP, and Standalone"
}

Set-Location $RootDir
if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
}

# 既存 build/ が別 generator/platform（例: 過去の Ninja や platform 未指定）で
# configure 済みだと "generator platform: x64 / Does not match ..." で落ちる。
# 目的の generator+platform と一致しないキャッシュは安全側で除去してから configure する。
$CacheFile = Join-Path $BuildDir "CMakeCache.txt"
if (Test-Path $CacheFile) {
    $cache = Get-Content $CacheFile -Raw
    $genOk  = $cache -match '(?m)^CMAKE_GENERATOR:.*Visual Studio 17 2022'
    $platOk = $cache -match '(?m)^CMAKE_GENERATOR_PLATFORM:[^=]*=x64'
    if (-not ($genOk -and $platOk)) {
        Write-Step "Existing CMake cache uses a different generator/platform - clearing it..."
        Remove-Item -Path $CacheFile -Force -ErrorAction SilentlyContinue
        Remove-Item -Path (Join-Path $BuildDir "CMakeFiles") -Recurse -Force -ErrorAction SilentlyContinue
        Write-Success "Stale CMake cache cleared"
    }
}

Write-Step "Configuring CMake for $Configuration build..."
cmake -S $RootDir -B $BuildDir -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=$Configuration
if ($LASTEXITCODE -ne 0) { Write-Fail "CMake configuration failed"; exit 1 }

Write-Step "Building VST3..."
cmake --build $BuildDir --config $Configuration --target MixCompare-vst3
if ($LASTEXITCODE -ne 0) { Write-Fail "VST3 build failed"; exit 1 }
Write-Success "VST3 built successfully"

Write-Step "Building CLAP..."
cmake --build $BuildDir --config $Configuration --target MixCompare-clap
if ($LASTEXITCODE -ne 0) { Write-Fail "CLAP build failed"; exit 1 }
Write-Success "CLAP built successfully"

Write-Step "Building Standalone..."
cmake --build $BuildDir --config $Configuration --target MixCompare-app
if ($LASTEXITCODE -ne 0) { Write-Fail "Standalone build failed"; exit 1 }
Write-Success "Standalone built successfully"

if ($BuildAAX) {
    Write-Step "Building AAX..."
    cmake --build $BuildDir --config $Configuration --target MixCompare-aax
    if ($LASTEXITCODE -ne 0) { Write-Fail "AAX build failed"; exit 1 }
    Write-Success "AAX built successfully"
}

# ----------------------------------------------------------------------------
# Step 3: Packaging
# ----------------------------------------------------------------------------
Write-Header "Step 3: Packaging for distribution"

# iPlug2 は全成果物を $BuildDir\out\MixCompare.* に出力する (RUNTIME_OUTPUT_DIRECTORY)。
$SrcVST3       = "$BuildDir\out\MixCompare.vst3"
$SrcCLAP       = "$BuildDir\out\MixCompare.clap"
$SrcStandalone = "$BuildDir\out\MixCompare.exe"
$SrcAAX        = "$BuildDir\out\MixCompare.aaxplugin"

$DestVST3       = "$OutputDir\Windows\MixCompare.vst3"
$DestCLAP       = "$OutputDir\Windows\MixCompare.clap"
$DestStandalone = "$OutputDir\Windows\MixCompare.exe"
$DestAAX        = "$OutputDir\Windows\MixCompare.aaxplugin"

Write-Step "Copying VST3..."
if (Test-Path $SrcVST3) {
    if (Test-Path $DestVST3) { Remove-Item -Path $DestVST3 -Recurse -Force }
    Copy-Item -Path $SrcVST3 -Destination $DestVST3 -Recurse -Force
    Write-Success "VST3 copied"
} else {
    Write-Fail "VST3 not found at: $SrcVST3"
    exit 1
}

Write-Step "Copying CLAP..."
if (Test-Path $SrcCLAP) {
    if (Test-Path $DestCLAP) { Remove-Item -Path $DestCLAP -Force }
    Copy-Item -Path $SrcCLAP -Destination $DestCLAP -Force
    Write-Success "CLAP copied"
} else {
    Write-Fail "CLAP not found at: $SrcCLAP"
    exit 1
}

Write-Step "Copying Standalone..."
if (Test-Path $SrcStandalone) {
    if (Test-Path $DestStandalone) { Remove-Item -Path $DestStandalone -Force }
    Copy-Item -Path $SrcStandalone -Destination $DestStandalone -Force
    Write-Success "Standalone copied"
} else {
    Write-Fail "Standalone not found at: $SrcStandalone"
    exit 1
}

# AAX copy + PACE signing (iLok)
$AAXSignedSuccessfully = $false
$AAXSigningStatus = "unsigned_developer"

if ($BuildAAX) {
    Write-Step "Copying AAX..."
    if (Test-Path $SrcAAX) {
        if (Test-Path $DestAAX) { Remove-Item -Path $DestAAX -Recurse -Force }
        New-Item -ItemType Directory -Force -Path $DestAAX | Out-Null
        Copy-Item -Path (Join-Path $SrcAAX '*') -Destination $DestAAX -Recurse -Force
        Write-Success "AAX copied (unsigned)"

        Write-Step "Signing AAX with PACE Eden tools..."
        $WrapToolPath = "C:\Program Files (x86)\PACEAntiPiracy\Eden\Fusion\Versions\5\wraptool.exe"

        if (-not (Test-Path $WrapToolPath)) {
            Write-Host "Warning: PACE Eden wraptool not found at $WrapToolPath" -ForegroundColor Yellow
            $AAXSigningStatus = "wraptool_missing"
        } else {
            $PaceVars = @("PACE_USERNAME", "PACE_PASSWORD", "PACE_ORGANIZATION")
            $MissingVars = @($PaceVars | Where-Object { -not (Get-Item "env:$_" -ErrorAction SilentlyContinue) })

            if ($MissingVars.Count -gt 0) {
                Write-Host "Missing PACE env vars: $($MissingVars -join ', ')" -ForegroundColor Yellow
                $AAXSigningStatus = "credentials_missing"
            } else {
                # --signtool 経路では PACE_FUSION_HOME (= wraptool.exe のディレクトリ) が必須。
                $env:PACE_FUSION_HOME = Split-Path $WrapToolPath -Parent

                # Authenticode は azuresigntool で Azure KV の正規証明書を使う。CA/B 2023-06
                # 以降コード署名鍵は非エクスポート (HSM/KV) で .pfx を作れないため、wraptool の
                # --keyfile ではなく --signtool に aax-signtool.bat を渡し wrap の最終工程で KV
                # 署名させる (aax-signtool.bat は CODESIGN_* 環境変数を読む)。
                $AaxWrapper = "$RootDir\aax-signtool.bat"
                $HaveAst = [bool](Get-Command azuresigntool -ErrorAction SilentlyContinue)

                $SigningArgs = $null
                $SignMethod  = ""
                if (-not $SkipCodeSign -and $HaveAst -and (Test-Path $AaxWrapper)) {
                    $env:CODESIGN_KVU = $AzureKeyVaultUrl
                    $env:CODESIGN_KVC = $AzureCertName
                    $env:CODESIGN_TR  = $TimestampUrl
                    $env:CODESIGN_AZURESIGNTOOL = (Get-Command azuresigntool).Source
                    $SigningArgs = @(
                        "sign", "--verbose", "--installedbinaries",
                        "--signtool", $AaxWrapper, "--signid", "1",
                        "--account",  $env:PACE_USERNAME,
                        "--password", $env:PACE_PASSWORD,
                        "--wcguid",   $env:PACE_ORGANIZATION,
                        "--in",  $DestAAX,
                        "--out", $DestAAX
                    )
                    $SignMethod = "Azure Key Vault ($AzureCertName)"
                } else {
                    # フォールバック: dev pfx で PACE + Authenticode 署名 (Pro Tools 動作には十分だが
                    # Authenticode ルートは未信頼)。-SkipCodeSign / azuresigntool 未検出時。
                    $PfxCandidates = @($env:PACE_PFX_PATH, "$RootDir\mixcompare-dev.pfx", "$env:USERPROFILE\.mixcompare\dev.pfx")
                    $PfxPath = $null
                    foreach ($candidate in $PfxCandidates) {
                        if ($candidate -and (Test-Path $candidate)) { $PfxPath = $candidate; break }
                    }
                    if (-not $PfxPath -or -not (Get-Item "env:PACE_KEYPASSWORD" -ErrorAction SilentlyContinue)) {
                        Write-Host "Warning: Azure signing unavailable and no usable dev pfx/PACE_KEYPASSWORD - AAX left unsigned" -ForegroundColor Yellow
                        $AAXSigningStatus = "certificate_missing"
                    } else {
                        $SigningArgs = @(
                            "sign", "--verbose",
                            "--account",  $env:PACE_USERNAME,
                            "--password", $env:PACE_PASSWORD,
                            "--wcguid",   $env:PACE_ORGANIZATION,
                            "--keyfile",  $PfxPath,
                            "--keypassword", $env:PACE_KEYPASSWORD,
                            "--in",  $DestAAX,
                            "--out", $DestAAX
                        )
                        $SignMethod = "dev pfx ($([IO.Path]::GetFileName($PfxPath)))"
                    }
                }

                if ($SigningArgs) {
                    Write-Step "AAX signing method: $SignMethod"
                    $SigningOutput = & $WrapToolPath $SigningArgs 2>&1
                    $SigningExitCode = $LASTEXITCODE
                    $SigningOutput | ForEach-Object { Write-Host "    $_" -ForegroundColor Gray }

                    if ($SigningExitCode -eq 0) {
                        Write-Success "AAX signed successfully ($SignMethod)"
                        $AAXSignedSuccessfully = $true
                        $AAXSigningStatus = if ($SignMethod -like 'Azure*') { "signed_kv" } else { "signed_devcert" }
                    } else {
                        Write-Host "Warning: AAX signing failed with exit code: $SigningExitCode" -ForegroundColor Yellow
                        $AAXSigningStatus = "signing_failed"
                    }
                }
            }
        }
    } else {
        Write-Fail "AAX not found at: $SrcAAX"
        exit 1
    }
}

# ----------------------------------------------------------------------------
# Step 3.5: Authenticode signing (VST3 DLL + CLAP + Standalone exe)
# ----------------------------------------------------------------------------
# ZIP / インストーラより前に署名し、配布物が署名済みバイナリを含むようにする。
# AAX はここでは署名しない (Step 3 の PACE wrap で一体署名済み)。
Write-Header "Step 3.5: Signing Windows binaries (Authenticode)"
$VST3InnerPE = Join-Path $DestVST3 "Contents\x86_64-win\MixCompare.vst3"
Invoke-AuthenticodeSign -Paths @($VST3InnerPE, $DestCLAP, $DestStandalone) | Out-Null

# ReadMe
Write-Step "Creating documentation..."
$ReadmeContent = @"
MixCompare $Version - Windows Installation Guide
====================================================

Important: Required Software
-------------------
This plugin requires the Microsoft Visual C++ 2019 Redistributable Package.
If the plugin fails to load, install it from:
https://aka.ms/vs/17/release/vc_redist.x64.exe

Installation Steps
-------------------
1. Close your DAW before proceeding.

2. For VST3 Plugin:
   Copy MixCompare.vst3 to:
   C:\Program Files\Common Files\VST3\

3. For CLAP Plugin:
   Copy MixCompare.clap to:
   C:\Program Files\Common Files\CLAP\

4. For Standalone Application:
   Copy MixCompare.exe to any preferred location.
"@

if ($BuildAAX) {
    $ReadmeContent += @"


5. For AAX Plugin (Pro Tools):
   Copy MixCompare.aaxplugin to:
   C:\Program Files\Common Files\Avid\Audio\Plug-Ins\
"@
}

$ReadmeContent += @"


Launch your DAW and rescan plugins.
"@

$ReadmeContent | Out-File -FilePath "$OutputDir\Windows\ReadMe.txt" -Encoding UTF8
Write-Success "Documentation created"

# version.json
$formats = @("VST3", "CLAP", "Standalone")
if ($BuildAAX) { $formats += "AAX" }

$VersionInfo = @{
    name        = "MixCompare"
    version     = $Version
    build_date  = $BuildDate
    platform    = "Windows"
    architecture = "x64"
    formats     = $formats
    webui       = "embedded"
    build_type  = $Configuration
    aax_signing = if ($BuildAAX) { $AAXSigningStatus } else { "N/A" }
    code_signing = $CodeSigningStatus
} | ConvertTo-Json

$VersionInfo | Out-File -FilePath "$OutputDir\Windows\version.json" -Encoding UTF8
Write-Success "Version info created"

# ZIP archive
Write-Step "Creating ZIP archive..."
if ($BuildAAX) {
    $ZipName = "MixCompare_${Version}_Windows_VST3_CLAP_AAX_Standalone.zip"
} else {
    $ZipName = "MixCompare_${Version}_Windows_VST3_CLAP_Standalone.zip"
}
$ZipPath = "$OutputDir\$ZipName"

if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }

# AGPL-3.0-or-later: バイナリ配布にライセンスを同梱する。
if (Test-Path "$RootDir\LICENSE") {
    Copy-Item -Path "$RootDir\LICENSE" -Destination "$OutputDir\Windows\LICENSE.txt" -Force
}

Compress-Archive -Path "$OutputDir\Windows\*" -DestinationPath $ZipPath -CompressionLevel Optimal
Write-Success "ZIP archive created"

# ----------------------------------------------------------------------------
# Step 4: Inno Setup installer (optional)
# ----------------------------------------------------------------------------
Write-Header "Step 4: Creating installer with Inno Setup"

$InnoSetupPath = "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
$InstallerScript = "$RootDir\installer.iss"

if (Test-Path $InnoSetupPath) {
    if (Test-Path $InstallerScript) {
        Write-Step "Building installer with Inno Setup..."
        & $InnoSetupPath /DMyAppVersion="$Version" /Q $InstallerScript
        if ($LASTEXITCODE -eq 0) {
            Write-Success "Installer created successfully"
            $InstallerExe = "$OutputDir\MixCompare_${Version}_Windows_Setup.exe"
            Invoke-AuthenticodeSign -Paths @($InstallerExe) | Out-Null
        } else {
            Write-Host "Warning: Installer creation failed (exit $LASTEXITCODE)" -ForegroundColor Yellow
        }
    } else {
        Write-Host "Warning: installer.iss not found - skipping installer creation" -ForegroundColor Yellow
    }
} else {
    Write-Host "Warning: Inno Setup not found - skipping installer creation" -ForegroundColor Yellow
}

# Final summary
$FileInfo = Get-Item $ZipPath
$SizeMB = [math]::Round($FileInfo.Length / 1MB, 2)

Write-Header "Build completed successfully!"
Write-Host "Package: $ZipPath" -ForegroundColor White
Write-Host "Size:    $SizeMB MB" -ForegroundColor White
Write-Host ""
if ($CodeSigningStatus -eq "signed") {
    Write-Host "[OK]  VST3 / CLAP / Standalone / Installer signed via Azure Key Vault" -ForegroundColor Green
} else {
    Write-Host "[!!]  Authenticode signing: $CodeSigningStatus" -ForegroundColor Yellow
}
if ($BuildAAX) {
    if ($AAXSigningStatus -eq "signed_kv") {
        Write-Host "[OK]  AAX signed via PACE Eden + Azure Key Vault (trusted Authenticode)" -ForegroundColor Green
    } elseif ($AAXSigningStatus -eq "signed_devcert") {
        Write-Host "[OK]  AAX signed via PACE Eden + dev cert (Authenticode root NOT trusted)" -ForegroundColor Yellow
    } else {
        Write-Host "[!!]  AAX is unsigned ($AAXSigningStatus)" -ForegroundColor Yellow
    }
}
