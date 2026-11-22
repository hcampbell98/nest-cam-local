<img src="assets/icon.svg" width="64" height="64" alt="">

# nest-cam-local

Reclaim a Gen 1 Nest Cam Outdoor from the cloud and turn it into a 100% local, privacy-respecting ONVIF Profile S and 1080p H.264 RTSP IP camera. Works offline with UniFi Protect, Frigate, Home Assistant, go2rtc, Blue Iris, or any standard ONVIF/RTSP NVR.

## What You Get

- **1080p H.264 RTSP streaming** (`nest_rtsp`) at 30 fps over TCP/UDP on port `554` (`/stream1`) with RTSP Digest and Basic authentication.
- **Calibrated daylight color and auto exposure/white balance** using the Ambarella A5s ISP black-level correction, AR0330 3D LUTs (`/etc/idsp/`), and continuous YUV-based AE/AWB.
- **Automatic day/night vision** controlling the mechanical IR-cut filter (`gpio45`/`gpio46`) and IR LED PWM dimmer (`pwm-backlight.0`), with hysteresis, close-reflection protection, and ONVIF `IrCutFilter` override.
- **AIMD adaptive bitrate (ABR)** that monitors live TCP send-queue pressure and scales smoothly between `--min-bitrate` (default `600 kbps` in `local_start.sh`) and `-b` (`2.2 Mbps`) so Wi-Fi fluctuations do not stall your NVR.
- **On-camera motion detection** analyzing preview frames with exposure compensation and IR-cut settle suppression, published via ONVIF PullPoint events (`IsMotion` and `MotionAlarm`).
- **ONVIF Profile S + WS-Discovery** (`nest_onvif`) on HTTP port `80` and UDP `3702`, with WS-Security UsernameToken + HTTP Digest/Basic auth, live JPEG snapshots (`/snapshot.jpg`), imaging control, and event subscriptions.
- **Optional local audio** using mono 8 kHz G.711 u-law (`PCMU`) for microphone capture and ONVIF two-way audio backchannel (`off`, `mic`, `speaker`, or `two-way`).
- **Cloud and OTA isolation** (`ie_auto.sh`) that neutralizes stock Nest `photon` and `connect` daemons, disables the status LED, replaces open Wi-Fi telnet with modern Dropbear SSH (Curve25519 + Ed25519 + ChaCha20-Poly1305), and preserves a USB-only rescue shell at `192.168.75.2`.

## Supported Hardware

Tested and verified on:

| Property | Value |
| --- | --- |
| Model | Gen 1 Nest Cam Outdoor (wired) |
| SoC | Ambarella A5s (ARMv6 soft-float) |
| Hardware revision | `hwver=5` (check `/mnt/dropcam/hwver` after rooting) |
| Image sensor | Aptina/ON Semi AR0330 / MT9T002 (`init.sh --ar0330`) |
| Stock OS | Linux 2.6.38.8, glibc 2.13 |

Other Ambarella A5s Dropcam/Nest cameras may require different sensor init flags, Bayer pattern (`-y`), or IR-cut GPIO pins (inspect `/etc/dropcam.conf` on your unit).

> **Note:** Both `nest_rtsp` and `nest_onvif` are intentionally single-threaded (`select()` event loops, no `libpthread`) to avoid a `futex_wait` kernel oops in the stock 2.6.38.8 firmware.

## Architecture Overview

At boot, stock `/usr/bin/bootstrap.sh` checks for `/mnt/dropcam/ie_auto.sh` on the writable JFFS2 configuration partition (`mtd9`) and runs it as `root`.

