#!/bin/zsh

# MixCompare macOS Release Build Script (zsh) — iPlug2 edition
# - WebUI 本番ビルド → CMake (Xcode) で VST3/AU/CLAP/Standalone (+ AAX) Universal Binary
# - Hardened Runtime コード署名 → AAX PACE (iLok) 署名（任意）
# - 言語別ライセンス (en/ja) を持つ署名済み PKG → notarytool で公証 → stapler 添付
# - 互換用 ZIP も同時生成（build_windows.ps1 と同じ流儀）
#
# 注: JUCE 版では全フォーマットに同一 CFBundleIdentifier が振られ、Logic 等で
# コンポーネント選択が壊れるため署名前に PlistBuddy で一意化していた。iPlug2 は
# BUNDLE_ID = BUNDLE_DOMAIN.BUNDLE_MFR.<API_EXT>.BUNDLE_NAME（API_EXT は
# vst3/audiounit/aax/app/clap でフォーマット毎に異なる）を生成するので、
# 手動の一意化処理は不要。
#
# 必須環境変数:
#   CODESIGN_IDENTITY  : "Developer ID Application: Your Name (TEAMID)"（未設定なら自動検出）
#   INSTALLER_IDENTITY : "Developer ID Installer: ..."（未設定なら自動検出）
#   公証はいずれか一組:
#     (A) APPLE_API_KEY_PATH / APPLE_API_KEY_ID(/APPLE_API_KEY) / APPLE_API_ISSUER
#     (B) NOTARYTOOL_PROFILE
#     (C) APPLE_ID / APP_PASSWORD / TEAM_ID
# 任意環境変数:
#   ENTITLEMENTS_PATH  : 付与する entitlements の .plist パス
#   PACE_USERNAME / PACE_PASSWORD / PACE_ORGANIZATION : AAX の iLok 署名（任意）
#   WRAPTOOL_PATH      : wraptool の明示パス
#   PKG_ID_BASE        : pkg 識別子のベース（既定 com.bucketrelay.mixcompare3）
#   SKIP_WEBUI=1       : WebUI ビルドをスキップ

set -e
set -u
set -o pipefail

color_cyan="\033[36m"
color_yellow="\033[33m"
color_green="\033[32m"
color_red="\033[31m"
color_gray="\033[90m"
color_reset="\033[0m"

echo_header()  { echo ""; echo -e "${color_cyan}============================================${color_reset}"; echo -e "${color_cyan}   $1${color_reset}"; echo -e "${color_cyan}============================================${color_reset}"; echo ""; }
echo_step()    { echo -e "${color_yellow}>> $1${color_reset}"; }
echo_success() { echo -e "${color_green}[OK] $1${color_reset}"; }
echo_warn()    { echo -e "${color_yellow}[!!] $1${color_reset}"; }
echo_error()   { echo -e "${color_red}[FAIL] $1${color_reset}" 1>&2; }

CONFIGURATION="Release"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --config|-c) CONFIGURATION="${2:-Release}"; shift 2 ;;
        --skip-webui) export SKIP_WEBUI="1"; shift 1 ;;
        *) echo_error "Unknown argument: $1"; exit 1 ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="${SCRIPT_DIR}"
VERSION_FILE="${ROOT_DIR}/VERSION"

[[ -f "${VERSION_FILE}" ]] || { echo_error "VERSION file not found: ${VERSION_FILE}"; exit 1; }
VERSION="$(cat "${VERSION_FILE}" | tr -d '\r' | tr -d '\n')"
BUILD_DATE="$(date +%Y-%m-%d)"

