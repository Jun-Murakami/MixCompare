#!/usr/bin/env bash
# MixCompare Version Update Script
# Updates version across all project files

set -euo pipefail

# --- ANSI colors (Write-Host -ForegroundColor 相当) ---
C_CYAN='\033[36m'
C_YELLOW='\033[33m'
C_GREEN='\033[32m'
C_RED='\033[31m'
C_GRAY='\033[90m'
C_WHITE='\033[97m'
C_RESET='\033[0m'

# --- Parse argument ($NewVersion, Mandatory) ---
if [[ $# -lt 1 || -z "${1:-}" ]]; then
    echo -e "${C_RED}Error: NewVersion is required.${C_RESET}"
    echo -e "${C_GRAY}Usage: ./update_version.sh X.Y.Z[-suffix]${C_RESET}"
    exit 1
fi
NewVersion="$1"

# --- Validate version format ---
if [[ ! "$NewVersion" =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[a-zA-Z0-9_]+)?$ ]]; then
    echo -e "${C_RED}Error: Invalid version format. Use X.Y.Z or X.Y.Z-suffix${C_RESET}"
    echo -e "${C_GRAY}Example: 3.0.0 or 3.0.0-beta1${C_RESET}"
    exit 1
fi

# --- Get script directory as root ($PSScriptRoot 相当) ---
ScriptDir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RootDir="$ScriptDir"

echo -e "${C_CYAN}============================================${C_RESET}"
echo -e "${C_CYAN}   Updating MixCompare version to $NewVersion${C_RESET}"
echo -e "${C_CYAN}============================================${C_RESET}"
echo ""

# 1. Update VERSION file
echo -e "${C_YELLOW}► Updating VERSION file...${C_RESET}"
printf '%s' "$NewVersion" > "$RootDir/VERSION"
echo -e "${C_GREEN}✓ VERSION file updated${C_RESET}"

# 2. Update package.json
echo -e "${C_YELLOW}► Updating package.json...${C_RESET}"
PackageJsonPath="$RootDir/webui/package.json"
if [[ -f "$PackageJsonPath" ]]; then
    # Extract base version without suffix for package.json
    BaseVersion="${NewVersion%%-*}"

    # Use regex to update version field
    perl -0pi -e 's/"version":\s*"[^"]+"/"version": "'"$BaseVersion"'"/' "$PackageJsonPath"
    echo -e "${C_GREEN}✓ package.json updated to $BaseVersion${C_RESET}"
else
    echo -e "${C_YELLOW}⚠ package.json not found${C_RESET}"
fi

# 3. Update PluginEditor.cpp
echo -e "${C_YELLOW}► Updating PluginEditor.cpp...${C_RESET}"
PluginEditorPath="$RootDir/plugin/src/PluginEditor.cpp"
if [[ -f "$PluginEditorPath" ]]; then
    perl -0pi -e 's/\.withInitialisationData\("pluginVersion", "[^"]+"\)/.withInitialisationData("pluginVersion", "'"$NewVersion"'")/' "$PluginEditorPath"
    echo -e "${C_GREEN}✓ PluginEditor.cpp updated${C_RESET}"
else
    echo -e "${C_YELLOW}⚠ PluginEditor.cpp not found${C_RESET}"
fi

# 4. Update build scripts
ScriptsToUpdate=(
    "$RootDir/scripts/build_windows_release.ps1"
    "$RootDir/scripts/build_complete_release.bat"
    "$RootDir/scripts/package_release.ps1"
)

for Script in "${ScriptsToUpdate[@]}"; do
    if [[ -f "$Script" ]]; then
        ScriptName="$(basename "$Script")"
        echo -e "${C_YELLOW}► Updating $ScriptName...${C_RESET}"

        # Update version patterns
        perl -0pi -e '
            s/Version\s*=\s*"[^"]+"\s*,/Version = "'"$NewVersion"'",/g;
            s/\$Version\s*=\s*"[^"]+"/\$Version = "'"$NewVersion"'"/g;
            s/set VERSION=[^\r\n]+/set VERSION='"$NewVersion"'/g;
        ' "$Script"
        echo -e "${C_GREEN}✓ $ScriptName updated${C_RESET}"
    fi
done

echo ""
echo -e "${C_GREEN}============================================${C_RESET}"
echo -e "${C_GREEN}   Version updated successfully!${C_RESET}"
echo -e "${C_GREEN}============================================${C_RESET}"
echo ""
echo -e "${C_WHITE}New version: $NewVersion${C_RESET}"
echo ""
echo -e "${C_CYAN}Next steps:${C_RESET}"
echo -e "${C_GRAY}1. Rebuild CMake configuration:${C_RESET}"
echo -e "${C_GRAY}   cd build && cmake ..${C_RESET}"
echo -e "${C_GRAY}2. Rebuild the project${C_RESET}"
echo -e "${C_GRAY}3. Commit the version change:${C_RESET}"
echo -e "${C_GRAY}   git add -A && git commit -m \"Bump version to $NewVersion\"${C_RESET}"
echo -e "${C_GRAY}4. Create a git tag:${C_RESET}"
echo -e "${C_GRAY}   git tag v$NewVersion${C_RESET}"
echo ""