```text
/usr/bin/bootstrap.sh (stock RO rootfs on mtd6)
  `-> /mnt/dropcam/ie_auto.sh
        |- Restores /dev/null if missing
        |- Feeds /dev/watchdog every 10s so the camera does not reboot
        |- Starts /mnt/dropcam/dropbear -R (SSH on port 22, Ed25519 host key)
        |- Brings up USB Ethernet rescue interface usb0 (192.168.75.2)
        |- Binds telnetd to 192.168.75.2 only (port 23 closed on Wi-Fi)
        |- Bind-mounts dummy sleep over /usr/bin/photon and /usr/bin/connect
        |- Wraps /sbin/wpa_supplicant to strip -W so Wi-Fi associates offline
        |- Runs udhcpc on wlan0 and pings gateway to announce LAN presence
        `-> Starts /mnt/dropcam/local_start.sh
              |- Turns off the front RGB status LED (lp5521)
              |- Runs nest_onvif watchdog loop (HTTP :80, WS-Discovery UDP :3702)
              |- Initializes Ambarella IAV driver (init.sh --ar0330)
              |- Reads /mnt/dropcam/audio.mode (off|mic|speaker|two-way)
              `- Runs nest_rtsp watchdog loop (RTSP :554, 1080p, ABR, night vision, motion)
```

Everything you install lives in `/mnt/dropcam` (~8 MB JFFS2 flash partition). The stock read-only root filesystem (`mtd6`) is never modified.

## Prerequisites

### Hardware

- A Gen 1 Nest Cam Outdoor.
- A Linux PC (Ubuntu, Debian, Pop!_OS, or Fedora recommended).
- A data-capable USB cable to connect the camera directly to a USB port on your PC (avoid unpowered USB hubs).
- A 2.4 GHz Wi-Fi network (or 2.4/5 GHz network compatible with the camera's legacy Wi-Fi chipset).

### Host Software (Ubuntu/Debian)

Install the ARM cross-compiler and build utilities:

```bash
sudo apt update
sudo apt install -y \
  gcc-arm-linux-gnueabi \
  binutils-arm-linux-gnueabi \
  make git curl python3 python3-venv \
  netcat-openbsd iproute2 telnet openssh-client
