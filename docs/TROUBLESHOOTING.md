# Troubleshooting and Recovery Guide

## 1. USB Ethernet Rescue Access (`192.168.75.2`)

Whenever `ie_auto.sh` runs at boot, it loads the USB gadget Ethernet driver (`g_ether`), assigns `192.168.75.2/24` to `usb0`, and binds a USB-only `telnetd` listener to `192.168.75.2` (while Dropbear SSH listens on all interfaces on port `22`). Even if Wi-Fi credentials are wrong or your router is down, you can always log in over USB:

1. Plug the camera into your Linux PC using a data-capable USB cable and wait ~20 seconds for it to boot.
2. Identify the new USB network interface on your host PC:

   ```bash
   ip -br link show
   ```

   Look for a new `usb0` or `enx...` interface.
3. If NetworkManager is running, tell it not to run DHCP on that interface, then assign `192.168.75.1/24`:

   ```bash
   USB_IFACE="enxXXXXXXXXXXXX"  # replace with your detected interface name
   sudo nmcli device set "$USB_IFACE" managed no 2>/dev/null || true
   sudo ip addr flush dev "$USB_IFACE"
   sudo ip addr add 192.168.75.1/24 dev "$USB_IFACE"
   sudo ip link set "$USB_IFACE" up
   ```

4. Connect via SSH (or telnet if Dropbear is not installed):

   ```bash
   ssh root@192.168.75.2
   # or:
   telnet 192.168.75.2
   ```

> **Note:** Each reboot generates a random MAC address on `usb0`, so Linux may give it a different `enx...` interface name after a reboot. Flush `192.168.75.1` from any stale interface if pings fail.

---

## 2. Checking Logs and Runtime State on the Camera

SSH into the camera and inspect the running daemons and `/tmp` logs:

```sh
# Confirm daemons are running
ps | grep -E 'dropbear|nest_rtsp|nest_onvif|wpa_supplicant|udhcpc'

# Check recent RTSP, ONVIF, and startup logs
tail -n 40 /tmp/rtsp.log
tail -n 40 /tmp/onvif.log
cat /tmp/local_start.log
cat /tmp/nest-encoder-init.log
cat /tmp/udhcpc.log

# Inspect live status files
cat /tmp/night_vision
cat /tmp/night_vision.status
cat /tmp/nest_motion.state
cat /tmp/nest_audio.capabilities
```

### Restarting Services Without Rebooting

Both `nest_rtsp` and `nest_onvif` run inside auto-restart `while true` loops in `local_start.sh`. Killing either process automatically restarts it after 2 seconds:

```sh
killall nest_rtsp
killall nest_onvif
```

---

## 3. Common Issues and Fixes

### Camera Does Not Connect to Wi-Fi

1. Connect via USB rescue (`192.168.75.2`).
2. Verify `/mnt/dropcam/wpa_supplicant.conf` has the correct SSID, PSK, and Unix (`LF`) line endings (no Windows `CRLF`).
3. Verify `ie_auto.sh` successfully bind-mounted the `wpa_supplicant` wrapper (stripping `-W`):

   ```sh
   ps | grep wpa_supplicant
   cat /tmp/udhcpc.log
   ```

   If `-W` is still present in the `wpa_supplicant` command line, `ie_auto.sh` may not have executable permissions (`chmod 755 /mnt/dropcam/ie_auto.sh`).
4. Ensure your router broadcasts 2.4 GHz WPA2-PSK (AES/CCMP). Legacy Ambarella/Atheros Wi-Fi drivers do not support WPA3-only networks.

### Video Is Upside-Down or Mirrored

`local_start.sh` passes `-m 3` by default for an upside-down mount. Edit `/mnt/dropcam/local_start.sh` and change `-m 3` to:
- `-m 0`: normal (upright)
- `-m 1`: horizontal flip
- `-m 2`: vertical flip
- `-m 3`: 180-degree rotation

Then restart `nest_rtsp` (`killall nest_rtsp`).

### Colors Look Wrong (Purple/Green Tint)

- Confirm your camera is `hwver=5` (`cat /mnt/dropcam/hwver`) with the AR0330 sensor.
- Verify the stock ISP calibration files exist on the rootfs:

  ```sh
  ls -l /etc/idsp/reg.bin /etc/idsp/sensors/mt9t002_03_3D.bin /etc/idsp/sensors/mt9t002_04_3D.bin
  ```

