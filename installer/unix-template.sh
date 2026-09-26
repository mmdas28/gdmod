#!/usr/bin/env bash
if [ -z "${BASH_VERSION:-}" ]; then exec bash "$0" "$@"; fi

APP_ID=322170
DEFAULT_MOD_ID='mmdas28.rhythm-path'
SELF=${BASH_SOURCE[0]}
MARK_BEGIN='#RP''DATA#'
MARK_END='#RP''END#'
NL='
'
TMP_DIR=''
OS=''
GD=''
MODS=''
ARG_PATH=''

if [ -t 1 ]; then
    C_RED=$(printf '\033[31m'); C_GREEN=$(printf '\033[32m'); C_YELLOW=$(printf '\033[33m')
    C_CYAN=$(printf '\033[36m'); C_BOLD=$(printf '\033[1m'); C_RESET=$(printf '\033[0m')
else
    C_RED=''; C_GREEN=''; C_YELLOW=''; C_CYAN=''; C_BOLD=''; C_RESET=''
fi

step() { printf '\n%s==> %s%s\n' "$C_CYAN" "$*" "$C_RESET"; }
ok() { printf '    %s%s%s\n' "$C_GREEN" "$*" "$C_RESET"; }
info() { printf '    %s\n' "$*"; }
warn() { printf '    %s%s%s\n' "$C_YELLOW" "$*" "$C_RESET"; }
die() {
    printf '\n  %sInstallation failed.%s\n' "$C_RED" "$C_RESET" >&2
    while [ $# -gt 0 ]; do printf '  %s\n' "$1" >&2; shift; done
    printf '\n' >&2
    exit 1
}

trap 'if [ -n "$TMP_DIR" ] && [ -d "$TMP_DIR" ]; then rm -rf "$TMP_DIR"; fi' EXIT
trap 'exit 130' INT TERM

interactive() { [ -t 0 ] && [ -t 1 ]; }

ask() {
    local answer
    printf '    %s ' "$1" >&2
    IFS= read -r answer || answer=''
    printf '%s' "$answer"
}

confirm() {
    local question=$1 default=$2 suffix='[y/N]' answer
    [ "$default" = y ] && suffix='[Y/n]'
    if ! interactive; then [ "$default" = y ]; return; fi
    while :; do
        answer=$(ask "$question $suffix")
        case "$answer" in
            '') [ "$default" = y ]; return ;;
            [Yy]|[Yy][Ee][Ss]) return 0 ;;
            [Nn]|[Nn][Oo]) return 1 ;;
        esac
    done
}

usage() {
    cat <<EOF
Rhythm Path installer for macOS and Linux (Steam + Proton).

Usage: bash $(basename "$SELF") [PATH]

PATH is optional. Use it if Geometry Dash is not found automatically:
  Linux: the Geometry Dash folder (it contains GeometryDash.exe)
  macOS: Geometry Dash.app (or the folder that contains it)

Geode must already be installed: https://geode-sdk.org/install
EOF
}

meta() {
    awk -v b="$MARK_BEGIN" -v e="$MARK_END" -v k="#$1=" '
        { sub(/\r$/, "") }
        $0 == b { f = 1; next }
        $0 == e { exit }
        f && index($0, k) == 1 { print substr($0, length(k) + 1); exit }
    ' "$SELF"
}

payload_b64() {
    awk -v b="$MARK_BEGIN" -v e="$MARK_END" '
        { sub(/\r$/, "") }
        $0 == b { f = 1; next }
        $0 == e { exit }
        f && length($0) > 0 && substr($0, 1, 1) != "#" { print }
    ' "$SELF"
}

b64decode() {
    if printf 'QQ==' | base64 --decode >/dev/null 2>&1; then
        base64 --decode
    elif printf 'QQ==' | base64 -D >/dev/null 2>&1; then
        base64 -D
    else
        openssl base64 -d
    fi
}

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print tolower($1)}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print tolower($1)}'
    else
        openssl dgst -sha256 -r "$1" | awk '{print tolower($1)}'
    fi
}

canon() { (cd "$1" 2>/dev/null && pwd -P); }

