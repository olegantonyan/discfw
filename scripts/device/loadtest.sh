#!/bin/sh
# On-device: play FILE on repeat; baseline, then CPU + SD + memory load. Counts PCM restarts and DMA pace misses.
# Usage: setsid sh loadtest.sh FILE [SECONDS_PER_PHASE] [SD_DIR] > /tmp/load.log 2>&1 < /dev/null &
M="mpc -h /run/mpd.sock"
S=/proc/asound/card0/pcm3p/sub0
file=$1
secs=${2:-60}
sd=${3:-/media/sd}
$M -q clear
$M -q add "file://$file"
$M -q repeat on
$M -q single on
$M -q play
sleep 3
rate=$(sed -n 's/^rate: \([0-9]*\).*/\1/p' $S/hw_params)
echo "file=$file rate=$rate mpd: $($M status | sed -n 2p)"
watch() {
	restarts=0; slow=0; stopped=0; n=0
	pt=$(sed -n 's/^trigger_time: //p' $S/status); ph=$(sed -n 's/^hw_ptr *: //p' $S/status); pu=$(cut -d' ' -f1 /proc/uptime)
	end=$(($(cut -d. -f1 /proc/uptime) + $1))
	while [ "$(cut -d. -f1 /proc/uptime)" -lt $end ]; do
		sleep 1
		st=$(sed -n 's/^state: //p' $S/status); tt=$(sed -n 's/^trigger_time: //p' $S/status); h=$(sed -n 's/^hw_ptr *: //p' $S/status); u=$(cut -d' ' -f1 /proc/uptime)
		n=$((n + 1))
		[ "$st" = RUNNING ] || stopped=$((stopped + 1))
		[ -n "$tt" ] && [ "$tt" != "$pt" ] && restarts=$((restarts + 1))
		if [ -n "$h" ] && [ -n "$ph" ] && [ "$tt" = "$pt" ]; then
			awk -v a="$ph" -v b="$h" -v s="$pu" -v e="$u" -v r="$rate" 'BEGIN { exit !((b - a) / (e - s) < r * 0.9) }' && slow=$((slow + 1))
		fi
		pt=$tt; ph=$h; pu=$u
	done
	echo "$2: samples=$n pcm_restarts=$restarts pace_misses=$slow not_running=$stopped load=$(cut -d' ' -f1-3 /proc/loadavg) mpd_rss=$(sed -n 's/VmRSS: *//p' /proc/$(pidof mpd)/status)"
}
watch "$secs" baseline
load() { sh -c "$1" loadgen & }
load 'while :; do :; done'
load 'while :; do :; done'
load "while :; do find '$sd' -type f | while read -r f; do cat \"\$f\" > /dev/null; done; done"
load 'while :; do dd if=/dev/zero of=/tmp/load.bin bs=1M count=24 2>/dev/null; rm -f /tmp/load.bin; done'
watch "$secs" "cpu+sd+mem load"
unload() { for p in $(ps | awk '/loadgen/ && !/awk/ { print $1 }'); do kill "$p" 2>/dev/null; done; }
unload; sleep 1; unload
rm -f /tmp/load.bin
watch 10 recovery
$M -q single off
$M -q stop
echo "LOADTEST DONE"
