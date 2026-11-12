# 依赖来源

上游基线、来源和 FFmpeg 校验值记录在 `dependencies.json`。`third_party` 中保存本次实际使用的源码快照，包含相机同步帧率和 ARM64 色彩转换的修改。

- `third_party/orbvi-4p-demo-ros`：原始 MIT 许可证保留在其 `LICENSE`。BSP 二进制库和头文件适用该目录 `build_deps/x5/LIBRARIES-LICENSE`、`HEADERS-LICENSE` 等原始条款。
- `third_party/orbvi-4p-sdk`：保持上游文件与声明，不将其他项目的许可证自动套用到此依赖。
- `wrappers/capture_statistics.hpp`：来自本工作区的 `orbvi_4p_bridge` 基线 `999f235d319f61cbfe852fed8cfdccc2632dc595`，用于真实订阅验收。
- ROS 1 镜像中的 FFmpeg 4.4.5 源自官方发布包，构建时核验 SHA256；源码包自带的许可证保持完整。

各依赖保留其原始许可证和声明；本包不改变这些依赖的许可条款。
