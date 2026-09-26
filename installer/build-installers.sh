#!/usr/bin/env bash
set -eu

die() { printf 'build-installers: %s\n' "$*" >&2; exit 1; }

if [ $# -ne 2 ]; then
    printf 'Usage: %s <path-to-mod.geode> <output-dir>\n' "$(basename "$0")" >&2
    exit 2
fi

GEODE_FILE=$1
OUT_DIR=$2
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
WIN_TEMPLATE="$HERE/windows-template.bat"
UNIX_TEMPLATE="$HERE/unix-template.sh"
WIN_NAME='RhythmPath-Installer-Windows.bat'
UNIX_NAME='RhythmPath-Installer-macOS-Linux.sh'
DEFAULT_ID='mmdas28.rhythm-path'
MARK_BEGIN='#RPDATA#'
MARK_END='#RPEND#'

[ -f "$GEODE_FILE" ] || die "not a file: $GEODE_FILE"
[ -s "$GEODE_FILE" ] || die "empty file: $GEODE_FILE"
[ -f "$WIN_TEMPLATE" ] || die "missing template: $WIN_TEMPLATE"
[ -f "$UNIX_TEMPLATE" ] || die "missing template: $UNIX_TEMPLATE"

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print tolower($1)}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print tolower($1)}'
    else
        openssl dgst -sha256 -r "$1" | awk '{print tolower($1)}'
    fi
}

b64decode() {
    if printf 'QQ==' | base64 --decode >/dev/null 2>&1; then base64 --decode; else base64 -D; fi
}

json_field() {
    local file=$1 key=$2
    if command -v python3 >/dev/null 2>&1; then
        python3 - "$file" "$key" <<'PY' 2>/dev/null && return 0
import json, sys
value = json.load(open(sys.argv[1], encoding="utf-8")).get(sys.argv[2], "")
print(value if isinstance(value, str) else "")
PY
    fi
    if command -v jq >/dev/null 2>&1; then
        jq -r --arg k "$key" '.[$k] // "" | strings' "$file" 2>/dev/null && return 0
    fi
    tr -d '\r\n' <"$file" | grep -o "\"$key\"[[:space:]]*:[[:space:]]*\"[^\"]*\"" | head -n 1 | sed 's/.*:[[:space:]]*"\(.*\)"$/\1/'
}

check_template() {
    local file=$1
    tr -d '\r' <"$file" | grep -qx -e "$MARK_BEGIN" -e "$MARK_END" && die "$file must not contain the payload marker lines"
    if LC_ALL=C grep -q '[^[:print:][:space:]]' "$file"; then die "$file must be plain ASCII"; fi
    return 0
}

strip_cr() { awk '{ sub(/\r$/, ""); print }'; }
to_crlf() { awk '{ sub(/\r$/, ""); printf "%s\r\n", $0 }'; }

check_template "$WIN_TEMPLATE"
check_template "$UNIX_TEMPLATE"
tr -d '\r' <"$WIN_TEMPLATE" | grep -qx '#RPPS#' || die "$WIN_TEMPLATE is missing the #RPPS# marker line"
tr -d '\r' <"$WIN_TEMPLATE" | head -n 1 | grep -qx '@echo off' || die "$WIN_TEMPLATE must start with @echo off"
bash -n "$UNIX_TEMPLATE" || die "$UNIX_TEMPLATE has a syntax error"

WORK=$(mktemp -d 2>/dev/null || mktemp -d -t rpbuild)
trap 'rm -rf "$WORK"' EXIT

MOD_JSON=''
if command -v unzip >/dev/null 2>&1 && unzip -p "$GEODE_FILE" mod.json >"$WORK/mod.json" 2>/dev/null && [ -s "$WORK/mod.json" ]; then
    MOD_JSON="$WORK/mod.json"
elif [ -f "$HERE/../mod.json" ]; then
    MOD_JSON="$HERE/../mod.json"
fi

MOD_ID=''
VERSION=''
GEODE_VER=''
if [ -n "$MOD_JSON" ]; then
    MOD_ID=$(json_field "$MOD_JSON" id || true)
    VERSION=$(json_field "$MOD_JSON" version || true)
    GEODE_VER=$(json_field "$MOD_JSON" geode || true)
fi
printf '%s' "$MOD_ID" | grep -Eq '^[a-z0-9_-]+\.[a-z0-9_.-]+$' || MOD_ID=$DEFAULT_ID
VERSION=$(printf '%s' "$VERSION" | tr -cd 'A-Za-z0-9._+-')
GEODE_VER=$(printf '%s' "$GEODE_VER" | tr -cd 'A-Za-z0-9._+-')
SHA=$(sha256_of "$GEODE_FILE")
SIZE=$(wc -c <"$GEODE_FILE" | tr -d ' ')

DATA="$WORK/data"
{
    printf '%s\n' "$MARK_BEGIN"
    printf '#id=%s\n' "$MOD_ID"
    printf '#version=%s\n' "$VERSION"
    printf '#geode=%s\n' "$GEODE_VER"
    printf '#size=%s\n' "$SIZE"
    printf '#sha256=%s\n' "$SHA"
    base64 <"$GEODE_FILE" | tr -d '\r\n' | fold -w 76
    printf '\n%s\n' "$MARK_END"
} >"$DATA"

with_newline() {
    cat "$1"
    if [ -n "$(tail -c 1 "$1")" ]; then printf '\n'; fi
}

mkdir -p "$OUT_DIR"
WIN_OUT="$OUT_DIR/$WIN_NAME"
UNIX_OUT="$OUT_DIR/$UNIX_NAME"
GEODE_OUT="$OUT_DIR/$MOD_ID.geode"

{ with_newline "$WIN_TEMPLATE"; cat "$DATA"; } | to_crlf >"$WORK/win"
{ with_newline "$UNIX_TEMPLATE"; cat "$DATA"; } | strip_cr >"$WORK/unix"

for built in "$WORK/win" "$WORK/unix"; do
    decoded=$(tr -d '\r' <"$built" | awk -v b="$MARK_BEGIN" -v e="$MARK_END" '$0 == b { f = 1; next } $0 == e { exit } f && substr($0, 1, 1) != "#" { print }' | b64decode | { cat >"$WORK/check.geode"; sha256_of "$WORK/check.geode"; })
    [ "$decoded" = "$SHA" ] || die "self-check failed: payload in $built does not decode to the input file"
done
[ "$(tr -d '\r' <"$WORK/win" | grep -cx '#RPPS#')" -eq 1 ] || die 'self-check failed: #RPPS# marker must appear exactly once'
if tr -d '\r\n' <"$WORK/win" | LC_ALL=C grep -q '[^[:print:][:space:]]'; then die 'self-check failed: Windows installer is not plain ASCII'; fi
bash -n "$WORK/unix" || die 'self-check failed: macOS/Linux installer has a syntax error'

mv -f "$WORK/win" "$WIN_OUT"
mv -f "$WORK/unix" "$UNIX_OUT"
chmod 755 "$UNIX_OUT"
chmod 644 "$WIN_OUT"
if [ ! "$GEODE_FILE" -ef "$GEODE_OUT" ]; then cp -f "$GEODE_FILE" "$GEODE_OUT"; fi
chmod 644 "$GEODE_OUT"

printf 'Mod:      %s %s (Geode %s)\n' "$MOD_ID" "${VERSION:-unknown}" "${GEODE_VER:-default}"
printf 'SHA-256:  %s\n' "$SHA"
printf 'Created:  %s\n' "$WIN_OUT" "$UNIX_OUT" "$GEODE_OUT"
