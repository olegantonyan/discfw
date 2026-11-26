#!/bin/sh
# On-device smoke test of discsys + discui + MPD through their sockets. Usage: uitest.sh [FOLDER_WITH_MUSIC]
# Leaves settings as they were; changes playback (plays the folder, then pauses).
DIR=${1:-}
M="mpc -h /run/mpd.sock"
pass=0
fail=0

ok() { pass=$((pass + 1)); echo "PASS $1"; }
bad() { fail=$((fail + 1)); echo "FAIL $1: $2"; }
ui() { discfw ui "$@"; }
field() { ui state | tr ' ' '\n' | sed -n "s/^$1=//p"; }
check() { [ "$2" = "$3" ] && ok "$1" || bad "$1" "got '$2', want '$3'"; }

discfw state > /dev/null && ok "discsys answers" || bad "discsys answers" "no reply"
ui state > /dev/null && ok "discui answers" || bad "discui answers" "no reply"
discfw screen on > /dev/null
discfw screen unlock > /dev/null
sleep 0.5

ui goto folders > /dev/null
sleep 0.3
case "$(field nav)" in *folders) ok "open folders" ;; *) bad "open folders" "$(field nav)" ;; esac

ui swipe 180 6 180 200 250 > /dev/null
sleep 0.6
check "shade opens" "$(field shade)" 1
ui swipe 180 300 180 100 250 > /dev/null
sleep 0.6
check "shade closes" "$(field shade)" 0

ui goto settings > /dev/null
sleep 0.3
before=$(field nav)
ui swipe 4 180 140 182 220 > /dev/null
sleep 0.6
after=$(field nav)
[ "$before" != "$after" ] && ok "back gesture" || bad "back gesture" "$before -> $after"

old=$(discfw cfg ui.cover_mode)
discfw set ui.cover_mode vinyl > /dev/null
sleep 3.5
check "setting saved" "$(discfw cfg ui.cover_mode)" vinyl
discfw set ui.cover_mode "$old" > /dev/null

oldvol=$($M volume | sed -n 's/volume: *\([0-9]*\)%.*/\1/p')
discfw vol 37 > /dev/null
sleep 0.3
check "volume to MPD" "$($M volume | sed -n 's/volume: *\([0-9]*\)%.*/\1/p')" 37
discctl status | grep -q 'volpct=37' && ok "volume to DAC" || bad "volume to DAC" "$(discctl status)"
discfw vol "${oldvol:-50}" > /dev/null

discfw screen lock > /dev/null
sleep 0.3
discfw screen on > /dev/null
sleep 0.8
check "lock on wake" "$(field locked)" 1
ui swipe 180 300 180 150 200 > /dev/null
sleep 0.6
check "unlock swipe" "$(field locked)" 0

if [ -n "$DIR" ]; then
	ui folder "$(echo "$DIR" | sed 's/%/%25/g; s/ /%20/g')" > /dev/null
	sleep 0.5
	ui tap 150 88 > /dev/null
	sleep 2
	$M status | grep -q '^\[playing\]' && ok "play from folder" || bad "play from folder" "$($M status | head -2)"
	case "$(field nav)" in *nowplaying) ok "now playing opens" ;; *) bad "now playing opens" "$(field nav)" ;; esac
	$M -q pause
fi

echo "uitest: $pass passed, $fail failed"
[ "$fail" = 0 ]