steam_roots() {
    local h=$HOME
    if [ "$OS" = mac ]; then
        printf '%s\n' "$h/Library/Application Support/Steam"
        return
    fi
    printf '%s\n' \
        "${XDG_DATA_HOME:-$h/.local/share}/Steam" \
        "$h/.local/share/Steam" \
        "$h/.steam/steam" \
        "$h/.steam/root" \
        "$h/.steam/debian-installation" \
        "$h/Steam" \
        "$h/.var/app/com.valvesoftware.Steam/.local/share/Steam" \
        "$h/.var/app/com.valvesoftware.Steam/data/Steam" \
        "$h/.var/app/com.valvesoftware.Steam/.steam/steam" \
        "$h/.var/app/com.valvesoftware.Steam/.steam/root" \
        "$h/snap/steam/common/.local/share/Steam" \
        "$h/snap/steam/common/.steam/steam" \
        "$h/snap/steam/common/.steam/root"
}

vdf_paths() {
    tr -d '\r' <"$1" | sed -n \
        -e 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' \
        -e 's/^[[:space:]]*"[0-9][0-9]*"[[:space:]]*"\(\/.*\)"[[:space:]]*$/\1/p' |
        sed 's/\\\\/\\/g'
}

steam_libraries() {
    local root vdf
    steam_roots | while IFS= read -r root; do
        [ -d "$root/steamapps" ] || continue
        printf '%s\n' "$root"
        for vdf in "$root/steamapps/libraryfolders.vdf" "$root/config/libraryfolders.vdf"; do
            if [ -f "$vdf" ]; then vdf_paths "$vdf"; fi
        done
    done
}

is_gd() {
    if [ "$OS" = mac ]; then
        [ -f "$1/Contents/MacOS/Geometry Dash" ]
    else
        [ -f "$1/libcocos2d.dll" ]
    fi
}

unique_gd() {
    local seen="$NL" d c
    while IFS= read -r d; do
        [ -n "$d" ] || continue
        is_gd "$d" || continue
        c=$(canon "$d") || continue
        [ -n "$c" ] || continue
        case "$seen" in *"$NL$c$NL"*) continue ;; esac
        seen="$seen$c$NL"
        printf '%s\n' "$c"
    done
}

candidates() {
    local lib
    if [ "$OS" = mac ]; then
        steam_libraries | while IFS= read -r lib; do
            printf '%s\n' "$lib/steamapps/common/Geometry Dash/Geometry Dash.app"
        done
        printf '%s\n' "/Applications/Geometry Dash.app" "$HOME/Applications/Geometry Dash.app"
        if command -v mdfind >/dev/null 2>&1; then
            mdfind "kMDItemCFBundleIdentifier == 'com.robtop.geometrydashmac'" 2>/dev/null
        fi
    else
        steam_libraries | while IFS= read -r lib; do
            printf '%s\n' "$lib/steamapps/common/Geometry Dash"
        done
        printf '%s\n' "$HOME/Games/Geometry Dash"
    fi
}

