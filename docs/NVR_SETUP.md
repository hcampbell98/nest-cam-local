# Connecting nest-cam-local to NVRs and Smart Home Platforms

Before configuring any NVR, assign a **static DHCP lease** (IP reservation) for your camera's Wi-Fi MAC address in your router so its IP address never changes after a reboot.

Check your camera's Wi-Fi MAC address over SSH:

```bash
ssh root@<CAMERA_IP> 'ifconfig wlan0 | grep HWaddr'
```

## Stream and Service URLs

Replace `<CAMERA_USER>`, `<CAMERA_PASS>`, and `<CAMERA_IP>` with the credentials from `/mnt/dropcam/onvif.conf` and your camera's reserved IP:

- **RTSP Video (+ Audio if enabled):** `rtsp://<CAMERA_USER>:<CAMERA_PASS>@<CAMERA_IP>:554/stream1`
- **JPEG Snapshot:** `http://<CAMERA_IP>/snapshot.jpg` (HTTP Digest or Basic auth)
- **ONVIF Device Service:** `http://<CAMERA_IP>/onvif/device_service`
- **ONVIF Media Service:** `http://<CAMERA_IP>/onvif/media_service`
- **ONVIF Imaging Service:** `http://<CAMERA_IP>/onvif/imaging_service`
- **ONVIF Events Service:** `http://<CAMERA_IP>/onvif/events_service`

---

## 1. UniFi Protect

`nest_onvif` and `nest_rtsp` are built specifically to integrate cleanly with UniFi Protect third-party ONVIF camera support.

![Gen 1 Nest Cam Outdoor adopted in UniFi Protect showing 1080p at 30 fps](images/unifi-protect.png)

### Adoption Steps

1. Make sure the camera and your UniFi Protect console (UDM/UNVR/Cloud Key) are on the same VLAN, or that mDNS/WS-Discovery and TCP ports `80` and `554` are permitted between VLANs.
2. In UniFi Protect, open **Settings -> System -> Advanced** and turn on **Discover Third-Party Cameras**.
3. Open **UniFi Devices**:
   - **Auto-discovery (same VLAN):** Wait up to 30 seconds for `NestCamOutdoor` to appear via WS-Discovery, click **Click to Adopt**, and enter your `onvif.conf` username and password.
   - **Manual adoption (different VLAN or firewall):** Click **Advanced Adoption** (or the `+` button), enter `<CAMERA_IP>`, port `80`, and your `onvif.conf` credentials.
4. Confirm the live feed loads at 1080p.

### What Works in UniFi Protect

- **Continuous and Motion-Based Recording:** Protect subscribes to ONVIF PullPoint events (`/onvif/events_service`) and triggers motion detections from the camera's built-in preview motion detector.
- **Live Snapshots and Thumbnails:** Protect fetches `/snapshot.jpg`, which triggers `SIGUSR1` on `nest_rtsp` for a fresh NV12-to-JPEG capture.
- **Night Vision Control:** Changing Night Vision mode in Protect (`Auto`, `Always Enable`, `Always Disable`) calls ONVIF Imaging `SetImagingSettings` (`IrCutFilter`), which updates `/tmp/night_vision` on the camera in real time:
  - `IrCutFilter = AUTO` -> automatic day/night switching
  - `IrCutFilter = OFF` -> IR-cut filter removed, IR LEDs on (forced night vision)
  - `IrCutFilter = ON` -> IR-cut filter engaged, IR LEDs off (forced daylight color)
- **Adaptive Bitrate:** If Wi-Fi signal dips, `nest_rtsp` scales bitrate smoothly between `600 kbps` and `2.2 Mbps` rather than dropping the TCP RTSP connection.

---

## 2. go2rtc (Recommended Relay and Two-Way Audio)

