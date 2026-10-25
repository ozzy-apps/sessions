#!/usr/bin/env bash
# Run inside an MSYS2 UCRT64 shell after building with the same toolchain.
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir="${1:-build}"
stage="$PWD/build/windows-package"
cmake -E rm -rf "$stage"
mkdir -p "$stage" dist
cmake --install "$build_dir" --prefix "$stage" --component Runtime
mv "$stage/bin/sessions.exe" "$stage/Sessions.exe"
rmdir "$stage/bin"
windeployqt6 --release --no-translations --compiler-runtime "$stage/Sessions.exe"
# Copy libsndfile and its transitive codecs, which windeployqt cannot discover.
cp /ucrt64/bin/libsndfile-1.dll "$stage/"
for dll in $(ldd /ucrt64/bin/libsndfile-1.dll "$stage/Sessions.exe" | awk '/\/ucrt64\/bin\// {print $3}' | sort -u); do cp "$dll" "$stage/"; done
# The Qt network TLS backend needs the OpenSSL runtime as well.
cp /ucrt64/bin/libssl-3-x64.dll /ucrt64/bin/libcrypto-3-x64.dll "$stage/"
cp distribution/THIRD-PARTY.md "$stage/"
cp -R distribution/licenses "$stage/"
mkdir -p "$stage/licenses/msys2"
cp -R /ucrt64/share/licenses/* "$stage/licenses/msys2/"
if test -f LICENSE; then cp LICENSE "$stage/"; fi
cmake -E rm -f dist/Sessions-windows-x86_64.zip
(cd "$stage" && 7z a -tzip ../../dist/Sessions-windows-x86_64.zip .)