- If you have a different sensor revision, test `-y 0`, `-y 1`, `-y 2`, or `-y 3` (Bayer pattern order) when running `nest_rtsp`.
- Check whether the IR-cut filter is stuck in night mode (`cat /tmp/night_vision.status`) and test toggling it manually:

  ```sh
  echo off > /tmp/night_vision   # force day mode (IR-cut filter IN, IR LEDs off)
  echo on  > /tmp/night_vision   # force night mode (IR-cut filter OUT, IR LEDs on)
  echo auto > /tmp/night_vision  # return to automatic switching
  ```

### Night Vision Cycles Rapidly at Dusk or Near a Wall

Bright IR reflection off a nearby wall, soffit, or window can trick simple luma thresholds. `nest_rtsp` includes cooldown windows and IR-off probing to prevent oscillation, and dims the IR LEDs automatically when reflections are too bright.

- Check `/tmp/night_vision.status` and `/tmp/rtsp.log` to see current luma and IR PWM values.
- If the camera is aimed through glass (indoor looking out a window), IR LEDs will reflect off the glass and wash out the image; set `echo off > /tmp/night_vision` (or set Night Vision to Off in UniFi Protect).

### UniFi Protect Cannot Discover or Adopt the Camera

1. Confirm `nest_onvif` is listening on port `80` and `nest_rtsp` on port `554`:

   ```sh
   netstat -tulnp
   ```

2. Confirm `/mnt/dropcam/onvif.conf` exists and follows `username:password` on one line with no trailing spaces.
3. Verify the camera's clock or credentials did not reject WS-Security auth by checking `/tmp/onvif.log`.
4. If the camera and Protect console are on different VLANs, WS-Discovery multicast (`239.255.255.250:3702`) will not cross subnets. Use **Advanced Adoption** in Protect and enter the camera's IP and port `80`.

### Binary Fails to Execute (`No such file or directory` or `GLIBC_2.xx not found`)

- **`No such file or directory` on an existing binary:** The ELF dynamic linker path is wrong. Confirm it uses `/lib/ld-linux.so.3` (`arm-linux-gnueabi-readelf -l nest_rtsp/nest_rtsp | grep interpreter`).
- **`version 'GLIBC_2.xx' not found`:** A symbol from a newer host glibc leaked into the link. Rebuild using the repository Makefiles or `dropbear/build.sh` with `ROOTFS` pointing to the camera's extracted glibc 2.13 rootfs, and confirm with:

  ```bash
  arm-linux-gnueabi-readelf -V nest_rtsp/nest_rtsp | grep 'Name: GLIBC'
  ```

  Only `GLIBC_2.4` (and up to `GLIBC_2.13`) should appear.

### Audio Testing Before Enabling Speaker or Two-Way Mode

Leave `/mnt/dropcam/audio.mode` at `off` (or omitted) until you have tested audio on your physical mount:

1. Start with `mic` only:

   ```sh
   echo mic > /mnt/dropcam/audio.mode
   killall nest_rtsp
   ```

   Verify clean audio playback in `ffplay` or go2rtc.
2. Make sure `/mnt/dropcam/onvif.conf` has a strong password set (the speaker track is disabled when auth is unconfigured).
3. Test `two-way` or `speaker` through go2rtc and listen for feedback/echo or clipping on the installed mount before leaving it enabled permanently.

---

## 4. Full Recovery (If `ie_auto.sh` Breaks Boot)

Because the Ambarella A5s ROM bootloader is read-only, a bad script on `mtd9` cannot brick the hardware:

1. Unplug the camera, hold the physical bootloader button, and plug it into your Linux PC to enter USB DFU mode (`4255:0001`).
2. Run `Dropcam-Rooter/root_camera.sh` (Option 1) to boot the RAM kernel and re-flash a clean `mtd9` partition.
3. Or, to restore your exact pre-root backup (`backups/adc_stock.bin`) from the RAM kernel shell (`10.9.9.1`):

   ```sh
   # On the camera in RAM kernel shell (after binding ambarella-nand):
   /usr/sbin/flash_eraseall /dev/mtd9
   /usr/sbin/nandwrite -p /dev/mtd9 /tmp/adc_stock.bin
   ```

   Then unplug and replug the camera to boot stock.
