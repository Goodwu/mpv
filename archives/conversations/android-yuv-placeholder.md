# Android YUV 占位初始化

## Current State

2026-10-21：保留修复已验收，随本次本地提交归档，不推送。`yuv_diag_ensure_offscreen` 将初始占位尺寸限制到至少 1×1，后续由既有 resize 替换真实尺寸，避免 Surface 先绑定、布局后到时直接退出 VO。代码差异只有该函数，所有临时观察开关和计数日志已回退。

## 依据与边界

- media-kit 的 AndroidVideoController 在 `rect == null` 时可能先写 `android-surface-size=1x1`，native 初始化优先采用该属性；Flutter viewport 不能保证异步 WID 绑定前 rect 已更新。
- 原实现对占位直接返回 false；新实现分配有效占位，未更改真实尺寸下的渲染和 resize 路径。
- 此项是初始化时序修复，不是视频蒙灰根因；真正灰层为 App 字幕 Obx 异常产生的 Release ErrorWidget。
- 未做仅切换此 hunk 的独立实机 A/B，不将联合构建验收表述为单因果实验。

## 联合验证

- 通过 `media-kit/tool/build-android-test.sh --app-dir ...` 从本地 mpv/media-kit/App 源码构建。
- 最终 APK SHA-256：`1686aeb3fd2cba04145f0f6472774d0957b85b122835f0764ecea74d56c3c308`。
- 包内 libmpv SHA-256：`3a3edc4732c92732b9706373afecfe26838f4f0969b4749bb2aa93c199e5085d`。
- media-kit 全量测试 1023 通过、8 跳过；LG 的 HDR 首启/重进、SDR 与另一 HDR 均真实复核通过；用户明确确认最终包画面和控件正常。
- 完整联合上下文：`../../../PiliPlus/archives/conversations/video-gray-getx-20261021.md`（从本文件相对定位）。