resolve_path() {
    local p=$1 i up
    case "$p" in \~/*) p="$HOME/${p#\~/}" ;; esac
    [ -e "$p" ] || return 1
    [ -f "$p" ] && p=$(dirname "$p")
    {
        printf '%s\n' "$p" "$p/Geometry Dash" "$p/Geometry Dash.app" \
            "$p/Geometry Dash/Geometry Dash.app" "$p/steamapps/common/Geometry Dash" \
            "$p/steamapps/common/Geometry Dash/Geometry Dash.app"
        up=$p
        i=0
        while [ $i -lt 4 ]; do
            up=$(dirname "$up")
            printf '%s\n' "$up"
            i=$((i + 1))
        done
    } | unique_gd | head -n 1
}

choose_gd() {
    local found count i line pick
    if [ -n "$ARG_PATH" ]; then
        GD=$(resolve_path "$ARG_PATH")
        [ -n "$GD" ] || die "This is not a Geometry Dash install: $ARG_PATH" \
            "$( [ "$OS" = mac ] && echo 'Pass the path to Geometry Dash.app.' || echo 'Pass the folder that contains GeometryDash.exe and libcocos2d.dll.')"
        return
    fi
    step 'Looking for Geometry Dash'
    found=$(candidates | unique_gd)
    count=$(printf '%s' "$found" | grep -c '^' || true)
    if [ "$count" -eq 0 ]; then
        if [ "$OS" = mac ]; then
            die 'Could not find Geometry Dash.app.' \
                'Run the installer again with its location, for example:' \
                "  bash \"$SELF\" \"\$HOME/Library/Application Support/Steam/steamapps/common/Geometry Dash/Geometry Dash.app\""
        else
            die 'Could not find Geometry Dash.' \
                'In Steam: right-click Geometry Dash > Manage > Browse local files to see its folder,' \
                'then run the installer again with that folder, for example:' \
                "  bash \"$SELF\" \"\$HOME/.local/share/Steam/steamapps/common/Geometry Dash\""
        fi
    fi
    if [ "$count" -eq 1 ] || ! interactive; then
        GD=$(printf '%s\n' "$found" | head -n 1)
        return
    fi
    info 'Found more than one Geometry Dash install:'
    i=0
    while IFS= read -r line; do
        i=$((i + 1))
        info "  [$i] $line"
    done <<EOF
$found
EOF
    while :; do
        pick=$(ask 'Which one? (press Enter for 1)')
        [ -z "$pick" ] && pick=1
        case "$pick" in
            *[!0-9]*) continue ;;
        esac
        if [ "$pick" -ge 1 ] && [ "$pick" -le "$count" ]; then
            GD=$(printf '%s\n' "$found" | sed -n "${pick}p")
            return
        fi
    done
}

geode_installed() {
    if [ "$OS" = mac ]; then
        [ -f "$GD/Contents/Frameworks/Geode.dylib" ]
    else
        [ -f "$GD/Geode.dll" ]
    fi
}

geode_missing() {
    local ver
    ver=$(meta geode)
    printf '\n  %sGeode is not installed%s in: %s\n\n' "$C_RED" "$C_RESET" "$GD" >&2
    printf '  Rhythm Path is a Geode mod, so Geode (the mod loader) has to be installed first.\n' >&2
    printf '  Full guide: https://geode-sdk.org/install\n\n' >&2
    if [ "$OS" = mac ]; then
        printf '  1. Download geode-installer-v%s-mac.pkg (or newer) from\n' "${ver:-X.Y.Z}" >&2
        printf '     https://github.com/geode-sdk/geode/releases/latest\n' >&2
        printf '  2. Open the .pkg and follow the steps (right-click > Open if macOS blocks it).\n' >&2
        printf '  3. Run this installer again.\n\n' >&2
    else
        printf '  1. Download geode-installer-v%s-linux.sh (or newer) from\n' "${ver:-X.Y.Z}" >&2
        printf '     https://github.com/geode-sdk/geode/releases/latest\n' >&2
        printf '     and run it:  bash geode-installer-*-linux.sh\n' >&2
        printf '  2. In Steam, open Geometry Dash > Properties > General > Launch Options and enter:\n' >&2
        printf '       WINEDLLOVERRIDES="xinput1_4=n,b" %%command%%\n' >&2
        printf '  3. Run this installer again.\n\n' >&2
    fi
    exit 1
}

gd_running() {
    command -v pgrep >/dev/null 2>&1 || return 1
    if [ "$OS" = mac ]; then
        pgrep -x 'Geometry Dash' >/dev/null 2>&1
    else
        pgrep 'GeometryDash' >/dev/null 2>&1
    fi
}

wait_gd_closed() {
    gd_running || return 0
    warn 'Geometry Dash is running. Mods only load when the game starts.'
    if ! interactive; then
        warn 'Restart Geometry Dash after the installer finishes.'
        return 0
    fi
    while gd_running; do
        ask 'Close Geometry Dash, then press Enter to continue...' >/dev/null
    done
}

check_launch_options() {
    local root cfg seen=0 hit=0
    [ "$OS" = linux ] || return 0
    while IFS= read -r root; do
        for cfg in "$root"/userdata/*/config/localconfig.vdf; do
            [ -f "$cfg" ] || continue
            seen=1
            if grep -qi 'xinput1_4' "$cfg" 2>/dev/null; then hit=1; fi
        done
    done <<EOF
$(steam_roots)
EOF
    if [ "$seen" -eq 1 ] && [ "$hit" -eq 0 ]; then
        printf '\n'
        warn 'Geode only loads under Proton if Geometry Dash has this Steam launch option:'
        warn '  WINEDLLOVERRIDES="xinput1_4=n,b" %command%'
        warn 'Set it in Steam: Geometry Dash > Properties > General > Launch Options.'
    fi
}

write_failed() {
    if [ "$OS" = mac ]; then
        die "macOS did not allow writing to: $MODS" \
            'Open System Settings > Privacy & Security > App Management and turn on Terminal' \
            '(or the app you ran this from), then run the installer again.' \
            'Or copy the .geode file by hand: right-click Geometry Dash.app > Show Package Contents >' \
            'Contents > geode > mods.'
    else
        die "Could not write to: $MODS" \
            'Check that the folder exists and that you own it, then run the installer again.'
    fi
}