```

Clone this repository:

```bash
git clone https://github.com/<your-username>/nest-cam-local.git
cd nest-cam-local
```

---

## Step 1: Root the Camera and Back Up Flash

Use [weegeeday/Dropcam-Rooter](https://github.com/weegeeday/Dropcam-Rooter) to root the camera over USB without soldering. It boots a temporary Linux kernel into RAM via the Ambarella USB bootloader, dumps the writable config partition (`mtd9`), enables root access with your Wi-Fi credentials, and injects the `/mnt/dropcam/ie_auto.sh` boot hook.

1. Clone and prepare `Dropcam-Rooter` (note that `Dropcam-Rooter/` is gitignored if you clone it inside this directory):

   ```bash
   git clone https://github.com/weegeeday/Dropcam-Rooter.git
   cd Dropcam-Rooter
   chmod +x setup.sh root_camera.sh rooted_toolkit.sh
   ./setup.sh
   ```

   Follow the `setup.sh` prompts to supply the required `HD2-firmware.bin` bootstrap file as documented in `Dropcam-Rooter/HOWTO.md`.

2. Put the camera into Ambarella USB DFU bootloader mode:
   - Unplug the camera from USB power.
   - Hold down the physical bootloader button on the camera PCB/housing (see `Dropcam-Rooter/HOWTO.md` for details).
   - Plug the USB cable into your Linux host while holding the button.
   - Wait 2 seconds, then release. The LED stays off and `lsusb` shows Ambarella bootloader VID:PID `4255:0001`.

3. Run the rooting script:

   ```bash
   ./root_camera.sh
   ```

   - Enter your Wi-Fi SSID and password when prompted.
   - Choose **Option 1** (Boot camera into RAM via USB bootloader).
   - Wait until the script finishes flashing `mtd9` and tells you to reboot.

4. Unplug the camera and plug it back in to boot normally. Find its IP address on your router's DHCP client list (or connect via USB Ethernet rescue; see [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)).

5. Connect via initial telnet and immediately set a strong root password (saved to `/mnt/dropcam/shadow`):

   ```bash
   telnet <CAMERA_IP>
   # Login as: root (empty password initially)
   passwd
   cat /mnt/dropcam/hwver
   ```

   Confirm `hwver` reports `5` for a Gen 1 Nest Cam Outdoor.

6. **Back up stock flash partitions** before making further changes. Return to your `nest-cam-local` directory (`cd ..`) and pull `mtd9` (config) and `mtd6` (active rootfs) into `backups/` (gitignored):

   ```bash
   mkdir -p backups
   ```

   On your Linux PC, start a temporary netcat receiver for `mtd9`:

   ```bash
   nc -l -p 9009 > backups/adc_stock.bin
   ```

   In the camera's shell, send `mtd9`:

   ```sh
   dd if=/dev/mtd9 bs=4096 | nc <YOUR_PC_IP> 9009
   ```

   Repeat for `mtd6` (the ~43 MB UBIFS root filesystem, needed in Step 2 for cross-compilation):

   ```bash
   # On your Linux PC:
   nc -l -p 9006 > backups/bak_stock.bin
   ```

   ```sh
   # On the camera:
   dd if=/dev/mtd6 bs=4096 | nc <YOUR_PC_IP> 9006
   ```

---

## Step 2: Extract the Camera Rootfs (glibc 2.13 + ALSA)

Because the camera runs Linux 2.6.38.8 with **glibc 2.13**, binaries linked against a modern host toolchain's glibc will fail to start on the camera. Instead, the Makefiles link directly against the camera's own `libc-2.13.so`, `ld-2.13.so`, and `libasound.so.2.0.0`.

### Option A: Extract from your `bak_stock.bin` backup (recommended)

Install `ubi_reader` in a local Python venv and extract `backups/bak_stock.bin`:

```bash
python3 -m venv workspace/ubi-venv
workspace/ubi-venv/bin/pip install ubi_reader
workspace/ubi-venv/bin/ubireader_extract_files -o workspace/bak_extracted backups/bak_stock.bin
```

Find the extracted rootfs path (the numeric UBI image ID varies per unit):

```bash
ROOTFS="$(echo "$PWD"/workspace/bak_extracted/*/rootfs)"
ls -l "$ROOTFS/lib/libc-2.13.so" "$ROOTFS/lib/ld-2.13.so" "$ROOTFS/usr/lib/libasound.so.2.0.0"
```

### Option B: Copy only the required shared libraries directly from the camera

If you do not want to unpack the entire UBIFS image, create a minimal sysroot containing just the libraries needed for linking:

```bash
mkdir -p workspace/rootfs/lib workspace/rootfs/usr/lib
```

Copy these files from the camera into `workspace/rootfs/` (for example via `nc` or `scp` once Dropbear is installed):
- `/lib/ld-2.13.so`
- `/lib/libc-2.13.so`
- `/lib/libcrypt-2.13.so`
- `/lib/libutil-2.13.so`
- `/usr/lib/libasound.so.2.0.0`

Then export `ROOTFS="$PWD/workspace/rootfs"`.

---

## Step 3: Cross-Compile the Binaries

All compiled binaries are gitignored so the repository stays source-only. Build them on your Linux host with `ROOTFS` pointing to the directory from Step 2.

### 1. Build `nest_rtsp`

```bash
make -C nest_rtsp ROOTFS="$ROOTFS"
```

Produces `nest_rtsp/nest_rtsp` (~66 KB stripped ARMv6 binary).

### 2. Build `nest_onvif`

```bash
make -C nest_onvif ROOTFS="$ROOTFS"
```

Produces `nest_onvif/nest_onvif` (~66 KB stripped ARMv6 binary).

### 3. Build minimal Dropbear SSH (`dropbear`)

The `dropbear/` directory includes:
- `dropbear/localoptions.h`: strips legacy crypto and unused features, leaving only Curve25519 + Ed25519 + ChaCha20-Poly1305 (~130 KB binary) and storing the host key at `/mnt/dropcam/dropbear_ed25519_host_key`.
- `dropbear/glibc213_compat.c`: shims modern glibc `stat`/`fstat`/`lstat` and `__isoc23_strtol`/`__isoc23_strtoul` symbols onto glibc 2.13's `__xstat`/`__fxstat`/`__lxstat` and `strtol`/`strtoul`.

Build it with the included helper script:

```bash
./dropbear/build.sh "$ROOTFS"
```

Produces `dropbear/dropbear` (~130 KB stripped ARMv6 binary) and verifies that it requires no glibc version newer than `GLIBC_2.4`.

---

## Step 4: Create Your Device Config Files

Device configuration files are listed in `.gitignore` so personal Wi-Fi keys and camera passwords are never committed. Templates live in `examples/`.

1. **ONVIF and RTSP credentials (`onvif.conf`)**

   Copy the template and set a username and strong password on a single line (`username:password`):

   ```bash
   cp examples/onvif.conf.example onvif.conf
   chmod 600 onvif.conf
   # Edit onvif.conf -> e.g.:
   # admin:change-me-to-a-strong-password
   ```

   Both `nest_onvif` and `nest_rtsp` read `/mnt/dropcam/onvif.conf`. If the username is `admin`, `nest_onvif` also accepts `root` with the same password.

2. **Optional audio mode (`audio.mode`)**

   By default, audio is `off`. To enable the microphone or two-way speaker, copy `examples/audio.mode.example` to `audio.mode` and set it to one of:
   - `off` (default)
   - `mic` (microphone capture only)
   - `speaker` (ONVIF backchannel speaker only; requires `onvif.conf` credentials)
   - `two-way` (both mic and speaker)

   Only enable `speaker` or `two-way` after verifying audio behavior on your installed unit (see [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)).

3. **Wi-Fi config (`wpa_supplicant.conf`)**

   `Dropcam-Rooter` already wrote `/mnt/dropcam/wpa_supplicant.conf` on the camera during Step 1. If you ever need to recreate or change your Wi-Fi network, see `examples/wpa_supplicant.conf.example`.

4. **Check camera orientation in `local_start.sh`**

   By default, `local_start.sh` passes `-m 3` (180-degree flip: horizontal + vertical mirror) for an upside-down mount. If your camera is mounted right-side up, change `-m 3` to `-m 0` in `local_start.sh` before deploying:

   | `-m` value | Orientation |
   | --- | --- |
   | `0` | Normal (upright mount) |
   | `1` | Horizontal mirror |
   | `2` | Vertical flip |
   | `3` | 180-degree rotation (upside-down mount) |

---

## Step 5: Deploy to `/mnt/dropcam` and Reboot

### First-time deployment (when only Telnet is running)

On a freshly rooted camera, Dropbear SSH is not installed yet, so transfer the initial files over `nc` or `Dropcam-Rooter/rooted_toolkit.sh`, or install `dropbear` + `ie_auto.sh` first so every future update uses SSH.

**Quick bootstrap via netcat (one tarball over LAN):**

On your Linux PC, pack the files and serve them on port `9099`:

```bash
tar -cf - \
  ie_auto.sh local_start.sh onvif.conf \
  -C "$PWD/dropbear" dropbear \
  -C "$PWD/nest_rtsp" nest_rtsp \
  -C "$PWD/nest_onvif" nest_onvif \
  | nc -l -p 9099
