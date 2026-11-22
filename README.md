<img src="assets/icon.svg" width="64" height="64" alt="">

# nest-cam-local

Created in [T3 Code](https://t3.codes).

## Local audio

The Gen 1 Nest Cam Outdoor RTSP server uses mono G.711 μ-law (PCMU, 8 kHz) for the microphone and the ONVIF audio backchannel. The speaker track requires configured RTSP credentials and an ONVIF backchannel request. A client such as go2rtc can negotiate it from the same `/stream1` URL.

The startup script reads `/mnt/dropcam/audio.mode` before starting RTSP. The accepted values are `off`, `mic`, `speaker`, and `two-way`. The default is `off`. Enable the directions that have passed an acoustic test on the installed camera.

## Motion events

The camera analyzes its own preview frames and reports motion start and clear through the authenticated ONVIF Events service at `/onvif/events_service`. Subscribe with `CreatePullPointSubscription` and poll the returned URL with `PullMessages`. Clients can filter for either `tns1:RuleEngine/CellMotionDetector/Motion` (`IsMotion`) or `tns1:VideoSource/MotionAlarm` (`State`). An unfiltered subscription receives both representations of each motion change.

The detector covers the full frame, compensates for changes in exposure, and pauses briefly when the IR-cut filter switches. Motion clears after 2.5 seconds without movement.
