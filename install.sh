#!/bin/sh
# Euros terminal client installer for Linux and macOS.
#
#   curl -fsSL https://raw.githubusercontent.com/KazHakiStan/euros-cli/master/install.sh | sh
#   ... | sh -s -- --uninstall
#
# EUROS_VERSION=v0.2.0      a specific release instead of the latest
# EUROS_INSTALL_DIR=<dir>   where to put the binary (default ~/.local/bin)
# EUROS_REPO=owner/name     another GitHub repository
# EUROS_NO_MODIFY_PATH=1    don't touch shell startup files
# EUROS_DOWNLOAD_BASE=<url> where the archives and SHA256SUMS are (a mirror)
set -eu

REPO="${EUROS_REPO:-KazHakiStan/euros-cli}"
VERSION="${EUROS_VERSION:-}"
DIR="${EUROS_INSTALL_DIR:-$HOME/.local/bin}"
MARK="# added by the euros installer"

say() { printf '%s\n' "$*"; }
die() { printf 'euros install: %s\n' "$*" >&2; exit 1; }

printf '%s' "$REPO" | grep -Eq '^[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$' || die "bad EUROS_REPO: $REPO"
if [ -n "$VERSION" ]; then
    printf '%s' "$VERSION" | grep -Eq '^v[0-9]+(\.[0-9]+){0,2}(-[A-Za-z0-9.]+)?$' \
        || die "bad EUROS_VERSION: $VERSION (expected like v0.2.0)"
fi

download() {
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 2 -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$2" "$1"
    else
        die "needs curl or wget"
    fi
}

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        die "needs sha256sum or shasum to check the download"
    fi
}

asset_name() {
    os=$(uname -s)
    arch=$(uname -m)
    case "$os/$arch" in
        Linux/x86_64|Linux/amd64) echo "euros-linux-x86_64.tar.gz" ;;
        Darwin/arm64) echo "euros-macos-arm64.tar.gz" ;;
        *) die "no ready-made build for $os/$arch yet. Build from source: https://github.com/$REPO#build" ;;
    esac
}

# Startup files of the shells in use, so new terminals find euros on PATH.
profile_files() {
    shell_name=$(basename "${SHELL:-sh}")
    case "$shell_name" in
        zsh) echo "${ZDOTDIR:-$HOME}/.zshrc" ;;
        bash)
            if [ "$(uname -s)" = Darwin ]; then
                echo "$HOME/.bash_profile"
            else
                echo "$HOME/.bashrc"
            fi
            ;;
        fish) echo "${XDG_CONFIG_HOME:-$HOME/.config}/fish/conf.d/euros.fish" ;;
        *) echo "$HOME/.profile" ;;
    esac
}

add_to_path() {
    case ":$PATH:" in
        *":$DIR:"*) return 0 ;;
    esac
    if [ -n "${EUROS_NO_MODIFY_PATH:-}" ]; then
        say "Add $DIR to your PATH to run euros."
        return 0
    fi
    for file in $(profile_files); do
        if [ -f "$file" ] && grep -qF "$MARK" "$file"; then
            continue
        fi
        mkdir -p "$(dirname "$file")"
        case "$file" in
            *.fish) printf '\n%s\nfish_add_path "%s"\n' "$MARK" "$DIR" >>"$file" ;;
            *) printf '\n%s\nexport PATH="%s:$PATH"\n' "$MARK" "$DIR" >>"$file" ;;
        esac
        say "Added $DIR to PATH in $file"
    done
    NEED_NEW_SHELL=1
}

remove_from_path() {
    for file in "$HOME/.zshrc" "${ZDOTDIR:-$HOME}/.zshrc" "$HOME/.bashrc" "$HOME/.bash_profile" "$HOME/.profile"; do
        if [ -f "$file" ] && grep -qF "$MARK" "$file"; then
            # The marker line and the line after it.
            awk -v mark="$MARK" '$0 == mark { skip = 1; next } skip == 1 { skip = 0; next } { print }' "$file" >"$file.euros-tmp"
            cat "$file.euros-tmp" >"$file"
            rm -f "$file.euros-tmp"
            say "Removed the PATH line from $file"
        fi
    done
    fish_file="${XDG_CONFIG_HOME:-$HOME/.config}/fish/conf.d/euros.fish"
    if [ -f "$fish_file" ]; then
        rm -f "$fish_file"
        say "Removed $fish_file"
    fi
}

uninstall() {
    rm -f "$DIR/euros"
    remove_from_path
    say "euros is uninstalled. Your session stays in ~/.config/euros (run \`euros logout\` first to remove it)."
}

install() {
    asset=$(asset_name)
    if [ -n "${EUROS_DOWNLOAD_BASE:-}" ]; then
        base="$EUROS_DOWNLOAD_BASE"
    elif [ -n "$VERSION" ]; then
        base="https://github.com/$REPO/releases/download/$VERSION"
    else
        base="https://github.com/$REPO/releases/latest/download"
    fi

    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT INT TERM
    say "Downloading $asset ${VERSION:-(latest)}..."
    download "$base/$asset" "$tmp/$asset" || die "download failed: $base/$asset"
    download "$base/SHA256SUMS" "$tmp/SHA256SUMS" || die "download failed: $base/SHA256SUMS"

    expected=$(awk -v f="$asset" '$2 == f || $2 == "*" f { print $1 }' "$tmp/SHA256SUMS")
    actual=$(sha256_of "$tmp/$asset")
    [ -n "$expected" ] || die "$asset is not listed in SHA256SUMS"
    [ "$expected" = "$actual" ] || die "checksum mismatch for $asset: the download is damaged or was tampered with"

    tar -xzf "$tmp/$asset" -C "$tmp"
    [ -f "$tmp/euros" ] || die "the archive has no euros binary"
    mkdir -p "$DIR"
    chmod 755 "$tmp/euros"
    # mv over the old file works even while euros is running.
    mv -f "$tmp/euros" "$DIR/euros"
    if [ "$(uname -s)" = Darwin ]; then
        xattr -d com.apple.quarantine "$DIR/euros" 2>/dev/null || true
    fi

    NEED_NEW_SHELL=0
    add_to_path
    say ""
    say "$("$DIR/euros" --version 2>/dev/null || echo euros) is installed in $DIR"
    if [ "$NEED_NEW_SHELL" = 1 ]; then
        say "Open a new terminal (or run: export PATH=\"$DIR:\$PATH\"), then: euros login"
    else
        say "Next: euros login"
    fi
}

case "${1:-}" in
    --uninstall) uninstall ;;
    '') install ;;
    *) die "unknown option: $1 (only --uninstall)" ;;
esac
