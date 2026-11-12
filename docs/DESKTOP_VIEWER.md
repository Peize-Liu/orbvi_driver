# Desktop viewer

The Electron application uses the existing local dashboard service. Its bundled UI stays available when the service is offline. It proxies only `/api/status` , `/video/live.x5us` and the four `/video/N.mp4` streams to a loopback HTTP origin; the default is `http://127.0.0.1:18765`.

The renderer runs sandboxed with context isolation and without Node.js access. The preload exposes only a JSON save dialog. Navigation, new windows and permission requests are denied. Closing the app leaves the USB bridge, board capture and ROS receiver running.

## Display semantics

- Capture FPS comes from board timestamps in ROS metadata; displayed FPS and displayed dropped frames come from Chromium video callbacks. These are separate measurements.
- The default USB preview reads one multiplexed X5US stream, decodes independent H.264 IDR pictures through WebCodecs with `optimizeForLatency`, and draws the latest available frame on the next animation frame. There is no playback timeline or intentional playout buffer. Each camera retains at most one pending encoded frame, two submitted pictures and one pending decoded frame; replaced preview frames are counted separately from capture gaps. Paused previews keep draining the source without decoding pictures.
- “Receive → draw” starts when the browser has parsed a complete camera packet and ends immediately before its canvas draw call. It includes any local pending-frame time, decoder scheduling and animation-frame wait, but excludes earlier HTTP/TCP/USB transit, exposure, board encoding and final compositor/display scanout. It is not glass-to-glass latency. A zero playback-buffer value does not mean that every layer is buffer-free.
- Selecting smooth mode uses the existing H.264 stream remuxed to fragmented MP4 without re-encoding. It is also the compatibility fallback for ADB or environments without a supported WebCodecs decoder. The four views decode independently and are not locked to the same captured frame.
- Camera skew is the maximum timestamp spread of the dashboard's latest 600 matched groups (within a 1 ms matching window). It is not an exposure calibration or end-to-end latency measurement.
- IMU count, frequency, gaps and error counters come from the full C++ diagnostic stream. The plot uses approximately 50 Hz latest-sample observations and cannot characterize 1000 Hz shock peaks.
- Core diagnostic counters are cumulative for the receiver process. Camera metadata counters are cumulative for the dashboard process. Exports include the last status snapshot, its connection state and independent display counters, without images.
- Smooth mode initially buffers about 300 ms of media. It adjusts playback speed to 1.02× only when media backlog exceeds 500 ms and seeks only for a backlog over 1 second (with a 2-second cooldown). The per-camera buffer value is queued media duration, not end-to-end latency; waiting counts exclude deliberate pauses and seeks.
- Pausing freezes the preview and charts while acquisition and health polling continue. Video buffering remains bounded. Resuming selects current samples in latest mode or seeks near the live edge in smooth mode. A stalled video connection is retried automatically.

## Verification

Build with Node.js 22.12+ and run against the configured 1280×1088, 60 Hz, 1000 Hz live dashboard:

```sh
cd desktop
npm ci
npm run package
# Install Playwright separately, or point PLAYWRIGHT_MODULE at an existing installation.
npm install --no-save --package-lock=false playwright
ELECTRON_EXECUTABLE="$PWD/dist/ORBVI Viewer-darwin-arm64/ORBVI Viewer.app/Contents/MacOS/ORBVI Viewer" node tests/smoke.cjs
```

The test checks four decoded streams, capture rates and diagnostics, sandbox isolation, a 15-second display pause with bounded buffering, resume, native JSON export, service outage/recovery and manual reconnect. It interrupts only a test proxy, leaving acquisition running. Reports and screenshots are saved under ignored `reports/`; they must not be published with the source.

The packaged artifact is a local unsigned Apple Silicon macOS app. Windows and Linux packaging have not been verified.

Verified on 2026-11-12 with the packaged Electron 45.3.1 macOS arm64 app and a live X5 USB source: all smoke checks passed; four capture rates 59.998–60.002 Hz, displayed rates 59.95–60.01 fps, IMU 1010.94 Hz. Core gaps, invalid packets, callback drops, transport drops and IMU pairing errors were zero. Chromium reported 18–21 cumulative display drops per stream during startup/playback; these are not capture losses. The recent matched timestamp spread was 130.334 µs. Capture continued after the app closed. These observations do not establish calibrated sensor accuracy or end-to-end latency.

## Playback profiling

`desktop/tests/playback-profile.cjs` records frame callback gaps, waiting/seeking events, media buffer depth and before/after core diagnostics. It uses `PLAYWRIGHT_MODULE` and `ELECTRON_EXECUTABLE` like the smoke test. Set `PROFILE_OUTPUT` for a local JSON report, `PROFILE_SECONDS` (default 60), and optionally `JITTER_MS` to delay the test proxy's video delivery every 3 seconds without interrupting capture.

During the 2026-11-12 stutter investigation, the original viewer ran for 60 seconds without a frame callback gap over 100 ms, a waiting event or a seek; maximum callback gap was 35.4 ms. Core transport errors and gaps stayed at zero. A later run captured simultaneous camera/IMU stalling with a `USB data timed out` diagnostic. The host bridge continued reporting received USB bytes, and a new container connection could receive data. The original receiver stopped permanently on this transient I/O failure. The underlying interruption trigger remains unconfirmed. The old 350 ms hard-seek threshold was replaced to avoid unnecessary jumps during moderate backlog, and buffer/wait telemetry was added for future diagnosis.

The C++ bridge receiver now retries transient disconnects, socket I/O errors and 5-second read timeouts. Each retry creates a fresh parser and waits for a validated configuration before admitting samples. Queued old samples are counted as drops and discarded; sequence history is retained so interruptions remain visible. Protocol, incompatible configuration, decoder and direct-USB device errors remain fatal. `usb_bridge_recovery` exercises disconnect recovery, idle-timeout recovery, pre-handshake sample rejection, retained gaps and fatal configuration mismatch using a fake loopback bridge, without hardware.

After the change, a 40-second live run with 180 ms video-delivery delays injected every 3 seconds recorded no seeks and no frame callback gap above 100 ms (maximum 34.4 ms). One camera emitted one brief `waiting` event. This is a bounded jitter check, not evidence that all intermittent link interruptions have been eliminated. Recovery tests passed in both Humble and Noetic builds.

## Latest-frame preview validation

Run `desktop/tests/latest-preview.cjs` with the same `PLAYWRIGHT_MODULE` and `ELECTRON_EXECUTABLE` environment variables. It measures four live canvases for 30 seconds, verifies bounded decode queues, pause/resume, a deliberately aborted preview connection, JSON export, both preview modes and reload. Capture error-counter deltas are checked independently; existing filtered-duplicate warnings remain visible. Reports are local and ignored.

Live USB validation on 2026-11-12 passed all latest-preview checks at 1280×1088 and 60 Hz. Across 120 periodic observations over 30 seconds, receive-to-draw time was median 20.4 ms, p95 35.8 ms and maximum 76.4 ms; mean displayed rates per camera were 58.5–59.3 fps. Preview frame replacement is intentional in this mode. Core gaps, invalid packets and transport/callback drops did not increase. One previously filtered duplicate remained in the receiver's cumulative diagnostics. These local drawing timings do not establish end-to-end latency.
