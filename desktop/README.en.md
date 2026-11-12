# ORBVI Electron Viewer

[English](README.en.md) | [简体中文](README.md)

The current packaging target is **Apple Silicon Mac (macOS arm64)**. Building and packaging require **Node.js 22.12 or newer** and npm; Node.js 24 is recommended. You do not need a connected board to build the application.

## Get the source and install dependencies

```sh
git clone https://github.com/Peize-Liu/orbvi_driver.git
cd orbvi_driver/desktop
node --version
npm ci
```

To install Node.js 24 on a Mac if needed:

```sh
brew install node@24
export PATH="$(brew --prefix node@24)/bin:$PATH"
```

Run the following commands from the repository's `desktop/` directory unless stated otherwise.

## Run in development mode

```sh
npm start
```

## Build and package

```sh
npm run package
```

The generated application is located at this path relative to the repository root:

```text
desktop/dist/ORBVI Viewer-darwin-arm64/ORBVI Viewer.app
```

Open the application or install it in your user's Applications directory:

```sh
open "dist/ORBVI Viewer-darwin-arm64/ORBVI Viewer.app"

# Optional: install, then open it from your user's Applications folder in Finder
mkdir -p "$HOME/Applications"
ditto "dist/ORBVI Viewer-darwin-arm64/ORBVI Viewer.app" "$HOME/Applications/ORBVI Viewer.app"
```

The packaged application runs without Node.js. The generated app is unsigned and intended for local use.

Electron is downloaded during the first build if it is not cached. If the download is slow, use a mirror:

```sh
ELECTRON_MIRROR=https://npmmirror.com/mirrors/electron/ npm run package
```

## Connect to live data

Complete the initial driver build and board deployment using the [driver setup instructions](../README.en.md#installation-and-startup). Then run the following commands from the **repository root**:

```sh
python3 tools/usb_host.py start --config config/orbvi.yaml
python3 tools/control.py start --config config/orbvi.yaml
python3 tools/run_ros.py ros2 --config config/orbvi.yaml --dashboard
```

Do not start these services again if they are already running. The viewer connects to `http://127.0.0.1:18765` by default. Closing the viewer leaves acquisition running. Live viewing requires the USB service, board acquisition and the preview service inside Docker to remain active.

## Options and controls

The current interface uses Chinese labels:

- **Low latency · Latest frame** (`低延迟·最新帧`): the default mode. Decode incoming images immediately and prioritize the latest frame. Discarded preview frames are counted separately from capture gaps.
- **Smooth · 300 ms buffer** (`平滑·300 ms 缓冲`): select this mode using the dropdown above the images.
- Click an image for fullscreen. Use the bottom buttons to pause the display, reconnect or export a JSON status snapshot.
- Set camera frame rate, resolution and IMU parameters in `config/orbvi.yaml`. See [configuration options](../README.en.md#configuration-options) for supported values.
- **Receive → draw** (`接收→绘制`) measures only the viewer's processing time. It excludes exposure, board encoding, USB transfer and display scanout.

To connect to another local preview service port, run this command from `desktop/`:

```sh
ORBVI_DASHBOARD_URL=http://127.0.0.1:18766 npm start
```
