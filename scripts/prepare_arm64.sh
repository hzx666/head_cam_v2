#!/usr/bin/env bash
# Download/extract Ubuntu ARM64 static development libraries into the workspace.
# No target packages are installed on the host. Requires apt-get and dpkg-deb.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
DEST="$ROOT/.deps/arm64"
WORK=$(mktemp -d)
trap 'rm -rf -- "$WORK"' EXIT
mkdir -p "$WORK/lists/partial" "$WORK/cache/archives/partial" "$DEST"
cat > "$WORK/sources.list" <<'EOF'
deb [arch=arm64] https://ports.ubuntu.com/ubuntu-ports jammy main universe
deb [arch=arm64] https://ports.ubuntu.com/ubuntu-ports jammy-updates main universe
deb [arch=arm64] https://ports.ubuntu.com/ubuntu-ports jammy-security main universe
EOF
OPTS=(-o APT::Architecture=arm64 -o APT::Architectures::=arm64
      -o Dir::Etc::sourcelist="$WORK/sources.list" -o Dir::Etc::sourceparts=-
      -o Dir::State::lists="$WORK/lists" -o Dir::Cache="$WORK/cache"
      -o APT::Get::List-Cleanup=0 -o Acquire::Languages=none)
apt-get "${OPTS[@]}" update -qq
cd "$WORK"
apt-get "${OPTS[@]}" download busybox-static:arm64 libssl-dev:arm64 liblz4-dev:arm64 libzstd-dev:arm64
for package in ./*.deb; do dpkg-deb -x "$package" "$DEST"; done
sha256sum ./*.deb > "$DEST/package-sha256.txt"
printf 'Prepared ARM64 static libraries in %s\n' "$DEST"