```

In the camera's telnet shell, extract directly into `/mnt/dropcam`, fix permissions, and reboot:

```sh
cd /mnt/dropcam
nc <YOUR_PC_IP> 9099 | tar -xf -
chmod 755 /mnt/dropcam/ie_auto.sh /mnt/dropcam/local_start.sh \
          /mnt/dropcam/dropbear /mnt/dropcam/nest_rtsp /mnt/dropcam/nest_onvif
chmod 600 /mnt/dropcam/onvif.conf
sync
reboot
```

If you created an `audio.mode` file, include `audio.mode` in the `tar` command too (or write it directly with `echo mic > /mnt/dropcam/audio.mode`).

### Subsequent deployments (over Dropbear SSH)

Once Dropbear is running on the camera, use the included helper script:

```bash
./scripts/deploy.sh root@<CAMERA_IP>
```

`scripts/deploy.sh` uploads `ie_auto.sh`, `local_start.sh`, `nest_rtsp/nest_rtsp`, `nest_onvif/nest_onvif`, and `dropbear/dropbear` (plus local `onvif.conf` and `audio.mode` if present), sets permissions atomically, and prints instructions to restart services or reboot.

### Verify Everything Is Running

Wait ~25 seconds after reboot, then test SSH, snapshot, and RTSP from your PC:

```bash
# 1. Verify Dropbear SSH works (and port 23 telnet is now closed on Wi-Fi)
ssh root@<CAMERA_IP> 'ps | grep -E "dropbear|nest_rtsp|nest_onvif"'

