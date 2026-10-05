# GPX 组件文档

| 文档 | 说明 |
|------|------|
| [design.md](design.md) | **设计思路**：架构、分层、缓冲策略、线程模型、API、演进路线 |
| [record_handle.md](record_handle.md) | **录制器句柄**：`gpx_record_t` 创建、start/push/stop/release 全流程 |
| [gpx_format.md](gpx_format.md) | **GPX 文件组成**：版本差异、体积估算、XML 结构、与 `gpx_point_t` 映射 |

代码快速上手见上级目录 [../README.md](../README.md)。  
自定义扩展示例：[../example/README.md](../example/README.md)

相关：

- [companion_recording.md](../../../../../docs/ble/companion_recording.md) — 记录与展示分离、App / BLE 方案
- [companion_proto.h](../../include/companion_proto.h) — BLE Companion 协议常量
