#!/bin/sh
# Installs luwu-lsp and sets up your editors on macOS, Linux and Android (Termux):
#
#     curl -fsSL https://raw.githubusercontent.com/luwu-community/luwu-lsp/luwu/scripts/install/install.sh | sh
#
# Everything the installer does is in install.luau, a seal script. This only finds a seal to run it with: the one on
# your PATH when it's newer than 0.8.0, or else the latest release, downloaded into a temporary directory that's deleted
# afterwards. Arguments go to install.luau (`... | sh -s -- --help` lists them; `--keep-seal` keeps the downloaded seal).
set -eu

raw="https://raw.githubusercontent.com/luwu-community/luwu-lsp/luwu/scripts/install"

fail() {
    echo "luwu-lsp installer: $1" >&2
    exit 1
}

command -v curl >/dev/null || fail "curl is required"

# The name seal's releases give this platform
system="$(uname -s)"
machine="$(uname -m)"
if [ "$system" = "Linux" ] && [ "$(uname -o 2>/dev/null)" = "Android" ]; then
    system="Android"
fi
case "$system-$machine" in
    Darwin-arm64) platform="macos-darwin-arm64" ;;
    Darwin-x86_64) fail "Intel Macs aren't supported, since Apple no longer supports them either" ;;
    Linux-x86_64) platform="linux-x64" ;;
    Linux-aarch64 | Linux-arm64) platform="linux-arm64" ;;
    Android-aarch64) platform="android-arm64" ;;
    Android-armv7l | Android-armv8l) platform="android-armv7" ;;
    *) fail "no luwu-lsp build for $system on $machine" ;;
esac

# Whether `$1 --version` is newer than 0.8.0
seal_is_new_enough() {
    version="$("$1" --version 2>/dev/null | grep -Eo '[0-9]+\.[0-9]+\.[0-9]+' | head -n 1)" || return 1
    [ -n "$version" ] || return 1
    echo "$version" | awk -F. '{ exit !($1 > 0 || $2 > 8 || ($2 == 8 && $3 > 0)) }'
}

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

seal=""
if command -v seal >/dev/null && seal_is_new_enough "$(command -v seal)"; then
    seal="$(command -v seal)"
else
    echo "Downloading seal to run the installer..."
    url="$(curl -fsSL https://api.github.com/repos/seal-runtime/seal/releases/latest |
        grep -o "https://[^\"]*-$platform[^\"]*\.tar\.gz" | head -n 1)"
    [ -n "$url" ] || fail "couldn't find a seal release for $platform"
    curl -fsSL "$url" | tar xz -C "$tmp" seal
    seal="$tmp/seal"
    chmod +x "$seal"
    # Lets the installer offer to keep it (`--keep-seal`)
    export LUWU_INSTALL_TEMPORARY_SEAL=1
fi

curl -fsSL "$raw/install.luau" -o "$tmp/install.luau"

# Run with the terminal as input even when this script came through a pipe (`curl | sh`), so the installer can ask its
# questions. Without a terminal, the installer only does what its arguments say.
status=0
if [ -t 0 ]; then
    "$seal" "$tmp/install.luau" "$@" || status=$?
elif (: </dev/tty) 2>/dev/null; then
    "$seal" "$tmp/install.luau" "$@" </dev/tty || status=$?
else
    "$seal" "$tmp/install.luau" "$@" || status=$?
fi
exit "$status"