# 2. Fetch a live JPEG snapshot
curl -u admin:<PASSWORD> --digest -o test_snap.jpg "http://<CAMERA_IP>/snapshot.jpg"

# 3. Play the live RTSP stream
ffplay -rtsp_transport tcp "rtsp://admin:<PASSWORD>@<CAMERA_IP>:554/stream1"
```

---

## Step 6: Connect to Your NVR

Set a **static DHCP lease** (IP reservation) for your camera's Wi-Fi MAC address in your router before adopting it into an NVR.

| Service | Endpoint |
| --- | --- |
| RTSP main stream | `rtsp://<user>:<pass>@<CAMERA_IP>:554/stream1` |
| Live JPEG snapshot | `http://<CAMERA_IP>/snapshot.jpg` (HTTP Digest or Basic auth) |
| ONVIF Device Service | `http://<CAMERA_IP>/onvif/device_service` |
| ONVIF Events Service | `http://<CAMERA_IP>/onvif/events_service` |
| WS-Discovery | UDP multicast `239.255.255.250:3702` |

### UniFi Protect (Quick Summary)

1. In UniFi Protect, go to **Settings -> System -> Advanced** and enable **Discover Third-Party Cameras**.
2. The camera appears automatically via WS-Discovery (or click **Advanced Adoption** and enter `<CAMERA_IP>` on port `80` with your `onvif.conf` username and password).
3. Once adopted, live view, continuous/motion recording (via ONVIF PullPoint events), snapshots, and night vision mode (`Auto` / `Always On` / `Off` via ONVIF Imaging `IrCutFilter`) work natively.

For detailed walkthroughs covering **UniFi Protect**, **Frigate**, **Home Assistant**, and **go2rtc** (including two-way audio backchannel), see [docs/NVR_SETUP.md](docs/NVR_SETUP.md) and the sample configs in [examples/](examples/).

---

## Configuration and Tuning Reference

### `nest_rtsp` CLI Flags

| Flag | Default | Description |
| --- | --- | --- |
| `-w <width>` | `1920` | Video width in pixels |
| `-h <height>` | `1080` | Video height in pixels |
| `-b <bps>` | `3000000` | Target / maximum H.264 bitrate in bits/sec (`local_start.sh` uses `2200000`) |
| `--min-bitrate <bps>` | `0` | Minimum ABR floor in bits/sec (`local_start.sh` uses `600000`) |
| `--no-abr` | off | Disable AIMD adaptive bitrate and keep a fixed target bitrate |
| `-p <port>` | `554` | RTSP TCP listen port |
| `-m <0..3>` | `0` | Sensor mirror/flip (`0` normal, `1` H-flip, `2` V-flip, `3` 180-deg flip) |
| `-y <0..3>` | `2` | Bayer pattern (`2` = GR, verified for `hwver=5` AR0330 daylight color) |
| `-c <path>` | `/mnt/dropcam/onvif.conf` | Path to `user:pass` credentials file |
| `--no-auth` | off | Disable RTSP authentication (not recommended; also disables speaker) |
| `--no-ae` | off | Disable software auto-exposure / AWB loop |
| `--nv <auto\|on\|off>` | `auto` | Initial night vision mode |
| `--mic` | off | Enable 8 kHz G.711 u-law microphone track (`/stream1/track2`) |
| `--speaker` | off | Enable ONVIF audio backchannel speaker track (`/stream1/track3`, requires auth) |

