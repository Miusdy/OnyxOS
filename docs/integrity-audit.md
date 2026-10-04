# 项目完整性审计

日期：2026-10-04。审计基线为远端 `main` 的 `9a4b18c`（已合并阶段四）。范围是仓库可见的本地/远端分支、全部历史 PR、README 与 `docs/`、构建入口、系统调用契约及实际回归；不是对所有可能的内核缺陷作无缺陷保证。

## 结论与分支归属

阶段一、二、三和阶段四所选虚拟内存方向均有主线代码、用户接口和验收入口。已合并 PR 的 head 与 merge commit 都在基线主线的祖先集合中，7 个已合并 PR 均没有缺失提交。PR #4 是唯一关闭未合并项，其内容不是当前路线图的必要组成部分。

| PR | 来源分支 | 内容 | 基线主线状态 |
| --- | --- | --- | --- |
| [#1](https://github.com/Miusdy/OnyxOS/pull/1) | batch4-observability | RSS、waitx、df、命令索引 | 已合并，head `63c182e` |
| [#2](https://github.com/Miusdy/OnyxOS/pull/2) | batch8-cow-fork | 批次 5–8，含调度、分配器、COW | 已合并，head `d1c0229` |
| [#3](https://github.com/Miusdy/OnyxOS/pull/3) | batch9-fmt-gate | 格式和构建修复 | 已合并，head `0b61fa0` |
| [#4](https://github.com/Miusdy/OnyxOS/pull/4) | batch10-portable-agent-os | 独立 Linux Agent OS 实验 | 关闭未合并，head `21245c8` |
| [#5](https://github.com/Miusdy/OnyxOS/pull/5) | batch11-identity-permissions | 身份与文件系统容量 | 已合并，head `05e9624` |
| [#6](https://github.com/Miusdy/OnyxOS/pull/6) | batch11-identity-permissions | inode 权限及访问检查 | 已合并，head `bda43f3` |
| [#7](https://github.com/Miusdy/OnyxOS/pull/7) | complete-stage3 | 特权边界和口令认证 | 已合并，head `7311917` |
| [#8](https://github.com/Miusdy/OnyxOS/pull/8) | codex/stage4-virtual-memory | 匿名/文件私有映射 | 已合并，head `9a644eb` |

审计开始时现存远端开发分支 `complete-stage3`、`codex/stage4-virtual-memory` 相对于 `origin/main` 的独有提交数均为 0。历史计划中的 `batch6-priority-scheduling`、`batch7-kalloc-debug` 分别对应 `e4053d8`、`d52dce8`，随 #2 进入主线；分支名不再存在不代表代码丢失。已删除且从未产生 PR 或可达提交的分支无法由 GitHub 当前记录恢复，本审计不对不可见历史作推断。

## 为什么不补合并 #4

1. 该提交的 [README](https://github.com/Miusdy/OnyxOS/blob/21245c846ecf2ba0f9c6114d539f09b88cbe4e03/portable-agent-os/README.md) 明确说明它与根目录 xv6 内核无关，是另一个便携式 Agent 系统想法，暂放同仓库。
2. [技术路线 §7](https://github.com/Miusdy/OnyxOS/blob/21245c846ecf2ba0f9c6114d539f09b88cbe4e03/portable-agent-os/portable-agent-os.md#7-与你现有-hachileios-的关系-) 明确建议两条线并行、不合并：xv6 用于机制学习，Agent OS 基于 Linux 不可变镜像。
3. 当前路线图、Makefile、系统调用表和 CI 均不依赖 Python Agent、Linux/systemd 或该目录。#4 只新增 `portable-agent-os/` 下 21 个文件、6383 行，没有修改 xv6 构建和内核文件。
4. GitHub 时间线显示作者于 2026-10-02 关闭该 PR，没有合并时间，也没有解释关闭原因的评论或 review。因此只能确认“关闭未合并”，不能认定为意外遗漏。

Git `merge-tree --write-tree origin/main 21245c8` 可以无文本冲突地构造合并树。但这只证明目录不冲突，不能证明它属于主线范围、能在 xv6 中运行或已完成 Linux 镜像交付。其原文也明确：镜像未实际构建/启动，mkosi 配置键未验证，未实际运行 agent。

本次不重开或合并 #4，也不把其中的独立实验接入主线。若之后单独推进该方向，需要另定 Linux 镜像、宿主平台、运行时及安全边界验收。

## #4 的隔离兼容性结果

从准确提交 `21245c846ecf2ba0f9c6114d539f09b88cbe4e03` 导出到临时目录，使用 Python 3.12 与独立虚拟环境中的 PyYAML。测试使用临时状态目录和回环假 provider，没有调用真实模型、安装系统服务或改写宿主系统配置。

| 原始测试 | macOS ARM64 实测 |
| --- | --- |
| `test_cli.py` | 99 项通过、1 项失败：`allowlist allows what is listed` |
| `test_image.py` | 67 项全部通过（临时镜像树/首次启动测试，不等于启动了 Linux 镜像） |
| `test_skeleton.py` | 首项执行前失败，硬编码作者 Windows Python 与源码路径，产生 FileNotFoundError |

白名单失败对应 `test_cli.py:232–236`：规则使用未解析的临时路径，而 `Policy.decide()` 用 `realpath` 后的路径匹配。macOS 的 `/var` 和 `/private/var` 前缀不一致，使应允许的路径被拒绝。`test_skeleton.py:7–8` 的绝对 Windows 路径则不能移植到当前环境。这些问题说明不能引用旧文档的“199 项全过”作为当前兼容性证据；由于 #4 不属于本次主线范围，未在主线导入或重写该独立原型。

## 本次发现并修复的问题

| 问题 | 触发与修复 | 回归证据 |
| --- | --- | --- |
| 非法信号处理器可使内核 panic | `sigaction(SIGTERM, MAXVA)` 在地址校验前进入 `walk()`；把地址上界检查移到页表查找之前 | 新 `sigtest` 在旧实现实测 `panic: walk`；增加 MAXVA 两侧、负地址、不可执行地址拒绝，验证拒绝后原处理器、fork/wait 仍工作 |
| 最大文件最后半页无法映射 | `MAXFILE=266` 个 1 KiB 块，末页只有 2 KiB；先把 length 向上取整再检查字节上限会误拒绝合法请求 | 新 `mmaptest` 在旧实现实测 `FAIL map complete maximum file`；先校验请求字节范围再取整，覆盖整文件、尾页、尾部零填充和超上限拒绝 |
| README 对共享页重复释放检测过度承诺 | 引用计数只能识别零引用再释放，不能辨认某个持有者多次释放自己的引用 | 修正文档，不把引用计数描述成每个持有者的所有权跟踪 |
| README 的历史行为说明过时 | `top` 实际默认持续刷新，调度不保证固定十轮获选，COW 引用计数表不可直接关闭 | 与 `top.c`、`scheduler()`、`kalloc.c` 对照修正说明 |
| 历史计划与现状易混淆 | 批次 5 的旧容量/结构/工具链描述及批次编号缺口容易被理解成当前缺失 | 新增 `docs/README.md`，标明历史计划时效、#4 独立范围及当前契约入口 |
| 文件映射预加载错误语义含糊 | 冷文件映射缓冲区可在实际 I/O 前因缺页失败而返回 -1 | 保留既有教学接口，明确其与通常短读写结果的区别，不宣称 POSIX 完整兼容 |

## 接口与交付核对

- 48 个系统调用号互不重复，与 48 个用户汇编桩、分发表及内核实现逐项相符。
- Makefile 中 44 个用户程序均有源码；11 个 dedicated 程序包含身份、权限、信号、COW、mmap 和组合压力测试。
- 当前权限/账户结构、`FSMAGIC=0x10203041`、3200 块文件系统及 16 个 VMA 与阶段记录相符；本次不改变磁盘格式、结构体 ABI 或系统调用编号。
- 已检查 README 与 `docs/` 的本地 Markdown 目标；历史代码示例和固定行号按开发时点保留，不当作当前接口。
- 基线主线的 [GitHub Actions](https://github.com/Miusdy/OnyxOS/actions/runs/37171038743) 已实际完成 Linux 单核、Linux 三核及 macOS 构建并全部成功。

## 本次验收

本地环境：macOS ARM64、RISC-V GCC 16.2.0、QEMU 11.1.2、Python 3.12.14、clang-format 21.1.8。两个缺陷先在原实现复现失败，再修复；最终验收不使用自动重试隐藏失败。

| 集合 | CPUS=1 | CPUS=3 |
| --- | --- | --- |
| sigtest 连续 10 轮 | 10/10 | 10/10 |
| mmaptest 连续 10 轮（含最大文件） | 10/10 | 10/10 |
| dedicated（11 个程序） | 11/11 | 11/11 |
| 完整 usertests（非 quick） | ALL TESTS PASSED，138.973 秒 | ALL TESTS PASSED，138.902 秒 |
| crash：日志、孤儿文件、孤儿目录 | 全部通过 | 全部通过 |
| 登录、改密、重启持久性和无效镜像 | 全部通过 | 全部通过 |

宿主回归 21 项、18 组 PBKDF2 参考向量、全树 clang-format 21 检查和 `git diff --check` 均通过。最终 QEMU 测试链退出码为 0，测试进程已回收。

复现失败记录为 `1791096541947558000-summary.json`（sigaction）和 `1791096496972299000-summary.json`（mmap）；最终完整 usertests 汇总为 `1791096725877948000-summary.json`（单核）、`1791096870129770000-summary.json`（三核）。记录位于隔离工作树的 `test-results/`，不进入版本控制。

合并校验流程要求修复分支的 Linux 单核/三核及 macOS CI 通过，再合并主线；合并后另核验主线准确提交的完整 CI，不把分支通过等同于合并后通过。远端结果保存在 [GitHub Actions](https://github.com/Miusdy/OnyxOS/actions/workflows/test.yml)，可按本审计对应 PR 与合并提交查询。

## 合并后复核（2026-10-04）

复核基线为上游 `main` 的 `bfafc0f991b1a79af93ddaea84a86169b3388508`。在初次审计之后，以下三个 PR 已合并；此前的阶段四实现和本次内核修复均包含在该主线中。

| PR | 合并提交 | 结果 |
| --- | --- | --- |
| [#9](https://github.com/Miusdy/OnyxOS/pull/9) | `43319ea` | 信号地址和最大文件映射边界修复、回归用例及审计文档；[合并后 CI](https://github.com/Miusdy/OnyxOS/actions/runs/37184805775) 三项全部成功 |
| [#10](https://github.com/Miusdy/OnyxOS/pull/10) | `2c94116` | 批次说明移入 `docs/batches.md`；与迁移前逐项核对，原正文保留 |
| [#11](https://github.com/Miusdy/OnyxOS/pull/11) | `bfafc0f` | 新增开发环境约定 `AGENTS.md`；未改动内核、用户程序、构建和测试代码 |

截至该基线共 11 个 PR：10 个已合并，#4 关闭未合并，没有待合并 PR。逐项检查已合并 PR 的 head 和 merge commit，全部是当前主线的祖先。

同时通过 GitHub compare 对照上游与 fork `Ella-101/HachileiOS` 的全部现存分支：上游 3 个开发分支、fork 的 `batch4`、`batch6`、`batch7`、`batch8`、`batch9`、`batch11`、`batch12` 及 fork `main`，相对于该基线的 `ahead_by` 均为 0。唯一例外仍是 `batch10-portable-agent-os`，独有 1 个提交 `21245c8`，与上述 #4 判定一致。fork 的旧 `main` 落后不构成主线内容缺失。

主线准确提交 `bfafc0f` 的 [GitHub Actions](https://github.com/Miusdy/OnyxOS/actions/runs/37192830284) 已全部完成并成功：Linux 单核、三核分别通过格式、宿主错误路径、PBKDF2、构建、阶段三认证、11 个专用程序、完整 usertests 和三类崩溃恢复测试；macOS 通过内核与文件系统镜像构建。此结果来自合并后的主线，不是仅引用 PR 分支结果。

文档复核还修正 README 遗留的程序计数：`UPROGS` 实为 44 项，`help` 索引为 43 条，唯一未列入索引的用户程序是 `help` 自身。此次补记与计数修正不改变运行行为或既有测试断言。
