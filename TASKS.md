# TASKS.md

## ANDROID-YUV-PLACEHOLDER-20261021

- 状态：done；本地提交归档，不推送。
- 修复：Android Surface 在首布局前可能报告 1×1（或非正）占位尺寸；为离屏 FBO 保留有效占位，沿用后续正常 resize，不因初始占位终止 VO。
- 范围：仅 `video/out/opengl/context_android.c` 的占位初始化；未保留 P0 观察、resize 计数和临时诊断。
- 验收：本地源码链构建的最终 APK 已安装 LG；HDR 首启、重进、SDR 与另一 HDR 机器复核通过，用户确认最终包“画面和控件都正常”。这不是独立 native A/B 实验；灰层根因在 App 字幕 Obx，native 改动解决独立初始化时序问题。
- context：[Android YUV 占位初始化](archives/conversations/android-yuv-placeholder.md)。