### `nest_onvif` CLI Flags

| Flag | Default | Description |
| --- | --- | --- |
| `-p <port>` | `80` | HTTP / SOAP listen port |
| `-r <port>` | `554` | Advertised RTSP port |
| `-i <iface>` | `wlan0` | Network interface used for IP, MAC, and WS-Discovery |
| `-c <path>` | `/mnt/dropcam/onvif.conf` | Path to `user:pass` credentials file |

### Runtime Control and Status Files

| Path on Camera | Purpose |
| --- | --- |
| `/tmp/night_vision` | Write `auto`, `on`, or `off` to change night vision mode live |
| `/tmp/night_vision.status` | Current night vision state, IR PWM level, and luma info |
| `/tmp/nest_motion.state` | Current motion detector state (`0` or `1`) read by `nest_onvif` |
| `/tmp/nest_audio.capabilities` | Active audio capability flags exported by `nest_rtsp` |
| `/tmp/snapshot.jpg` | Most recent NV12-to-JPEG snapshot (refreshed on `SIGUSR1` or ~5s interval) |
| `/tmp/rtsp.log` | `nest_rtsp` runtime log (auto-rotates to `/tmp/rtsp.log.old`) |
| `/tmp/onvif.log` | `nest_onvif` runtime log |
| `/tmp/local_start.log` | `local_start.sh` startup log |
| `/tmp/udhcpc.log` | Wi-Fi DHCP client log |

### Local Audio Details

The RTSP server uses mono G.711 u-law (`PCMU`, 8 kHz) for both the microphone and the ONVIF audio backchannel. The speaker track requires configured RTSP credentials in `/mnt/dropcam/onvif.conf` and an ONVIF backchannel negotiation. A client such as go2rtc can negotiate both tracks from the same `/stream1` URL.

`local_start.sh` reads `/mnt/dropcam/audio.mode` before launching `nest_rtsp`. Accepted values are `off`, `mic`, `speaker`, and `two-way` (default: `off`).

### Motion Events Details

The camera analyzes its own preview frames on a 48x32 cell grid, compensates for exposure changes, pauses briefly when the IR-cut filter switches, and clears motion after 2.5 seconds without movement. Events are exposed via `/onvif/events_service` (`CreatePullPointSubscription` + `PullMessages`) as both:
- `tns1:RuleEngine/CellMotionDetector/Motion` (`IsMotion`)
- `tns1:VideoSource/MotionAlarm` (`State`)

---

## Troubleshooting and USB Rescue

If Wi-Fi fails or a script edit breaks networking, you can always reach the camera over USB:
- `ie_auto.sh` configures `usb0` at **`192.168.75.2/24`** and binds `telnetd` to `192.168.75.2` (in addition to Dropbear SSH on port `22`).
- Plug the camera into your Linux PC via USB, assign `192.168.75.1/24` to the new host USB Ethernet interface, and connect via `ssh root@192.168.75.2` or `telnet 192.168.75.2`.

See [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) for full recovery steps, common errors, and log diagnostics.

## Credits

- [weegeeday/Dropcam-Rooter](https://github.com/weegeeday/Dropcam-Rooter) for the solderless Ambarella A5s USB rooting workflow.
- [evilwombat/gopro-usb-tools](https://github.com/evilwombat/gopro-usb-tools) for the Ambarella USB bootloader and RAM Linux loader.
- Kris Brosch (Include Security) for pioneering Dropcam hardware reverse engineering.
- [Dropbear SSH](https://matt.ucc.asn.au/dropbear/dropbear.html) for a compact, modern SSH server suitable for embedded ARMv6 targets.
