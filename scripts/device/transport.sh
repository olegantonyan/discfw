#!/bin/sh
# On-device: gapless same-format transition, seek, rapid pause/resume, stop/play. Usage: transport.sh CORPUS_DIR
M="mpc -h /run/mpd.sock"
S=/proc/asound/card0/pcm3p/sub0
c=${1:-/tmp/corpus}
pass=0
fail=0
running() {
	h1=$(sed -n 's/^hw_ptr *: //p' $S/status); sleep 0.5; h2=$(sed -n 's/^hw_ptr *: //p' $S/status)
	[ "$(sed -n 's/^state: //p' $S/status)" = RUNNING ] && [ -n "$h1" ] && [ "$h2" -gt "$h1" ] && discctl status | grep -q muted=0
}
check() {
	if eval "$2"; then pass=$((pass + 1)); echo "PASS $1"; else fail=$((fail + 1)); echo "FAIL $1 | $(sed -n 's/^state: //p' $S/status) | $(discctl status) | $($M status | tr '\n' ' ')"; fi
}
$M -q single off
$M -q repeat off
$M -q clear
for f in r44100.flac c-s16.wav r44100.flac; do $M -q add "file://$c/$f"; done
$M -q play 1
sleep 0.8
tt=$(sed -n 's/^trigger_time: //p' $S/status)
own=$(sed -n 's/^owner_pid *: //p' $S/status)
while [ "$($M status | sed -n 's/.*#\([0-9]*\)\/.*/\1/p')" = 1 ]; do sleep 0.1; done
sleep 0.5
check "gapless 1->2 same open PCM" '[ "$(sed -n "s/^trigger_time: //p" $S/status)" = "$tt" ] && [ "$(sed -n "s/^owner_pid *: //p" $S/status)" = "$own" ] && running'
$M -q clear
$M -q add "/media/sd/Thy Catafalque/Thy Catafalque - 2020 - Naiv/Thy Catafalque - Naiv - 01 A bolyongás ideje.flac" 2>/dev/null || $M -q add "file:///media/sd/Thy Catafalque/Thy Catafalque - 2020 - Naiv/Thy Catafalque - Naiv - 01 A bolyongás ideje.flac"
$M -q play 1
sleep 1
for i in 1 2 3 4 5 6; do $M -q seek $((RANDOM % 250 + 10)); sleep 0.0$((RANDOM % 5)); done
sleep 1
e1=$($M status | sed -n 's/.* \([0-9]*\):\([0-9]*\)\/.*/\1 \2/p' | awk '{print $1 * 60 + $2}')
check "6 rapid seeks -> playing" 'running'
sleep 2
e2=$($M status | sed -n 's/.* \([0-9]*\):\([0-9]*\)\/.*/\1 \2/p' | awk '{print $1 * 60 + $2}')
check "after seeks time advances in real time ($e1 -> $e2)" '[ $((e2 - e1)) -ge 1 ] && [ $((e2 - e1)) -le 4 ]'
for i in $(seq 1 15); do $M -q toggle; sleep 0.0$((RANDOM % 9)); done
[ "$($M status | sed -n 's/^\[\([a-z]*\)\].*/\1/p')" = playing ] || $M -q play
sleep 1
check "15 rapid pause toggles -> playing" 'running'
$M -q pause
sleep 1
check "paused -> PCM closed, DAC muted after release" '[ "$(head -1 $S/status)" = closed ] && discctl status | grep -q muted=1'
$M -q play
sleep 1
check "resume after pause" 'running'
$M -q stop
sleep 1
check "stop -> PCM closed, DAC muted" '[ "$(head -1 $S/status)" = closed ] && discctl status | grep -q muted=1'
$M -q play
sleep 1
check "play after stop" 'running'
$M -q stop
echo "RESULT pass=$pass fail=$fail"
