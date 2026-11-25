#!/bin/sh
set -e
IMG="$BINARIES_DIR/rootfs.squashfs"
OUT="$BINARIES_DIR/disc-rootfs-96M.bin"
SIZE=100663296

used=$(stat -c %s "$IMG")
[ "$used" -le "$SIZE" ] || { echo "rootfs.squashfs is $used bytes, max $SIZE"; exit 1; }
cp "$IMG" "$OUT"
truncate -s $SIZE "$OUT"
cd "$BINARIES_DIR"
sha256sum disc-rootfs-96M.bin rootfs.squashfs xImage-stock-v257 > sha256sums.txt
echo "post-image: rootfs.squashfs $used bytes, padded image $OUT"
