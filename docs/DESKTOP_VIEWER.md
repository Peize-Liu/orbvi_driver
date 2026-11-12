# Desktop viewer

The Electron application uses the existing local dashboard service. Its bundled UI stays available when the service is offline. It proxies only `/api/status` and the four `/video/N.mp4` streams to a loopback HTTP origin; the default is `http://127.0.0.1:18765`.

The renderer runs sandboxed with context isolation and without Node.js access. The preload exposes only a JSON save dialog. Navigation, new windows and permission requests are denied. Closing the app leaves the USB bridge, board capture and ROS receiver running.

## Display semantics

- Capture FPS comes from board timestamps in ROS metadata; displayed FPS and displayed dropped frames come from Chromium video callbacks. These are separate measurements.
- Video uses the existing H.264 stream, remuxed to fragmented MP4 without re-encoding. The four preview players decode independently; their displayed frames are not locked together.
- Camera skew is the maximum timestamp spread of the dashboard's latest 600 matched groups (within a 1 ms matching window). It is not an exposure calibration or end-to-end latency measurement.
- IMU count, frequency, gaps and error counters come from the full C++ diagnostic stream. The plot uses approximately 50 Hz latest-sample observations and cannot characterize 1000 Hz shock peaks.
- Core diagnostic counters are cumulative for the receiver process. Camera metadata counters are cumulative for the dashboard process. Exports include the last status snapshot, its connection state and independent display counters, without images.
- Pausing freezes the preview and charts while acquisition and health polling continue. Video buffering remains bounded. Resuming seeks near the live edge. A stalled video connection is retried automatically.

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
