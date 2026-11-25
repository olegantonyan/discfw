#!/bin/sh
# On-device, read-only: power, thermal, radios, storage, USB, input, memory.
for d in /sys/class/power_supply/*; do
	echo "== $d"
	for a in type status online present capacity voltage_now current_now temp health charge_type input_current_limit constant_charge_current; do
		[ -r "$d/$a" ] && echo "  $a=$(cat $d/$a 2>/dev/null)"
	done
done
echo "== thermal"; for z in /sys/class/thermal/thermal_zone*; do [ -r $z/temp ] && echo "  $(cat $z/type 2>/dev/null) $(cat $z/temp)"; done
echo "== rfkill"; for r in /sys/class/rfkill/*; do [ -d $r ] && echo "  $(cat $r/name) $(cat $r/type) soft=$(cat $r/soft) hard=$(cat $r/hard)"; done
echo "== mmc"; for m in /sys/bus/mmc/devices/*; do [ -d $m ] && echo "  ${m##*/} type=$(cat $m/type 2>/dev/null) name=$(cat $m/name 2>/dev/null) vendor=$(cat $m/vendor 2>/dev/null) device=$(cat $m/device 2>/dev/null)"; done
echo "== sdio funcs"; for f in /sys/bus/sdio/devices/*; do [ -d $f ] && echo "  ${f##*/} vendor=$(cat $f/vendor) device=$(cat $f/device)"; done
echo "== usb role"; cat /sys/class/usb_role/*/role 2>/dev/null; ls /sys/class/udc
echo "== input"; for i in /sys/class/input/event*; do echo "  ${i##*/} $(cat $i/device/name)"; done
echo "== dev"; ls /dev | tr '\n' ' '; echo
echo "== cpufreq"; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies 2>&1
echo "== mem"; free; echo "== load"; cat /proc/loadavg
echo "== procs"; for p in mpd discd discui; do pid=$(pidof $p); [ -n "$pid" ] && echo "  $p pid=$pid $(grep -E 'VmRSS|Threads' /proc/$pid/status | tr '\n' ' ')"; done
echo "== uptime"; cat /proc/uptime
