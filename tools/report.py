#!/usr/bin/env python3
"""Render the evidence collected by the hardware and ROS subscription checks."""
import json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
REPORTS=ROOT/'reports'
matrix=json.loads((REPORTS/'camera-matrix.json').read_text())
imu=json.loads((REPORTS/'imu-settings.json').read_text())
sizes=list(dict.fromkeys((r['width'],r['height']) for r in matrix))
lines=['# 实板验证报告（2026-11-12）','',
       '板卡：HS ORBVI 4P V1.0 / 地瓜 X5，USB ADB `328159a5063d4d1102e069ac00120040`。',
       'BSP：`6.1.83-DR-PL5.1_V1.1.2`。本机为 Apple Silicon，Docker Linux ARM64。','',
       '## 分辨率和同步帧率','',
       '每项同时接收四路真实 RGB 图像和 IMU，预热 3 秒，统计 5 秒。检查尺寸、时间戳、帧号连续性、IMU 配对时间差、回调队列丢弃和四路时间戳配对。此扫描是短时功能验收，不是全部组合的长时间耐久测试。','',
       '| 输出尺寸 | 25 Hz | 30 Hz | 40 Hz | 50 Hz | 60 Hz |',
       '| --- | --- | --- | --- | --- | --- |']
for width,height in sizes:
    row=[f'{width}×{height}']
    for fps in [25,30,40,50,60]:
        values=[r for r in matrix if r['width']==width and r['height']==height and r['fps']==fps]
        row.append('通过' if values and values[-1]['pass_test'] else '失败')
    lines.append('| '+' | '.join(row)+' |')
lines+=['',f'共测试 {len(matrix)} 项，通过 {sum(r["pass_test"] for r in matrix)} 项。原始记录：[camera-matrix.json](camera-matrix.json)。',
        '', '高竖向尺寸失败原因：固定旋转之前 VSE 需要输出宽度 1280，超过当前缩放链路的输入宽度 1088；四路均在 `hbn_vnode_set_ochn_attr` 返回 -10。参见 [1088×1280 日志](rejected-1088x1280.log) 和 [720×1280 日志](rejected-720x1280.log)。最终配置校验提前拒绝这两种尺寸。',
        '', '## IMU 可配置能力','',
        '| 参数 | 检查候选值 | BSP 接受值 |', '| --- | --- | --- |']
for key in ['frequency_hz','accel_range_g','gyro_range_dps']:
    rows=[r for r in imu if r['parameter']==key]
    lines.append('| '+key+' | '+', '.join(str(r['value']) for r in rows)+' | '+', '.join(str(r['value']) for r in rows if r['accepted'])+' |')
lines+=['', '以上为独立参数能力校验，其他参数保持默认，共 19 次检查。未向不支持的量程/频率强写寄存器。实际支持组合为 1 组：1000 Hz、±16 g、±2000°/s；读回 scale 和 available 原始值见 [board-capabilities-final.json](board-capabilities-final.json)。',
        '', '## 三种接口的接收验收','',
        '下面的测试依次运行，没有把三个高带宽接口或额外网页视频同时叠加。ROS 验证真实订阅完整 RGB 与 IMU，并逐时间戳匹配各自元数据；不是只统计发布次数。',
        '', '| 接口 | 时长 | 四路图像计数 | IMU 计数 | 结果 |', '| --- | ---: | --- | ---: | --- |']
for kind in ['native','ros2','ros1']:
    path=REPORTS/(kind+'-final-60s.json')
    if not path.exists() or not path.read_text().strip():
        continue
    value=json.loads(path.read_text())
    lines.append('| '+kind+' | 60 s | '+', '.join(str(c['count']) for c in value['camera'])+' | '+str(value['imu']['count'])+' | '+('通过' if value['pass'] else '失败')+' |')
lines+=['', '上述 JSON 文件保留全部计数和失败字段。原生接口采用实测墙钟窗口，因边界和传输到达时间差，计数可能比 3600 多或少 1；ROS 检查采用各流自己的 60 秒板端时间戳窗口。',
        '', '早期 `native-default.json` 按各路相同帧号配对，遇到独立帧号起点偏移而失败；后续改为板端时间戳配对。`native-ready.json` 的 100 ms 接收配对缓存也记录了失败，最终缓存为 1 秒，图像回调仍立即交付。保留这些记录，最终验收以 `*-final-60s.json` 为准。',
        '', '`ros2-before-duplicate-filter-60s.json` 记录一次失败：camera1 重复一帧，图像时间戳、元数据时间戳和帧号共计 3 个连续性异常。板端 `board-before-duplicate-fix.log` 同时记录 `frame id discontinuity 7799 -> 7799`。最终核心过滤相邻且帧号/时间戳完全相同的图像，并通过 `duplicates_filtered` 统计和 ROS diagnostics 显式报告；没有隐藏丢帧或重置。重复过滤边界通过回归测试，最终三接口重新验收。',
        '', '## 构建与功能检查','',
        '- 从随包源码完整重建：C++ 核心、板端后端、ROS 2 Humble、ROS 1 Noetic。',
        '- 配置 schema、未知/重复字段、硬件能力拒绝、资源占用拒绝、读回及恢复、时间戳配对/不匹配检查：两套 Linux ARM64 工具链通过。',
        '- IMU TCP 转发：分片、合包、非法长度、不完整帧、双客户端相同数据和单客户端断开检查通过（4 个测试方法）。',
        '- 接收端配置与板端不一致时拒绝启动：以请求 ±4 g、板端实际 ±16 g 验证。',
        '- 板端启动/停止已在矩阵中多次执行；保留 BSP 服务，没有刷固件。',
        '', '## 解释边界','',
        '传感器名义 IMU 频率为 1000 Hz，实测速率约 1011 Hz。相机时间戳配对精度不等于曝光同步标定精度。图像通过有损 H.264 传输再解码；没有相机内外参或 IMU 噪声/温度标定。ROS 时间戳保留板端单调时钟域，没有映射 UTC。网页独立播放四路视频，不能以肉眼同屏判断同帧同步。',
        '', '三种接口支持独立连接同一板端采集；同时多开时的额外带宽/CPU负载需要按实际应用另测。']
dashboard=REPORTS/'dashboard-final.json'
if dashboard.exists():
    result=json.loads(dashboard.read_text())
    lines+=['', '## 本地预览检查', '',
            '本地网页恢复至统一包的 ROS 2 接口。浏览器自动检查四路视频实际解码尺寸为 1280×1088、相机/IMU 在线、IMU 统计频率在 950–1050 Hz 且无序号缺口、无 JavaScript 异常：'+('通过' if result['pass'] else '失败')+'。',
            '预览最初使用 Python 订阅全部 IMU 元数据，发生订阅掉队（保留 `dashboard-before-telemetry.*`）。最终 IMU 频率与缺口来自 C++ 核心 diagnostics，曲线 best-effort 仅取最新样本；网页不代表全量 ROS 订阅连续性。',
            '原始状态：[dashboard-final.json](dashboard-final.json)，页面截图：[dashboard-final.png](dashboard-final.png)。播放帧率受浏览器刷新及调度影响，不替代上面的 ROS 全量订阅验收。']
(REPORTS/'VALIDATION.md').write_text('\n'.join(lines)+'\n')
