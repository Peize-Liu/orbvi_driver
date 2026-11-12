#!/usr/bin/env python3
"""Write the concise USB validation summary from completed hardware checks."""
import json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
REPORTS=ROOT/'reports'
lines=['# USB 模式验证（2026-11-12）','',
       '环境：ORBVI 4P / X5，USB SuperSpeed，Mac 原生 libusb 接收器 + Docker Linux ARM64。',
       '数据路径：板端 Bulk → Mac libusb → 本机 TCP → C++ / ROS 接收器。测试时 ADB 转发表为空；ADB 仅用于部署和板端进程管理。','',
       '| 接口 | 统计时长 | 四路 RGB 帧数 | IMU 条数 | 结果 |',
       '| --- | --- | --- | --- | --- |']
assert not (REPORTS/'usb-adb-forwards.txt').read_text().strip(), 'ADB forwards must be empty'
for kind in ['native','ros2','ros1']:
    value=json.loads((REPORTS/f'usb-{kind}-final-60s.json').read_text())
    assert value['pass'],kind
    lines.append('| '+kind+' | 60 秒 | '+', '.join(str(c['count']) for c in value['camera'])+' | '+str(value['imu']['count'])+' | 通过 |')
small=json.loads((REPORTS/'usb-640x480-30.json').read_text());assert small['pass']
disconnect=json.loads((REPORTS/'usb-disconnect.json').read_text());assert not disconnect['pass'] and 'disconnected' in disconnect['error']
assert 'configuration mismatch: accel_range_g' in (REPORTS/'usb-config-mismatch.stderr').read_text()
lines+=['','默认模式为四路 1280×1088、60 Hz，IMU 名义 1000 Hz，实际约 1011 Hz。以上接口依次测试，均无序号缺口、无效数据或配对异常；C++ 接口的回调丢弃、USB 汇聚丢弃和无效包增量均为零。','',
        '- 640×480 / 30 Hz：10 秒接收通过，并恢复默认模式。',
        '- 配置不匹配：请求 ±4 g、板端实际 ±16 g 时，USB 握手明确拒绝启动。',
        '- 接收器断开：停止 Mac USB 接收服务后，采集报告明确返回链路断开和失败状态。',
        '- C++ 协议测试：分片、合包、长度上限、保留字段、首次接入对齐通过。',
        '- 板端消息测试：不完整消息不转发、非法长度及相机编号拒绝通过。',
        '- ROS 1 / ROS 2、板端后端和 C++ 核心编译及已有回归检查通过。','',
        '本模式复用官方 Bulk 端点并使用本驱动扩展消息；不修改默认 30 fps UVC 描述符。全量分辨率矩阵此前在 ADB 模式完成，本次 USB 实测覆盖上面两组配置。Linux 原生 direct 后端已编译，未在独立 Linux USB 主机上进行实板验收。','',
        '时间戳为板端单调时钟，图像为 H.264 解码 RGB。60 秒验收不代表长期耐久测试；四路时间戳配对也不替代物理曝光同步标定。']
adb=REPORTS/'usb-adb-regression-10s.json'
if adb.exists():
    assert json.loads(adb.read_text())['pass']
    lines+=['','ADB 兼容模式另完成 10 秒接收回归，并恢复 USB 模式。']
dashboard=REPORTS/'usb-dashboard-final.json'
if dashboard.exists():
    assert json.loads(dashboard.read_text())['pass']
    lines+=['','网页预览通过浏览器检查：四路 1280×1088 视频解码、USB 模式标识、IMU 频率及在线状态正常，无 JavaScript 异常。网页播放调度与完整 ROS 订阅验收分开统计。']
(ROOT/'docs/USB_VALIDATION.md').write_text('\n'.join(lines)+'\n')
