#!/bin/sh
# On-device: time from "mpc next" until the PCM is RUNNING again with a new trigger time (ms, ~10 ms resolution).
M="mpc -h /run/mpd.sock"
S=/proc/asound/card0/pcm3p/sub0/status
up() { cut -d' ' -f1 /proc/uptime; }
for i in $(seq 1 ${1:-10}); do
	trig=$(sed -n 's/^trigger_time: //p' $S)
	t0=$(up)
	$M -q next
	while :; do
		st=$(sed -n 's/^state: //p' $S)
		nt=$(sed -n 's/^trigger_time: //p' $S)
		[ "$st" = RUNNING ] && [ "$nt" != "$trig" ] && break
		t=$(up)
		awk -v a="$t0" -v b="$t" 'BEGIN { exit !(b - a > 5) }' && { echo "timeout"; break; }
	done
	t1=$(up)
	echo "skip $i: $(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%d", (b - a) * 1000 }') ms rate=$(sed -n 's/^rate: \([0-9]*\).*/\1/p' ${S%status}hw_params)"
	sleep 1.5
done
