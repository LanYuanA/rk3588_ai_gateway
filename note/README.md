# RK3588 AI网关项目笔记目录

**文档整理时间：** 2026-05-30 17:15:00
**整理目的：** 整合分散的笔记文档，按类型分类，方便查阅

---

## 📁 文档分类目录

### 一、项目架构与指南（3篇）

| 序号 | 文档名称 | 创建时间 | 说明 |
|------|---------|---------|------|
| 1 | [01_项目指南_PROJECT_GUIDE.md](01_项目指南_PROJECT_GUIDE.md) | 2026-04-19 | 项目整体架构、技术栈、演进记录 |
| 2 | [02_项目架构详解_PROJECT_ARCHITECTURE_DETAILED.md](02_项目架构详解_PROJECT_ARCHITECTURE_DETAILED.md) | 2026-04-22 | 详细架构设计、模块职责 |
| 3 | [03_项目架构概览_PROJECT_ARCHITECTURE_OVERVIEW.md](03_项目架构概览_PROJECT_ARCHITECTURE_OVERVIEW.md) | 2026-04-23 | 架构总览、数据流处理 |

---

### 二、MPP编解码技术（4篇）

| 序号 | 文档名称 | 创建时间 | 说明 |
|------|---------|---------|------|
| 1 | [04_MPP硬件编码详解_MPP_HARDWARE_ENCODING.md](04_MPP硬件编码详解_MPP_HARDWARE_ENCODING.md) | 2026-04-22 | MPP框架、RKVENC2架构、编码流程 |
| 2 | [05_MPP编解码资源分配_MPP_RESOURCE_ALLOCATION.md](05_MPP编解码资源分配_MPP_RESOURCE_ALLOCATION.md) | 2026-04-23 | 资源分配策略、内存管理 |
| 3 | [06_MPP_H265实现指南_MPP_H265_GUIDE.md](06_MPP_H265实现指南_MPP_H265_GUIDE.md) | 2026-05-30 | H265编码解码API详解 |
| 4 | [07_MPP_H265编码器实现_MPP_H265_IMPLEMENTATION.md](07_MPP_H265编码器实现_MPP_H265_IMPLEMENTATION.md) | 2026-05-30 | 编码器封装、集成到推流线程 |

---

### 三、RGA图像处理（4篇）

| 序号 | 文档名称 | 创建时间 | 说明 |
|------|---------|---------|------|
| 1 | [08_RGA问题复盘_RGA_DEBUG_RECORD.md](08_RGA问题复盘_RGA_DEBUG_RECORD.md) | 2026-04-22 | RGA2/RGA3选择、4G内存问题 |
| 2 | [09_RGA优化总结_RGA_OPTIMIZATION.md](09_RGA优化总结_RGA_OPTIMIZATION.md) | 2026-04-23 | RGA性能优化策略 |
| 3 | [10_RGA_YUV转换优化_RGA_YUV_OPTIMIZATION.md](10_RGA_YUV转换优化_RGA_YUV_OPTIMIZATION.md) | 2026-04-23 | YUV格式转换优化 |
| 4 | [11_RGA开发指南_RGA_DEVELOPER_GUIDE.md](11_RGA开发指南_RGA_DEVELOPER_GUIDE.md) | 2026-04-20 | 瑞芯微RGA官方文档 |

---

### 四、NPU推理与调试（2篇）

| 序号 | 文档名称 | 创建时间 | 说明 |
|------|---------|---------|------|
| 1 | [12_NPU调试报告_NPU_DEBUG_REPORT.md](12_NPU调试报告_NPU_DEBUG_REPORT.md) | 2026-04-19 | 多线程掉帧、死锁调试 |
| 2 | [13_优化记录_OPTIMIZATION_RECORD.md](13_优化记录_OPTIMIZATION_RECORD.md) | 2026-04-20 | RK3588整体优化记录 |

---

### 五、调试与优化记录（4篇）

| 序号 | 文档名称 | 创建时间 | 说明 |
|------|---------|---------|------|
| 1 | [14_RTSP_MPP调试记录_RTSP_DEBUG.md](14_RTSP_MPP调试记录_RTSP_DEBUG.md) | 2026-04-20 | RTSP推流调试 |
| 2 | [15_优化总结_OPTIMIZATION_SUMMARY.md](15_优化总结_OPTIMIZATION_SUMMARY.md) | 2026-05-30 | monster分支优化总结 |
| 3 | [16_代码合并分析_MERGE_ANALYSIS.md](16_代码合并分析_MERGE_ANALYSIS.md) | 2026-05-30 | monster分支合并详解 |
| 4 | [17_线程安全分析_THREAD_SAFETY.md](17_线程安全分析_THREAD_SAFETY.md) | 2026-04-23 | 多线程安全问题分析 |

---

### 六、面试与学习笔记（5篇）

