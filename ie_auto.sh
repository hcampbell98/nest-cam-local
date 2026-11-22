#!/bin/sh
export PATH=/sbin:/usr/sbin:/usr/local/bin:/usr/bin:/bin

# Daemons that fork at boot require the standard null character device.
if [ ! -c /dev/null ]; then
  null_node="/dev/.nest-null.$$"
  if ! { mknod "$null_node" c 1 3 && chmod 666 "$null_node" && mv -f "$null_node" /dev/null; }; then
    rm -f "$null_node"
    printf '%s\n' 'Could not restore /dev/null' >&2
  fi
fi

# 1. Keep /dev/watchdog fed on one open file descriptor right away
(
  if [ -e /dev/watchdog ]; then
    exec 3>/dev/watchdog
    while true; do
      printf '0' >&3
      sleep 10
    done
  fi
) &

# 2. Start minimal Dropbear SSH server (Curve25519 + Ed25519 + ChaCha20-Poly1305)
if [ -x /mnt/dropcam/dropbear ]; then
  /mnt/dropcam/dropbear -R &
fi

# 3. Enable USB Ethernet interface (192.168.75.2) for always-available local shell
rmmod g_dropcam_storage 2>/dev/null
rmmod g_mass_storage 2>/dev/null
modprobe ambarella_udc 2>/dev/null
modprobe g_ether 2>/dev/null
ifconfig usb0 192.168.75.2 netmask 255.255.255.0 up 2>/dev/null

# 4. Restrict telnetd to USB-only rescue access (port 23 closed on Wi-Fi)
if [ -x /mnt/dropcam/dropbear ]; then
  /usr/sbin/telnetd -b 192.168.75.2 -l /bin/login &
else
  /usr/sbin/telnetd -l /bin/login &
fi

# 5. Neuter Nest/Google cloud & OTA clients cleanly (sleep forever so respawn doesn't spin)
touch /tmp/stop_photon
cat << 'EOF' > /tmp/dummy_sleep
#!/bin/sh
while true; do
  sleep 3600
done
EOF
chmod +x /tmp/dummy_sleep
mount -o bind /tmp/dummy_sleep /usr/bin/connect 2>/dev/null
mount -o bind /tmp/dummy_sleep /usr/bin/photon 2>/dev/null
killall -9 photon connect 2>/dev/null

# 6. Wrap wpa_supplicant to strip the -W flag (bootstrap.sh runs wpa_supplicant after us)
if [ -x /sbin/wpa_supplicant ]; then
  cp /sbin/wpa_supplicant /tmp/wpa_supplicant_real
  cat << 'EOF' > /tmp/wpa_supplicant_wrapper
#!/bin/sh
remaining=$#
while [ "$remaining" -gt 0 ]; do
  arg=$1
  shift
  if [ "$arg" != "-W" ]; then
    set -- "$@" "$arg"
  fi
  remaining=$((remaining - 1))
done
exec /tmp/wpa_supplicant_real "$@"
EOF
  chmod +x /tmp/wpa_supplicant_wrapper
  mount -o bind /tmp/wpa_supplicant_wrapper /sbin/wpa_supplicant 2>/dev/null
fi

# 7. Background Wi-Fi DHCP + optional local startup hook
(
  while [ ! -e /sys/class/net/wlan0 ]; do
    sleep 1
  done
  ifconfig wlan0 up 2>/dev/null

  # Give bootstrap.sh a moment to launch wpa_supplicant, then start DHCP
  sleep 5
  udhcpc -i wlan0 -b -R >/tmp/udhcpc.log 2>&1 &

  # Announce IP on LAN once wlan0 gets a lease so routers/clients find it immediately
  (
    i=0
    while [ "$i" -lt 30 ]; do
      GW=$(route -n 2>/dev/null | awk '$1=="0.0.0.0" && $8=="wlan0" {print $2; exit}')
      if [ -n "$GW" ]; then
        ping -c 2 -W 1 "$GW" >/dev/null 2>&1
        break
      fi
      sleep 2
      i=$((i + 1))
    done
  ) &

  # Optional post-boot user hook (e.g. for local RTSP streamer)
  if [ -x /mnt/dropcam/local_start.sh ]; then
    /mnt/dropcam/local_start.sh >/tmp/local_start.log 2>&1 &
  fi
) &
