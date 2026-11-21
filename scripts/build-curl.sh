#!/bin/sh
# Builds a small static libcurl (HTTPS + WebSocket only) for release builds.
#   scripts/build-curl.sh <prefix> <tls option>
#   tls option: --with-openssl (Linux) or --with-secure-transport (macOS)
set -eu

PREFIX=$1
TLS=$2
VERSION=${CURL_VERSION:-8.11.1}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
curl -fsSL -o curl.tar.xz "https://curl.se/download/curl-$VERSION.tar.xz"
if [ -n "${CURL_SHA256:-}" ]; then
    echo "$CURL_SHA256  curl.tar.xz" | sha256sum -c - 2>/dev/null || echo "$CURL_SHA256  curl.tar.xz" | shasum -a 256 -c -
fi
tar -xf curl.tar.xz
cd "curl-$VERSION"

./configure --prefix="$PREFIX" --disable-shared --enable-static "$TLS" --with-zlib --enable-websockets \
    --disable-ldap --disable-ldaps --disable-rtsp --disable-dict --disable-telnet --disable-tftp \
    --disable-pop3 --disable-imap --disable-smb --disable-smtp --disable-gopher --disable-mqtt \
    --disable-manual --disable-docs --disable-ntlm --disable-kerberos-auth --disable-negotiate-auth \
    --without-libpsl --without-brotli --without-zstd --without-nghttp2 --without-libidn2 \
    --without-librtmp --without-libssh2 --without-libgsasl

make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
make install