| 序号 | 文档名称 | 创建时间 | 说明 |
|------|---------|---------|------|
| 1 | [18_CPP面试题_CPP_INTERVIEW_QA.md](18_CPP面试题_CPP_INTERVIEW_QA.md) | 2026-04-23 | C++与视频处理面试题 |
| 2 | [19_详细实现笔记_IMPLEMENTATION_NOTES.md](19_详细实现笔记_IMPLEMENTATION_NOTES.md) | 2026-04-23 | 代码实现细节 |
| 3 | [20_GStreamer深入学习_GSTREAMER_DEEP_DIVE.md](20_GStreamer深入学习_GSTREAMER_DEEP_DIVE.md) | 2026-04-22 | GStreamer框架学习 |
| 4 | [21_视频输出参数_VIDEO_PARAMETERS.md](21_视频输出参数_VIDEO_PARAMETERS.md) | 2026-04-22 | 视频参数配置 |
| 5 | [22_YOLOv8机制详解_YOLOV8_EXPLAINED.md](22_YOLOv8机制详解_YOLOV8_EXPLAINED.md) | 2026-04-22 | YOLOv8模型原理 |

---

### 七、推流实现（2篇）

| 序号 | 文档名称 | 创建时间 | 说明 |
|------|---------|---------|------|
| 1 | [24_MPP_H265推流接入实现_H265_PUSH_IMPLEMENTATION.md](24_MPP_H265推流接入实现_H265_PUSH_IMPLEMENTATION.md) | 2026-05-30 | MPP H265编码+GStreamer推流完整实现 |
| 2 | [25_非阻塞实时架构优化_REALTIME_NONBLOCK_ARCHITECTURE.md](25_非阻塞实时架构优化_REALTIME_NONBLOCK_ARCHITECTURE.md) | 2026-05-30 | 非阻塞实时推流架构，解决推理延迟问题 |

### 八、性能优化（26篇）

| 序号 | 文档名称 | 创建时间 | 说明 |
|------|---------|---------|------|
| 1 | [26_RGA_DMA内存优化_RGA_DMA_OPTIMIZATION.md](26_RGA_DMA内存优化_RGA_DMA_OPTIMIZATION.md) | 2026-05-30 | RGA DMA内存分配，解决回退CPU问题 |

### 九、问题修复（27篇）

| 序号 | 文档名称 | 创建时间 | 说明 |
|------|---------|---------|------|
| 1 | [27_H265黑屏修复_VPS_SPS_PPS_FIX.md](27_H265黑屏修复_VPS_SPS_PPS_FIX.md) | 2026-05-30 | H265流VLC播放黑屏，缺少VPS/SPS/PPS头部数据 |
| 2 | [28_检测框坐标映射修复_DETECTION_BOX_FIX.md](28_检测框坐标映射修复_DETECTION_BOX_FIX.md) | 2026-05-30 | 检测框不显示，坐标映射错误 |

---

### 八、源码参考（1篇）

| 序号 | 文档名称 | 创建时间 | 说明 |
|------|---------|---------|------|
| 1 | [23_MPP源码_mpp_rkvenc2.c](23_MPP源码_mpp_rkvenc2.c) | 2026-04-20 | MPP编码器内核源码 |

---

## 📊 文档统计

| 类别 | 数量 | 总字数 |
|------|------|--------|
| 项目架构与指南 | 3篇 | 约2.5万字 |
| MPP编解码技术 | 4篇 | 约5万字 |
| RGA图像处理 | 4篇 | 约18万字 |
| NPU推理与调试 | 2篇 | 约1.3万字 |
| 调试与优化记录 | 4篇 | 约3万字 |
| 面试与学习笔记 | 5篇 | 约3万字 |
| 源码参考 | 1篇 | 约6.5万字 |
| **总计** | **23篇** | **约39.3万字** |

---

## 🔍 快速查找指南

### 按技术领域查找

**想了解MPP编解码？**
→ 阅读 [04_MPP硬件编码详解](04_MPP硬件编码详解_MPP_HARDWARE_ENCODING.md) 和 [06_MPP_H265实现指南](06_MPP_H265实现指南_MPP_H265_GUIDE.md)

**想了解RGA图像处理？**
→ 阅读 [08_RGA问题复盘](08_RGA问题复盘_RGA_DEBUG_RECORD.md) 和 [11_RGA开发指南](11_RGA开发指南_RGA_DEVELOPER_GUIDE.md)

**想了解NPU推理？**
→ 阅读 [12_NPU调试报告](12_NPU调试报告_NPU_DEBUG_REPORT.md)

**想了解项目整体架构？**
→ 阅读 [01_项目指南](01_项目指南_PROJECT_GUIDE.md) 和 [02_项目架构详解](02_项目架构详解_PROJECT_ARCHITECTURE_DETAILED.md)

### 按开发阶段查找

**项目初期（架构设计）**
→ 01_项目指南、02_项目架构详解、03_项目架构概览

**开发中期（硬件调试）**
→ 08_RGA问题复盘、12_NPU调试报告、14_RTSP_MPP调试记录

**开发后期（性能优化）**
→ 09_RGA优化总结、13_优化记录、15_优化总结

**最新更新（H265编码）**
→ 06_MPP_H265实现指南、07_MPP_H265编码器实现、16_代码合并分析

---

## 📝 文档维护说明

1. **命名规范：** `序号_中文标题_英文标识.md`
2. **时间戳：** 每个文档头部标注创建时间和修改时间
3. **分类原则：** 按技术领域和开发阶段分类
4. **更新频率：** 每次代码修改后同步更新相关文档

---

**目录维护人：** Claude Code
**最后更新时间：** 2026-05-30 17:15:00
