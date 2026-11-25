#!/bin/sh
# On-device soak: shuffle-play the current queue for N seconds with periodic skip bursts; log anomalies + resources.
# Usage: nohup sh soak.sh SECONDS > /tmp/soak.log 2>&1 &
M="mpc -h /run/mpd.sock"
S=/proc/asound/card0/pcm3p/sub0
dur=${1:-1800}
start=$(cut -d. -f1 /proc/uptime)
bad=0
anomalies=0
samples=0
last_skip=$start
prev_h=""; prev_t=""; prev_cpu=""
$M -q random on
$M -q repeat on
$M -q play
while :; do
	now=$(cut -d. -f1 /proc/uptime)
	[ $((now - start)) -ge "$dur" ] && break
	if [ $((now - last_skip)) -ge 120 ]; then
		n=$((RANDOM % 4 + 1))
		for i in $(seq 1 $n); do $M -q next; sleep 0.0$((RANDOM % 9)); done
		echo "$(date +%T) skip burst n=$n"
		last_skip=$now
		prev_h=""
		sleep 2
	fi
	t=$(cut -d' ' -f1 /proc/uptime)
	h=$(sed -n 's/^hw_ptr *: //p' $S/status)
	st=$(sed -n 's/^state: //p' $S/status)
	rate=$(sed -n 's/^rate: \([0-9]*\).*/\1/p' $S/hw_params)
	mst=$($M status | sed -n 's/^\[\([a-z]*\)\].*/\1/p')
	dac=$(discctl status)
	pid=$(pidof mpd)
	cpu=$(awk '{print $14 + $15}' /proc/$pid/stat)
	ok=1
	if [ -n "$prev_h" ] && [ -n "$h" ] && [ "$st" = RUNNING ]; then
		fps=$(awk -v a="$prev_h" -v b="$h" -v s="$prev_t" -v e="$t" 'BEGIN { d = b - a; printf "%d", (d < 0 ? 0 : d) / (e - s) }')
		awk -v f="$fps" -v r="$rate" 'BEGIN { exit !(f < r * 0.85 || f > r * 1.15) }' && ok=0
		echo "$dac" | grep -q "rate=$rate .*muted=0" || ok=0
	elif [ "$mst" = playing ] && [ "$st" != RUNNING ]; then
		ok=0
	fi
	samples=$((samples + 1))
	if [ $ok = 0 ]; then
		bad=$((bad + 1))
		[ $bad -ge 2 ] && { anomalies=$((anomalies + 1)); echo "$(date +%T) ANOMALY st=$st rate=$rate fps=$fps mpd=$mst | $dac | $($M status | sed -n 1,2p | tr '\n' ' ')"; }
	else
		bad=0
	fi
	if [ $((samples % 12)) = 0 ]; then
		cpup=""
		[ -n "$prev_cpu" ] && cpup=$(awk -v a="$prev_cpu" -v b="$cpu" 'BEGIN { printf "%.1f", (b - a) / 60 }')
		echo "$(date +%T) t=$((now - start))s st=$st rate=$rate mpd_cpu=${cpup}% mpd_rss=$(sed -n 's/VmRSS: *//p' /proc/$pid/status) discui_rss=$(sed -n 's/VmRSS: *//p' /proc/$(pidof discui)/status) memfree=$(sed -n 's/MemAvailable: *//p' /proc/meminfo) anomalies=$anomalies | $($M status | sed -n 1p | cut -c1-50)"
		prev_cpu=$cpu
	fi
	prev_h=$h; prev_t=$t
	sleep 5
done
$M -q random off
echo "SOAK DONE samples=$samples anomalies=$anomalies"
