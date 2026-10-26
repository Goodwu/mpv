# TASKS.md

## SOURCE-COLOR-TRACK-20261025

- 状态：完成，用户授权本地提交，不推送；为 PiliPlus 播放诊断条新增独立片源色彩字段。mpv `track-list/N` 新增 `demux-color-primaries/transfer/matrix/range`，来源为解复用保留的原始 codec 参数；未知字段省略，解码输出属性保持原语义。标准源码整包VERIFIED（96d5b558…434571b）部署5台在线机并逐机核哈希；同包libmpv原生CLI在tagged/untagged SDR样本及current-tracks/video alias实测准确/缺失省略，库与UI定向测试和V1通过。唯一跨仓context：`/Users/wuweiwei1/src/PiliPlus/archives/conversations/playback-diag-bar-20261022.md`。
- 本仓改动：`player/command.c` 与 `DOCS/man/input.rst`。使用 FFmpeg 原始名称（如 `bt2020/arib-std-b67/bt2020nc/tv`），严格省略 unknown/unspecified/reserved；不使用会将编码传递特性转换为 EOTF 的 libplacebo 映射，也不以非 lavf 的显示抽象色彩猜片源值。NDK `-fsyntax-only` 与 `git diff --check` 通过。media-kit已接入，Lead统一标准源码构建与设备/原生属性验收结果见首条；本仓实施者仅做源码与语法检查。


## VIDEO-STARTUP-NATIVE-BATCH-20261022

- 状态：用户于 2026-10-27 决定按完成状态关闭。本次关闭代表用户结束该任务，不代表历史“点击至实际视频显示少于 2 秒”指标通过；最后保留的观测仍高于该目标，见 PiliPlus context。
- 历史实现：新增可选 fork export `mpv_mediakit_get_property_strings`，在单次 `run_locked` 中按输入顺序使用原 `M_PROPERTY_GET_STRING` 读取并返回 STRING/NONE 节点数组，统一由 `mpv_free_node_contents` 释放。未改上游 API version；media-kit 动态发现符号，旧库保留 scalar fallback。
- 历史边界：核心锁不等于全系统原子快照；当时仍保持上层身份/epoch、decoder头尾、输出与稳定性校验和超时排空；API isolate 与 event pump 继续分离。
- 收口：已移除 mpv 的 `MK_STARTUP_NATIVE` 日志和 media-kit 的 `MEDIA_KIT_STARTUP_TRACE` 输出开关及纯诊断状态，保留 seek、prefetch、HDR 探测与播放功能逻辑。最后的历史观测包括 `proof-clock1` 光学正确帧 2.290s、motion 2.459s，及单次 codec-stats 的 library-start→correct-frame 2.173s、first-packet→correct-frame 1.312s；这些样本未达到或证明稳定低于 2 秒。原计划只读评估 bounded video prefetch 与 FFmpeg 标准重建可行性未执行，随用户关闭一并取消，不再是待办。PNG、cache-range-join 与 stale-callback race 等历史覆盖缺口保留记录，不作为本次用户关闭的阻碍。详见 PiliPlus 唯一 context。
- 唯一context：`/Users/wuweiwei1/src/PiliPlus/archives/conversations/video-startup-latency-20261022.md`。保留本仓既有占位初始化记录。

## ANDROID-YUV-PLACEHOLDER-20261021

- 状态：done；本地提交归档，不推送。
- 修复：Android Surface 在首布局前可能报告 1×1（或非正）占位尺寸；为离屏 FBO 保留有效占位，沿用后续正常 resize，不因初始占位终止 VO。
- 范围：仅 `video/out/opengl/context_android.c` 的占位初始化；未保留 P0 观察、resize 计数和临时诊断。
- 验收：本地源码链构建的最终 APK 已安装 LG；HDR 首启、重进、SDR 与另一 HDR 机器复核通过，用户确认最终包“画面和控件都正常”。这不是独立 native A/B 实验；灰层根因在 App 字幕 Obx，native 改动解决独立初始化时序问题。
- context：[Android YUV 占位初始化](archives/conversations/android-yuv-placeholder.md)。
