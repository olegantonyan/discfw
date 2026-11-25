#!/bin/sh
TARGET="$1"
READELF="$(ls "$2"/bin/*-readelf | head -1)"
bad=0
n=0
for f in $(find "$TARGET" -type f ! -name '*.ko' ! -path '*/lib/firmware/*'); do
	head -c 4 "$f" | grep -q "ELF" || continue
	n=$((n + 1))
	hdr="$("$READELF" -h "$f")"
	echo "$hdr" | grep -q "Machine:.*MIPS" || { echo "check-abi: not MIPS: $f"; bad=1; continue; }
	echo "$hdr" | grep -q "nan2008" || { echo "check-abi: legacy NaN: $f"; bad=1; }
	"$READELF" -A "$f" | grep -q "FP ABI: Hard float (32-bit CPU, 64-bit FPU)" || { echo "check-abi: not FP64: $f"; bad=1; }
done
echo "check-abi: $n ELF files checked"
exit $bad