# Load .env if present
ENV_FILE="${ROOT_DIR}/.env"
if [[ -f "${ENV_FILE}" ]]; then
    echo -e "${color_gray}Loading environment variables from .env ...${color_reset}"
    while IFS= read -r line || [[ -n "$line" ]]; do
        line="${line## }"; line="${line%% }"
        [[ -z "$line" || "$line" == \#* ]] && continue
        key="${line%%=*}"; value="${line#*=}"
        value="${value#\"}"; value="${value%\"}"
        value="${value#\'}"; value="${value%\'}"
        if [[ -z "${(P)key:-}" ]]; then export "$key=$value"; fi
    done < "${ENV_FILE}"
fi

echo_header "MixCompare ${VERSION} Build Script (macOS zsh, iPlug2)"

WEBUI_DIR="${ROOT_DIR}/webui"
BUILD_DIR="${ROOT_DIR}/build"
OUTPUT_DIR="${ROOT_DIR}/releases/${VERSION}/macOS"
# iPlug2 の AAX 検出は ${IPLUG_DEPS_DIR}/AAX_SDK で hard-coded。
AAX_SDK_PATH="${ROOT_DIR}/iPlug2/Dependencies/IPlug/AAX_SDK"

echo_step "Checking AAX SDK..."
if [[ -f "${AAX_SDK_PATH}/Interfaces/AAX.h" ]]; then
    echo_success "AAX SDK found - AAX will be built"
    BUILD_AAX=1

    # iPlug2 がバンドルしている AAX SDK は cmake_minimum_required(3.12) で書かれて
    # おり、PUBLIC な include path が "../../Interfaces" のような相対パス。CMake 4.x
    # は INTERFACE_INCLUDE_DIRECTORIES がソースツリー内を指すと拒否するので、
    # PUBLIC の二行を $<BUILD_INTERFACE:...> でラップしてから configure に渡す。
    AAX_LIB_CMAKE="${AAX_SDK_PATH}/Libs/AAXLibrary/CMakeLists.txt"
    if [[ -f "${AAX_LIB_CMAKE}" ]] && ! grep -qF 'BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../Interfaces' "${AAX_LIB_CMAKE}"; then
        echo_step "Patching AAX SDK CMakeLists.txt for CMake 4.x compatibility..."
        # macOS の sed は -i に空文字を要求する（GNU sed と差異あり）。
        /usr/bin/sed -i '' \
            -e 's|^    \${CMAKE_CURRENT_SOURCE_DIR}/\.\./\.\./Interfaces$|    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../Interfaces>|' \
            -e 's|^    \${CMAKE_CURRENT_SOURCE_DIR}/\.\./\.\./Interfaces/ACF$|    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../Interfaces/ACF>|' \
            "${AAX_LIB_CMAKE}"
        echo_success "AAX SDK patched"
    fi
else
    echo_warn "AAX SDK not found at ${AAX_SDK_PATH} - AAX will be skipped"
    BUILD_AAX=0
fi

mkdir -p "${OUTPUT_DIR}"

# ----------------------------------------------------------------------------
# Step 1: WebUI build
# ----------------------------------------------------------------------------
echo_header "Step 1: Building WebUI for production"

if [[ "${SKIP_WEBUI:-0}" != "1" ]]; then
    [[ -d "${WEBUI_DIR}" ]] || { echo_error "WebUI directory not found: ${WEBUI_DIR}"; exit 1; }

    # Vite outDir は plugin/resources/web (小文字)。LoadIndexHtml(__FILE__,...) の
    # Debug 経路は parent_path() / "Resources" / "web" を引くが、APFS 既定の
    # case-insensitive 解決で同じファイルにヒットする。
    WEB_OUT_DIR="${ROOT_DIR}/plugin/resources/web"
    [[ -d "${WEB_OUT_DIR}" ]] && rm -rf "${WEB_OUT_DIR}"

    pushd "${WEBUI_DIR}" >/dev/null
    if [[ ! -d node_modules ]]; then
        echo_step "Installing npm dependencies..."
        npm install --no-audit --no-fund
    fi
    echo_step "Building WebUI..."
    npm run build
    popd >/dev/null

    [[ -f "${WEB_OUT_DIR}/index.html" ]] || { echo_error "WebUI build output not found at ${WEB_OUT_DIR}"; exit 1; }
    echo_success "WebUI build completed"
else
    echo_step "Skipping WebUI build (SKIP_WEBUI=1)"
fi

# ----------------------------------------------------------------------------
# Step 2: Native plugin build (Universal Binary)
# ----------------------------------------------------------------------------
if [[ ${BUILD_AAX} -eq 1 ]]; then
    echo_header "Step 2: Building Plugins (VST3/AU/CLAP/Standalone/AAX)"
else
    echo_header "Step 2: Building Plugins (VST3/AU/CLAP/Standalone)"
fi

echo_step "CMake configuration (${CONFIGURATION}, Universal Binary)..."
# IPLUG2_UNIVERSAL=ON は CMAKE_OSX_ARCHITECTURES と Xcode の ARCHS / ONLY_ACTIVE_ARCH
# を一括設定する iPlug2 提供のフラグ。明示的な CMAKE_OSX_ARCHITECTURES 上書きより
# 推奨経路。
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -G Xcode \
    -DCMAKE_BUILD_TYPE="${CONFIGURATION}" \
    -DIPLUG2_UNIVERSAL=ON

# iPlug2 の iplug_add_plugin が生成するターゲット名はハイフン区切り小文字。
TARGETS=(MixCompare-vst3 MixCompare-au MixCompare-clap MixCompare-app)
if [[ ${BUILD_AAX} -eq 1 ]]; then
    TARGETS+=(MixCompare-aax)
fi

echo_step "Executing build (${TARGETS[@]})..."
cmake --build "${BUILD_DIR}" --config "${CONFIGURATION}" --target ${TARGETS[@]}
echo_success "Plugin build completed"

# iPlug2 の出力先は build/out/<CONFIG>/MixCompare.{vst3,component,clap,app,aaxplugin}。
# Windows 版 (build_windows.ps1) では iplug2 が _DEBUG/_RELEASE 用の
# RUNTIME_OUTPUT_DIRECTORY をフラットに上書きするので out/ 直下に出るが、
# macOS は generic な RUNTIME_OUTPUT_DIRECTORY しか設定していないため、Xcode
# generator が $(CONFIGURATION) を自動付与する。
ARTIFACTS_DIR="${BUILD_DIR}/out/${CONFIGURATION}"
SRC_VST3="${ARTIFACTS_DIR}/MixCompare.vst3"
SRC_AU="${ARTIFACTS_DIR}/MixCompare.component"
SRC_CLAP="${ARTIFACTS_DIR}/MixCompare.clap"
SRC_APP="${ARTIFACTS_DIR}/MixCompare.app"

[[ -d "${SRC_VST3}" ]] || { echo_error "VST3 not found: ${SRC_VST3}"; exit 1; }
[[ -d "${SRC_AU}" ]]   || { echo_error "AU not found: ${SRC_AU}";     exit 1; }
[[ -d "${SRC_CLAP}" ]] || { echo_error "CLAP not found: ${SRC_CLAP}"; exit 1; }
[[ -d "${SRC_APP}" ]]  || { echo_error "Standalone not found: ${SRC_APP}"; exit 1; }

if [[ ${BUILD_AAX} -eq 1 ]]; then
    SRC_AAX="${ARTIFACTS_DIR}/MixCompare.aaxplugin"
    [[ -d "${SRC_AAX}" ]] || { echo_error "AAX not found: ${SRC_AAX}"; exit 1; }
fi

# Universal Binary の確認
echo_step "Verifying Universal Binary slices..."
for bundle in "${SRC_VST3}" "${SRC_AU}" "${SRC_CLAP}" "${SRC_APP}"; do
    bin="${bundle}/Contents/MacOS/MixCompare"
    [[ -f "${bin}" ]] || bin="${bundle}/Contents/MacOS/$(basename "${bundle}" | sed 's/\..*//')"
    if [[ -f "${bin}" ]]; then
        info="$(lipo -info "${bin}" 2>/dev/null || true)"
        echo -e "${color_gray}  $(basename "${bundle}"): ${info}${color_reset}"
    fi
done

echo_step "Collecting artifacts..."
DEST_VST3="${OUTPUT_DIR}/MixCompare.vst3"
DEST_AU="${OUTPUT_DIR}/MixCompare.component"
DEST_CLAP="${OUTPUT_DIR}/MixCompare.clap"
DEST_APP="${OUTPUT_DIR}/MixCompare.app"
rm -rf "${DEST_VST3}" "${DEST_AU}" "${DEST_CLAP}" "${DEST_APP}"
cp -R "${SRC_VST3}" "${DEST_VST3}"
cp -R "${SRC_AU}"   "${DEST_AU}"
cp -R "${SRC_CLAP}" "${DEST_CLAP}"
cp -R "${SRC_APP}"  "${DEST_APP}"

if [[ ${BUILD_AAX} -eq 1 ]]; then
    DEST_AAX="${OUTPUT_DIR}/MixCompare.aaxplugin"
    rm -rf "${DEST_AAX}"
    cp -R "${SRC_AAX}" "${DEST_AAX}"
fi

# ----------------------------------------------------------------------------
# Step 3: Codesign (Hardened Runtime)
# ----------------------------------------------------------------------------
echo_header "Step 3: Code Signing (Hardened Runtime)"

if [[ -z "${CODESIGN_IDENTITY:-}" ]]; then
    echo_step "CODESIGN_IDENTITY not set, attempting auto-detection..."
    if [[ -n "${CODESIGN_TEAM_ID:-}" ]]; then
        CODESIGN_IDENTITY=$(security find-identity -v -p codesigning 2>/dev/null | awk -v team="${CODESIGN_TEAM_ID}" -F '"' '/Developer ID Application:/ && $0 ~ team {print $2; exit}') || true
    fi
    if [[ -z "${CODESIGN_IDENTITY:-}" ]]; then
        CODESIGN_IDENTITY=$(security find-identity -v -p codesigning 2>/dev/null | awk -F '"' '/Developer ID Application:/ {print $2; exit}') || true
    fi
    [[ -n "${CODESIGN_IDENTITY:-}" ]] || { echo_error "CODESIGN_IDENTITY not found. Install a Developer ID Application certificate and retry."; exit 1; }
    echo_success "Auto-selected signing ID: ${CODESIGN_IDENTITY}"
fi

sign_bundle() {
    local bundle_path="$1"
    local entitlements_args=()
    if [[ -n "${ENTITLEMENTS_PATH:-}" && -f "${ENTITLEMENTS_PATH}" ]]; then
        entitlements_args=(--entitlements "${ENTITLEMENTS_PATH}")
    fi
    codesign --force --timestamp --options runtime "${entitlements_args[@]}" --sign "${CODESIGN_IDENTITY}" "${bundle_path}"
    codesign --verify --deep --strict --verbose=2 "${bundle_path}"
}

echo_step "Signing VST3...";       sign_bundle "${DEST_VST3}"; echo_success "VST3 signing OK"
echo_step "Signing AU...";         sign_bundle "${DEST_AU}";   echo_success "AU signing OK"
echo_step "Signing CLAP...";       sign_bundle "${DEST_CLAP}"; echo_success "CLAP signing OK"
echo_step "Signing Standalone..."; sign_bundle "${DEST_APP}";  echo_success "Standalone signing OK"
if [[ ${BUILD_AAX} -eq 1 ]]; then
    echo_step "Signing AAX (developer signature, before PACE)..."
    sign_bundle "${DEST_AAX}"; echo_success "AAX dev signing OK"
fi

# ----------------------------------------------------------------------------
# Step 3.5: AAX PACE / iLok signing (optional, mirrors build_windows.ps1)
# ----------------------------------------------------------------------------
AAX_PACE_STATUS="not_attempted"
if [[ ${BUILD_AAX} -eq 1 ]]; then
    echo_header "Step 3.5: AAX PACE (iLok) signing"

    # 旧スクリプト互換: PACE_WCGUID が設定されていて PACE_ORGANIZATION が未設定なら採用。
    PACE_ORG_EFFECTIVE="${PACE_ORGANIZATION:-${PACE_WCGUID:-}}"

    WRAPTOOL_PATH_CANDIDATES=()
    [[ -n "${WRAPTOOL_PATH:-}" ]] && WRAPTOOL_PATH_CANDIDATES+=("${WRAPTOOL_PATH}")
    WRAPTOOL_PATH_CANDIDATES+=(
        "/Applications/PACEAntiPiracy/Eden/Fusion/Versions/5/bin/wraptool"
        "/Applications/PACE Anti-Piracy/Eden/Fusion/Versions/5/bin/wraptool"
        "/Applications/PACEAntiPiracy/Eden/Fusion/Versions/5/wraptool"
        "/Applications/PACE Anti-Piracy/Eden/Fusion/Versions/5/wraptool"
        "/usr/local/bin/wraptool"
        "/opt/local/bin/wraptool"
    )
    FOUND_WRAPTOOL=""
    for p in "${WRAPTOOL_PATH_CANDIDATES[@]}"; do
        if [[ -x "$p" ]]; then FOUND_WRAPTOOL="$p"; break; fi
    done

    MISSING_PACE_VARS=()
    [[ -z "${PACE_USERNAME:-}" ]]      && MISSING_PACE_VARS+=("PACE_USERNAME")
    [[ -z "${PACE_PASSWORD:-}" ]]      && MISSING_PACE_VARS+=("PACE_PASSWORD")
    [[ -z "${PACE_ORG_EFFECTIVE}" ]]   && MISSING_PACE_VARS+=("PACE_ORGANIZATION")

    if [[ -z "${FOUND_WRAPTOOL}" ]]; then
        echo_warn "wraptool not found - skipping PACE signing (set WRAPTOOL_PATH to override)."
        AAX_PACE_STATUS="wraptool_missing"
    elif (( ${#MISSING_PACE_VARS[@]} > 0 )); then
        echo_warn "Missing PACE credentials: ${MISSING_PACE_VARS[*]} - skipping PACE signing."
        AAX_PACE_STATUS="credentials_missing"
    else
        echo_step "Signing AAX with PACE wraptool..."
        # パスワードはログに出さない
        echo -e "${color_gray}  ${FOUND_WRAPTOOL} sign --account ${PACE_USERNAME} --password *** --wcguid ${PACE_ORG_EFFECTIVE} --signid \"${CODESIGN_IDENTITY}\" --in ${DEST_AAX} --out ${DEST_AAX}${color_reset}"
        if "${FOUND_WRAPTOOL}" sign \
                --verbose \
                --account "${PACE_USERNAME}" \
                --password "${PACE_PASSWORD}" \
                --wcguid "${PACE_ORG_EFFECTIVE}" \
                --signid "${CODESIGN_IDENTITY}" \
                --dsigharden \
                --dsig1-compat on \
                --in "${DEST_AAX}" \
                --out "${DEST_AAX}"; then
            echo_success "AAX PACE signed"
            AAX_PACE_STATUS="signed"
        else
            echo_warn "AAX PACE signing failed - continuing with developer-signed build"
            AAX_PACE_STATUS="signing_failed"
        fi
    fi
fi

# ----------------------------------------------------------------------------
# Step 4: PKG composition with localized licenses (en / ja)
# ----------------------------------------------------------------------------
echo_header "Step 4: Building component PKGs and localized product PKG"

PKG_WORK_DIR="${OUTPUT_DIR}/pkgwork"
rm -rf "${PKG_WORK_DIR}"
mkdir -p "${PKG_WORK_DIR}"
# 既存リリース (3.0.x) と同じ識別子ベースを維持し、クリーンなアップグレードを保証する。
PKG_ID_BASE="${PKG_ID_BASE:-com.bucketrelay.mixcompare3}"

build_component_pkg() {
    local kind="$1" src="$2" dst_path="$3"
    local pkgroot="${PKG_WORK_DIR}/root_${kind}"
    rm -rf "${pkgroot}"
    mkdir -p "${pkgroot}${dst_path}"
    cp -R "${src}" "${pkgroot}${dst_path}/"
    pkgbuild \
        --root "${pkgroot}" \
        --identifier "${PKG_ID_BASE}.${kind}" \
        --version "${VERSION}" \
        --install-location "/" \
        --ownership recommended \
        "${PKG_WORK_DIR}/MixCompare_${kind}.pkg"
}

build_component_pkg "vst3" "${DEST_VST3}" "/Library/Audio/Plug-Ins/VST3"
build_component_pkg "au"   "${DEST_AU}"   "/Library/Audio/Plug-Ins/Components"
build_component_pkg "clap" "${DEST_CLAP}" "/Library/Audio/Plug-Ins/CLAP"
pkgbuild --component "${DEST_APP}" --identifier "${PKG_ID_BASE}.app" --version "${VERSION}" \
    --install-location "/Applications" "${PKG_WORK_DIR}/MixCompare_app.pkg"

if [[ ${BUILD_AAX} -eq 1 ]]; then
    build_component_pkg "aax" "${DEST_AAX}" "/Library/Application Support/Avid/Audio/Plug-Ins"
fi

# 言語別ライセンスを Resources/<lang>.lproj/License.txt に配置すると、
# productbuild の Distribution.xml の <license file="License.txt"/> が自動的に
# OS ロケールに応じて切り替えてくれる。
RESOURCES_DIR="${PKG_WORK_DIR}/resources"
mkdir -p "${RESOURCES_DIR}/en.lproj" "${RESOURCES_DIR}/ja.lproj"
[[ -f "${ROOT_DIR}/LICENSE" ]]       && cp "${ROOT_DIR}/LICENSE"       "${RESOURCES_DIR}/en.lproj/License.txt"
[[ -f "${ROOT_DIR}/LICENSE.ja.md" ]] && cp "${ROOT_DIR}/LICENSE.ja.md" "${RESOURCES_DIR}/ja.lproj/License.txt"
# ja ライセンスが無ければ en をフォールバックとして配置（ロケール切替が空にならないように）
[[ -f "${RESOURCES_DIR}/ja.lproj/License.txt" ]] || \
    { [[ -f "${RESOURCES_DIR}/en.lproj/License.txt" ]] && cp "${RESOURCES_DIR}/en.lproj/License.txt" "${RESOURCES_DIR}/ja.lproj/License.txt"; }

DIST_XML="${PKG_WORK_DIR}/Distribution.xml"
{
    echo "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    echo "<installer-gui-script minSpecVersion=\"1\">"
    echo "  <title>MixCompare ${VERSION}</title>"
    echo "  <options customize=\"always\" allow-external-scripts=\"no\"/>"
    echo "  <domains enable_currentUserHome=\"true\" enable_localSystem=\"true\"/>"
    [[ -f "${RESOURCES_DIR}/en.lproj/License.txt" ]] && echo "  <license file=\"License.txt\"/>"
    echo "  <choices-outline>"
    echo "    <line choice=\"choice_vst3\"/>"
    echo "    <line choice=\"choice_au\"/>"
    echo "    <line choice=\"choice_clap\"/>"
    echo "    <line choice=\"choice_app\"/>"
    [[ ${BUILD_AAX} -eq 1 ]] && echo "    <line choice=\"choice_aax\"/>"
    echo "  </choices-outline>"
    echo "  <choice id=\"choice_vst3\" title=\"VST3 Plugin\" enabled=\"true\" selected=\"true\">"
    echo "    <pkg-ref id=\"${PKG_ID_BASE}.vst3\"/>"
    echo "  </choice>"
    echo "  <choice id=\"choice_au\" title=\"Audio Unit (AU)\" enabled=\"true\" selected=\"true\">"
    echo "    <pkg-ref id=\"${PKG_ID_BASE}.au\"/>"
    echo "  </choice>"
    echo "  <choice id=\"choice_clap\" title=\"CLAP Plugin\" enabled=\"true\" selected=\"true\">"
    echo "    <pkg-ref id=\"${PKG_ID_BASE}.clap\"/>"
    echo "  </choice>"
    echo "  <choice id=\"choice_app\" title=\"Standalone Application\" enabled=\"true\" selected=\"true\">"
    echo "    <pkg-ref id=\"${PKG_ID_BASE}.app\"/>"
    echo "  </choice>"
    if [[ ${BUILD_AAX} -eq 1 ]]; then
        echo "  <choice id=\"choice_aax\" title=\"AAX (Pro Tools)\" enabled=\"true\" selected=\"true\">"
        echo "    <pkg-ref id=\"${PKG_ID_BASE}.aax\"/>"
        echo "  </choice>"
    fi
    echo "  <pkg-ref id=\"${PKG_ID_BASE}.vst3\">MixCompare_vst3.pkg</pkg-ref>"
    echo "  <pkg-ref id=\"${PKG_ID_BASE}.au\">MixCompare_au.pkg</pkg-ref>"
    echo "  <pkg-ref id=\"${PKG_ID_BASE}.clap\">MixCompare_clap.pkg</pkg-ref>"
    echo "  <pkg-ref id=\"${PKG_ID_BASE}.app\">MixCompare_app.pkg</pkg-ref>"
    [[ ${BUILD_AAX} -eq 1 ]] && echo "  <pkg-ref id=\"${PKG_ID_BASE}.aax\">MixCompare_aax.pkg</pkg-ref>"
    echo "</installer-gui-script>"
} > "${DIST_XML}"

if [[ -z "${INSTALLER_IDENTITY:-}" ]]; then
    echo_step "INSTALLER_IDENTITY not set, attempting auto-detection..."
    INSTALLER_IDENTITY=$(security find-identity -v 2>/dev/null | awk -F '"' '/Developer ID Installer:/ {print $2; exit}') || true
    [[ -n "${INSTALLER_IDENTITY:-}" ]] || { echo_error "Developer ID Installer certificate not found"; exit 1; }
    echo_success "Auto-selected installer signing ID: ${INSTALLER_IDENTITY}"
fi

PRODUCT_PKG_PATH="${OUTPUT_DIR}/../MixCompare_${VERSION}_macOS.pkg"
echo_step "productbuild + sign..."
productbuild \
    --distribution "${DIST_XML}" \
    --package-path "${PKG_WORK_DIR}" \
    --resources "${RESOURCES_DIR}" \
    --sign "${INSTALLER_IDENTITY}" \
    "${PRODUCT_PKG_PATH}"
echo_success "Signed product PKG: ${PRODUCT_PKG_PATH}"

# ----------------------------------------------------------------------------
# Step 5: Notarize + staple
# ----------------------------------------------------------------------------
echo_header "Step 5: Notarization and stapling"

API_KEY_ID_EFFECTIVE="${APPLE_API_KEY_ID:-}"
[[ -z "${API_KEY_ID_EFFECTIVE}" && -n "${APPLE_API_KEY:-}" ]] && API_KEY_ID_EFFECTIVE="${APPLE_API_KEY}"

if [[ -n "${APPLE_API_KEY_PATH:-}" && -n "${API_KEY_ID_EFFECTIVE}" && -n "${APPLE_API_ISSUER:-}" ]]; then
    echo_step "Submitting via App Store Connect API key..."
    xcrun notarytool submit "${PRODUCT_PKG_PATH}" \
        --key "${APPLE_API_KEY_PATH}" --key-id "${API_KEY_ID_EFFECTIVE}" --issuer "${APPLE_API_ISSUER}" --wait
elif [[ -n "${NOTARYTOOL_PROFILE:-}" ]]; then
    echo_step "Submitting via keychain profile (${NOTARYTOOL_PROFILE})..."
    xcrun notarytool submit "${PRODUCT_PKG_PATH}" --keychain-profile "${NOTARYTOOL_PROFILE}" --wait
elif [[ -n "${APPLE_ID:-}" && -n "${APP_PASSWORD:-}" && -n "${TEAM_ID:-}" ]]; then
    echo_step "Submitting via Apple ID + app-specific password..."
    xcrun notarytool submit "${PRODUCT_PKG_PATH}" --apple-id "${APPLE_ID}" --password "${APP_PASSWORD}" --team-id "${TEAM_ID}" --wait
else
    echo_error "Notarization credentials not set (APPLE_API_KEY_PATH/ID/ISSUER, NOTARYTOOL_PROFILE, or APPLE_ID/APP_PASSWORD/TEAM_ID)"
    exit 1
fi
echo_success "Notarization completed"

xcrun stapler staple "${PRODUCT_PKG_PATH}"
echo_success "Stapling completed"

# ----------------------------------------------------------------------------
# Step 6: ZIP for compatibility / direct download distribution
# ----------------------------------------------------------------------------
echo_header "Step 6: Creating compatibility ZIP"

# ReadMe.txt + version.json + LICENSE 等のサイドカーをまとめて同梱する。
cat > "${OUTPUT_DIR}/ReadMe.txt" <<EOF
MixCompare ${VERSION} - macOS Installation Guide
====================================================

Installation Steps
-------------------
1. Close your DAW before proceeding.

2. For VST3 Plugin:
   Copy MixCompare.vst3 to:
   ~/Library/Audio/Plug-Ins/VST3/  (per-user)
   /Library/Audio/Plug-Ins/VST3/   (system-wide)

3. For Audio Unit (AU):
   Copy MixCompare.component to:
   ~/Library/Audio/Plug-Ins/Components/  (per-user)
   /Library/Audio/Plug-Ins/Components/   (system-wide)

4. For CLAP Plugin:
   Copy MixCompare.clap to:
   ~/Library/Audio/Plug-Ins/CLAP/  (per-user)
   /Library/Audio/Plug-Ins/CLAP/   (system-wide)

5. For Standalone Application:
   Copy MixCompare.app to /Applications/.
EOF
if [[ ${BUILD_AAX} -eq 1 ]]; then
    cat >> "${OUTPUT_DIR}/ReadMe.txt" <<EOF

6. For AAX Plugin (Pro Tools):
   Copy MixCompare.aaxplugin to:
   /Library/Application Support/Avid/Audio/Plug-Ins/

   Note: this build is ${AAX_PACE_STATUS}. AAX plug-ins must be PACE-signed
   to load in Pro Tools production builds; otherwise only Pro Tools Developer
   can host them.
EOF
fi

if [[ ${BUILD_AAX} -eq 1 ]]; then
    FORMATS='["VST3", "AU", "CLAP", "Standalone", "AAX"]'
    AAX_FIELD="\"${AAX_PACE_STATUS}\""
else
    FORMATS='["VST3", "AU", "CLAP", "Standalone"]'
    AAX_FIELD='"N/A"'
fi
cat > "${OUTPUT_DIR}/version.json" <<EOF
{
  "name": "MixCompare",
  "version": "${VERSION}",
  "build_date": "${BUILD_DATE}",
  "platform": "macOS",
  "architecture": "universal",
  "formats": ${FORMATS},
  "webui": "embedded",
  "build_type": "${CONFIGURATION}",
  "aax_signing": ${AAX_FIELD}
}
EOF

[[ -f "${ROOT_DIR}/LICENSE" ]]       && cp "${ROOT_DIR}/LICENSE"       "${OUTPUT_DIR}/LICENSE.txt"
[[ -f "${ROOT_DIR}/LICENSE.ja.md" ]] && cp "${ROOT_DIR}/LICENSE.ja.md" "${OUTPUT_DIR}/LICENSE.ja.txt"

if [[ ${BUILD_AAX} -eq 1 ]]; then
    ZIP_NAME="MixCompare_${VERSION}_macOS_VST3_AU_CLAP_AAX_Standalone.zip"
else
    ZIP_NAME="MixCompare_${VERSION}_macOS_VST3_AU_CLAP_Standalone.zip"
fi
ZIP_PATH="${OUTPUT_DIR}/../${ZIP_NAME}"

[[ -f "${ZIP_PATH}" ]] && rm -f "${ZIP_PATH}"
echo_step "Creating ZIP: ${ZIP_NAME}..."
(
    cd "${OUTPUT_DIR}"
    ZIP_ITEMS=(
        "$(basename "${DEST_VST3}")"
        "$(basename "${DEST_AU}")"
        "$(basename "${DEST_CLAP}")"
        "$(basename "${DEST_APP}")"
    )
    [[ ${BUILD_AAX} -eq 1 ]] && ZIP_ITEMS+=("$(basename "${DEST_AAX}")")
    ZIP_ITEMS+=(ReadMe.txt version.json LICENSE.txt)
    [[ -f LICENSE.ja.txt ]] && ZIP_ITEMS+=(LICENSE.ja.txt)
    /usr/bin/zip -r -y "${ZIP_PATH}" "${ZIP_ITEMS[@]}" >/dev/null
)
echo_success "ZIP: ${ZIP_PATH}"

# ----------------------------------------------------------------------------
# Final summary
# ----------------------------------------------------------------------------
PRODUCT_SIZE_MB=$(python3 -c 'import os,sys;print(round(os.path.getsize(sys.argv[1])/1024/1024,2))' "${PRODUCT_PKG_PATH}" 2>/dev/null || echo "-")
ZIP_SIZE_MB=$(python3 -c 'import os,sys;print(round(os.path.getsize(sys.argv[1])/1024/1024,2))' "${ZIP_PATH}" 2>/dev/null || echo "-")

echo_header "Build completed successfully!"
echo "PKG: ${PRODUCT_PKG_PATH} (${PRODUCT_SIZE_MB} MB)"
echo "ZIP: ${ZIP_PATH} (${ZIP_SIZE_MB} MB)"
echo ""
echo -e "${color_cyan}The package includes:${color_reset}"
echo -e "${color_green}[OK] VST3 (signed, hardened)${color_reset}"
echo -e "${color_green}[OK] AU (signed, hardened)${color_reset}"
echo -e "${color_green}[OK] CLAP (signed, hardened)${color_reset}"
echo -e "${color_green}[OK] Standalone (signed, hardened)${color_reset}"
if [[ ${BUILD_AAX} -eq 1 ]]; then
    case "${AAX_PACE_STATUS}" in
        signed) echo -e "${color_green}[OK] AAX (PACE-signed)${color_reset}" ;;
        *)      echo -e "${color_yellow}[!!] AAX (${AAX_PACE_STATUS})${color_reset}" ;;
    esac
fi
echo -e "${color_green}[OK] Installer signed (Developer ID Installer)${color_reset}"
echo -e "${color_green}[OK] Notarized + stapled (PKG)${color_reset}"
echo -e "${color_green}[OK] EULA in en/ja shown by installer based on locale${color_reset}"

exit 0
