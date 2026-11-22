#!/bin/sh
export PATH=/sbin:/usr/sbin:/usr/local/bin:/usr/bin:/bin

# Turn off the RGB status LED after boot.
for led in /sys/class/leds/lp5521:channel*; do
  [ -d "$led" ] || continue
  printf '0\n' >"$led/brightness"
  printf '0\n' >"$led/led_current"
done

# Start ONVIF Profile S + WS-Discovery daemon (port 80 + UDP 3702) once wlan0 has an IPv4 address
if [ -x /mnt/dropcam/nest_onvif ] && ! pidof nest_onvif >/dev/null; then
  (
    i=0
    while [ "$i" -lt 20 ] && ! ifconfig wlan0 2>/dev/null | grep -q 'inet addr:'; do
      sleep 1
      i=$((i + 1))
    done
    while true; do
      /mnt/dropcam/nest_onvif -p 80 -r 554 -i wlan0 -c /mnt/dropcam/onvif.conf >/tmp/onvif.log 2>&1
      sleep 2
    done
  ) &
fi

# Start RTSP with the audio directions enabled for this installation.
while true; do
  # The stock filesystem has a static /dev/iav node before its driver loads.
  if ! grep -qw iav /proc/devices; then
    if ! /usr/local/bin/init.sh --ar0330 >/tmp/nest-encoder-init.log 2>&1; then
      sleep 2
      continue
    fi
  fi
  set --
  case "$(cat /mnt/dropcam/audio.mode 2>/dev/null)" in
    mic) set -- --mic ;;
    speaker) set -- --speaker ;;
    two-way) set -- --mic --speaker ;;
  esac
  # Mirror both sensor axes for the upside-down mount; ABR scales 600 kbps..2.2 Mbps with Wi-Fi headroom.
  /mnt/dropcam/nest_rtsp -w 1920 -h 1080 -b 2200000 --min-bitrate 600000 -p 554 -m 3 "$@" >/dev/null 2>&1
  sleep 2
done