[go2rtc](https://github.com/AlexxIT/go2rtc) is ideal if you want low-latency WebRTC viewing, multi-client fan-out (so the camera only serves one RTSP client), or two-way audio talkback.

See [examples/go2rtc.yaml.example](../examples/go2rtc.yaml.example):

```yaml
streams:
  nest_outdoor:
    - rtsp://admin:YOUR_PASSWORD@192.0.2.50:554/stream1
```

### Two-Way Audio with go2rtc

1. Ensure `/mnt/dropcam/onvif.conf` is configured (the speaker backchannel refuses to start without credentials).
2. Test your camera's microphone and speaker first (see [TROUBLESHOOTING.md](TROUBLESHOOTING.md)), then set `/mnt/dropcam/audio.mode` to `two-way` (or `mic` / `speaker`) and restart `nest_rtsp`:

   ```bash
   ssh root@<CAMERA_IP> 'echo two-way > /mnt/dropcam/audio.mode && killall nest_rtsp'
   ```

3. Use the same `/stream1` URL in `go2rtc.yaml`. go2rtc automatically negotiates the ONVIF backchannel track (`/stream1/track3`, 8 kHz G.711 u-law `PCMU`) alongside video (`/stream1/track1`) and microphone (`/stream1/track2`).

---

## 3. Frigate NVR

Using go2rtc (built into Frigate) as the restream layer keeps load off the camera's single-core ARMv6 CPU and lets Frigate record, detect, and serve live WebRTC views from a single camera connection.

See [examples/frigate.yml.example](../examples/frigate.yml.example) for a complete template:

```yaml
go2rtc:
  streams:
    nest_outdoor:
      - rtsp://admin:YOUR_PASSWORD@192.0.2.50:554/stream1

cameras:
  nest_outdoor:
    ffmpeg:
      inputs:
        - path: rtsp://127.0.0.1:8554/nest_outdoor
          input_args: preset-rtsp-restream
          roles:
            - record
            - detect
    detect:
      enabled: true
      width: 1920
      height: 1080
      fps: 5
    record:
      enabled: true
```

**Tip:** Keep `input_args: preset-rtsp-restream` (TCP transport) so H.264 keyframes and ABR transitions remain artifact-free over Wi-Fi.

---

## 4. Home Assistant

### Option A: Built-in ONVIF Integration (Video + Snapshots + Motion Sensor)

1. In Home Assistant, go to **Settings -> Devices & Services -> Add Integration -> ONVIF**.
2. Let it auto-discover `NestCamOutdoor`, or enter:
   - **Host:** `<CAMERA_IP>`
   - **Port:** `80`
   - **Username / Password:** from `/mnt/dropcam/onvif.conf`
3. Home Assistant creates:
   - A camera entity backed by `rtsp://<CAMERA_IP>:554/stream1` and `http://<CAMERA_IP>/snapshot.jpg`.
   - A binary motion sensor fed by ONVIF PullPoint motion events (`IsMotion` / `MotionAlarm`).

### Option B: Generic Camera Integration (RTSP + Snapshot Only)

1. Go to **Settings -> Devices & Services -> Add Integration -> Generic Camera**.
2. Set:
   - **Still Image URL:** `http://<CAMERA_IP>/snapshot.jpg`
   - **Stream Source URL:** `rtsp://<CAMERA_USER>:<CAMERA_PASS>@<CAMERA_IP>:554/stream1`
   - **RTSP transport protocol:** `TCP`
   - **Authentication:** `digest`
   - **Username / Password:** from `/mnt/dropcam/onvif.conf`

---

## 5. Quick Command-Line Verification

Test RTSP playback with `ffplay` or VLC:

```bash
ffplay -rtsp_transport tcp "rtsp://<CAMERA_USER>:<CAMERA_PASS>@<CAMERA_IP>:554/stream1"
```

Inspect stream codec, resolution, framerate, and audio tracks with `ffprobe`:

```bash
ffprobe -v error -rtsp_transport tcp -show_streams \
  "rtsp://<CAMERA_USER>:<CAMERA_PASS>@<CAMERA_IP>:554/stream1"
```

Fetch a live JPEG snapshot with `curl`:

```bash
curl -f -u "<CAMERA_USER>:<CAMERA_PASS>" --digest \
  "http://<CAMERA_IP>/snapshot.jpg" -o snapshot.jpg
```
