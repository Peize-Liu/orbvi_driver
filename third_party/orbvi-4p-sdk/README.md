# ORBVI 4P SDK

用于在 Linux 电脑上接收 ORBVI 4P 板卡的数据，提供 C++ 接口和示例程序。

## 功能

- 四路相机视频：接收 H.264 RTSP 视频，默认解码为灰度图像（GRAY8），可选彩色 BGR8。
- IMU 数据：接收加速度和角速度。
- 图像保存：接收并保存 NV12 / JPEG 图像。
- CPU 温度：接收板端温度数据。

## 使用文档

板卡部署、SDK 编译、数据接收及 ROS 2 接入，请按 [ORBVI 4P 使用指南](https://huanshizhineng.feishu.cn/wiki/RxH8wzbVgioHgXk5GqEcy67Uned) 操作。

RTSP 的 `make_rtsp_client(config)` 保持灰度输出；使用
`make_rtsp_client(config, PixelFormat::bgr8)` 可接收 BGR8 帧，实际颜色取决于
RTSP 视频源是否包含颜色信息。`FrameView::stride`
是每行字节数，可能包含对齐填充；`data` 的有效期仅限回调期间。
单路示例可传 `orbvi_sdk_rtsp_demo <url> 10 10 bgr8` 检查彩色帧格式。

## 源码目录

- [`include/orbvi_sdk/`](include/orbvi_sdk/)：C++ 接口。
- [`examples/`](examples/)：示例程序。
- [`tests/`](tests/)：测试代码。