install_mod() {
    local final tmp f base id_re
    final="$MODS/$MOD_ID.geode"
    tmp="$MODS/.$MOD_ID.geode.rp-new"
    mkdir -p "$MODS" 2>/dev/null || write_failed
    [ -w "$MODS" ] || write_failed
    id_re=$(printf '%s' "$MOD_ID" | sed 's/[].[^$*+?(){}|\\]/\\&/g')
    for f in "$MODS/$MOD_ID"*.geode; do
        [ -f "$f" ] || continue
        base=${f##*/}
        if printf '%s\n' "$base" | grep -Eq "^${id_re}( ?\([0-9]+\)|-[0-9]+)\.geode$"; then
            rm -f "$f" && info "Removed an old duplicate: $base"
        fi
    done
    cp "$PAYLOAD_FILE" "$tmp" 2>/dev/null || { rm -f "$tmp" 2>/dev/null; write_failed; }
    if [ "$(sha256_of "$tmp")" != "$SHA" ]; then
        rm -f "$tmp"
        die 'The mod file could not be written correctly (checksum mismatch). Check your disk and try again.'
    fi
    mv -f "$tmp" "$final" 2>/dev/null || { rm -f "$tmp" 2>/dev/null; write_failed; }
    [ "$(sha256_of "$final")" = "$SHA" ] || die "The mod file at $final does not match the expected checksum. Run the installer again."
    ok "Installed and verified: $final"
}

launch_gd() {
    local url="steam://rungameid/$APP_ID"
    if [ "$OS" = mac ]; then
        open "$url" >/dev/null 2>&1 && { ok 'Starting Geometry Dash through Steam...'; return; }
    else
        if command -v xdg-open >/dev/null 2>&1; then
            (xdg-open "$url" >/dev/null 2>&1 &) && { ok 'Starting Geometry Dash through Steam...'; return; }
        fi
    fi
    warn 'Could not start Geometry Dash automatically. Start it from Steam.'
}

main() {
    local version title
    case "${1:-}" in
        -h|--help) usage; exit 0 ;;
        '') ;;
        *) ARG_PATH=$1 ;;
    esac

    case "$(uname -s)" in
        Darwin) OS=mac ;;
        Linux) OS=linux ;;
        *) die "Unsupported system: $(uname -s). On Windows, use RhythmPath-Installer-Windows.bat." ;;
    esac

    printf '\n  %sRhythm Path installer%s\n  ---------------------\n' "$C_BOLD" "$C_RESET"

    [ -f "$SELF" ] || die 'Could not read the installer file. Run it as: bash RhythmPath-Installer-macOS-Linux.sh'
    MOD_ID=$(meta id)
    printf '%s' "$MOD_ID" | grep -Eq '^[a-z0-9_-]+\.[a-z0-9_.-]+$' || MOD_ID=$DEFAULT_MOD_ID
    SHA=$(meta sha256 | tr 'A-F' 'a-f')
    version=$(meta version)
    title='Rhythm Path'
    [ -n "$version" ] && title="Rhythm Path $version"
    info "Mod: $title ($MOD_ID)"

    TMP_DIR=$(mktemp -d 2>/dev/null || mktemp -d -t rhythmpath) || die 'Could not create a temporary folder.'
    PAYLOAD_FILE="$TMP_DIR/$MOD_ID.geode"
    payload_b64 | b64decode >"$PAYLOAD_FILE" 2>/dev/null
    if [ -z "$SHA" ] || [ ! -s "$PAYLOAD_FILE" ] || [ "$(sha256_of "$PAYLOAD_FILE")" != "$SHA" ]; then
        die 'The mod inside this installer is damaged (checksum mismatch). Download the installer again.'
    fi

    choose_gd
    ok "Geometry Dash: $GD"
    if [ "$OS" = mac ]; then MODS="$GD/Contents/geode/mods"; else MODS="$GD/geode/mods"; fi

    step 'Checking Geode'
    geode_installed || geode_missing
    ok 'Geode is installed.'

    wait_gd_closed

    step "Installing $title"
    install_mod

    check_launch_options

    printf '\n  %sDone! %s is installed.%s\n\n' "$C_GREEN" "$title" "$C_RESET"
    if interactive && confirm 'Start Geometry Dash now?' y; then launch_gd; fi
    return 0
}

main "$@"
exit $?
