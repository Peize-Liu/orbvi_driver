# ORBVI Electron 上位机

当前打包目标为 **Apple Silicon Mac（macOS arm64）**。编译、打包需要 Node.js **22.12 或更高版本**及 npm；推荐 Node.js 24。打包时无需连接开发板。

## 获取源码和安装依赖

```sh
git clone https://github.com/Peize-Liu/orbvi_driver.git
cd orbvi_driver/desktop
node --version
npm ci
```

如需在 Mac 上安装 Node.js 24：

```sh
brew install node@24
export PATH="$(brew --prefix node@24)/bin:$PATH"
```

以下命令均在仓库的 `desktop/` 目录执行。

## 开发运行

```sh
npm start
```

## 编译打包

```sh
npm run package
```

应用生成在：

```text
desktop/dist/ORBVI Viewer-darwin-arm64/ORBVI Viewer.app
```

打开应用或安装到当前用户的应用目录：

```sh
open "dist/ORBVI Viewer-darwin-arm64/ORBVI Viewer.app"

# 可选：安装后，从 Finder 的用户 Applications 目录打开
mkdir -p "$HOME/Applications"
ditto "dist/ORBVI Viewer-darwin-arm64/ORBVI Viewer.app" "$HOME/Applications/ORBVI Viewer.app"
```

打包后的应用可直接打开，运行时不需要 Node.js。当前生成的是本地未签名应用。

首次打包会下载 Electron；下载较慢时可以使用镜像：

```sh
ELECTRON_MIRROR=https://npmmirror.com/mirrors/electron/ npm run package
```

## 接入实时数据

先按[驱动安装与启动说明](../README.md#安装与启动)完成首次构建和板端部署，再在**仓库根目录**执行：

```sh
python3 tools/usb_host.py start --config config/orbvi.yaml
python3 tools/control.py start --config config/orbvi.yaml
python3 tools/run_ros.py ros2 --config config/orbvi.yaml --dashboard
```

以上服务已运行时无需重复启动。上位机默认连接本机 `http://127.0.0.1:18765`；关闭上位机不停止采集。查看实时数据需要 USB 服务、板端采集和 Docker 内的预览服务持续运行。

## 可选设置与操作

- **低延迟·最新帧**：默认模式，收到即解码，优先显示最新画面；预览丢旧帧独立于采集缺帧统计。
- **平滑·300 ms 缓冲**：通过图像区域上方的下拉菜单切换。
- 点击图像可全屏；底部可暂停显示、重新连接、导出 JSON 状态。
- 相机帧率、分辨率及 IMU 设置通过仓库的 `config/orbvi.yaml` 修改，可选值见[驱动配置](../README.md#可选配置)。
- “接收→绘制”仅测量上位机这一段，不包含曝光、板端编码、USB 传输及屏幕扫描时间。

如需连接其他本机预览服务端口，在 `desktop/` 目录运行：

```sh
ORBVI_DASHBOARD_URL=http://127.0.0.1:18766 npm start
```
