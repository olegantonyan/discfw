#!/bin/sh
# Prints "Zone<TAB>POSIX-TZ" for every zone in zone1970.tab, from a compiled zoneinfo tree (TZif footer line).
# Regenerate zones.tsv after tzdata updates: ./mkzones.sh /usr/share/zoneinfo > zones.tsv
Z=$1
printf 'UTC\tUTC0\n'
grep -v '^#' "$Z/zone1970.tab" | cut -f3 | sort -u | while read -r zone; do
	f="$Z/posix/$zone"
	[ -f "$f" ] || f="$Z/$zone"
	[ -f "$f" ] || continue
	tz=$(tail -n 1 "$f")
	[ -n "$tz" ] && printf '%s\t%s\n' "$zone" "$tz"
done
