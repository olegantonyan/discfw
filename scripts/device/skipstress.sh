#!/bin/sh
# On-device (queue tracks >= 30 s, repeat on): random next/prev bursts; after each, verify PCM runs at the stream rate, MPD time is real-time, DAC matches.
M="mpc -h /run/mpd.sock"
S=/proc/asound/card0/pcm3p/sub0
bursts=${1:-20}
maxn=${2:-4}
pass=0
fail=0
secs() { echo "$1" | awk -F: '{ print $1 * 60 + $2 }'; }
el() { $M status | sed -n 's/.* \([0-9]*:[0-9]*\)\/.*/\1/p'; }
short=$($M playlist -f '%time%' | awk -F: '$1 * 60 + $2 < 30' | wc -l)
[ "$short" -gt 0 ] && echo "WARN: $short queued tracks < 30 s; tracks ending mid-check read as failures"
for b in $(seq 1 "$bursts"); do
	n=$((RANDOM % maxn + 1))
	for i in $(seq 1 $n); do
		if [ $((RANDOM % 3)) = 0 ]; then $M -q prev; else $M -q next; fi
		sleep 0.0$((RANDOM % 9 + 1))
	done
	t0=$(cut -d' ' -f1 /proc/uptime)
	sleep 1.5
	h1=$(sed -n 's/^hw_ptr *: //p' $S/status); t1=$(cut -d' ' -f1 /proc/uptime); e1=$(secs "$(el)")
	sleep 2
	h2=$(sed -n 's/^hw_ptr *: //p' $S/status); t2=$(cut -d' ' -f1 /proc/uptime); e2=$(secs "$(el)")
	st=$(sed -n 's/^state: //p' $S/status)
	rate=$(sed -n 's/^rate: \([0-9]*\).*/\1/p' $S/hw_params)
	dac=$(discctl status)
	v=$(awk -v a="$h1" -v b="$h2" -v s="$t1" -v e="$t2" -v r="$rate" -v e1="$e1" -v e2="$e2" -v st="$st" -v dac="$dac" 'BEGIN {
		fps = (a == "" || b == "") ? 0 : (b - a) / (e - s)
		de = e2 - e1
		ok = st == "RUNNING" && r > 0 && fps > r * 0.9 && fps < r * 1.1 && de >= 1 && de <= 3 && index(dac, "rate=" r " ") && index(dac, "muted=0")
		printf "%s fps=%d/%d dt_mpd=%d %s", ok ? "PASS" : "FAIL", fps, r, de, st
	}')
	case $v in PASS*) pass=$((pass + 1)) ;; *) fail=$((fail + 1)) ;; esac
	echo "burst $b n=$n $v | $dac | $($M status | sed -n 1p | cut -c1-50)"
	if [ "$($M status | sed -n 's/^\[\([a-z]*\)\].*/\1/p')" != playing ]; then $M -q play 1; fi
done
echo "RESULT pass=$pass fail=$fail"
