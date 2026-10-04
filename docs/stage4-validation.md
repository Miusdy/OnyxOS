# 阶段四：虚拟内存方向

日期：2026-10-04。选择路线图中的虚拟内存方向，交付匿名映射、文件私有映射和映射页按需加载。文件系统、网络、用户线程、页面回收/swap 是独立候选，本批未实施。

## 接口与边界

```c
#include "kernel/types.h"
#include "user/user.h"

void *mmap(void *addr, uint64 length, int prot, int flags, int fd, uint64 offset);
int munmap(void *addr, uint64 length);
```

- 系统调用号 47、48；`mmap` 失败返回 `MAP_FAILED`，`munmap` 失败返回 -1、成功返回 0。
- `addr` 必须为 0，地址由内核在 `[1 GiB, 2 GiB)` 区间按 first fit 选择，空洞可复用。`sbrk` 不能越过现存映射的起点；无映射时仍保留原来的大范围惰性堆。选择映射地址时跳过现有堆，堆已超过映射区时创建失败。ELF 加载检查 trapframe 边界。
- 每进程最多 16 个映射区域，每次映射长度为 1 字节至 64 MiB，向上取整到 4096 字节页。页粒度保护意味着最后一页在映射长度之外的字节仍可访问。
- `prot` 支持 `PROT_READ` 或 `PROT_READ | PROT_WRITE`。所有映射不可执行；不支持 `PROT_NONE`、仅写保护、`PROT_EXEC`、`mprotect` 或固定地址映射。
- 匿名映射使用 `MAP_PRIVATE | MAP_ANONYMOUS`、`fd=-1`、`offset=0`。建立映射不分配数据页；首次读写分配零页。
- 文件映射使用 `MAP_PRIVATE`、可读的普通文件描述符和页对齐偏移。拒绝目录、设备、管道和仅写 fd；请求的字节范围不得超过文件系统 `MAXFILE * BSIZE` 的寻址上限，校验后才向上取整到页，因此最大文件最后半页可以映射。可读 fd 允许建立私有可写映射，修改只保留在进程内存中。
- 文件页首次访问时读取对应 inode，不改变 fd 的文件位置。文件末尾所在页的剩余部分填零；整页起点位于 EOF 之后则缺页失败。用户指令访问失败以 -1 状态退出；系统调用拷贝返回失败或短读写；文件缓冲区预加载可能在实际传输前返回 -1（包括请求范围触及 EOF 后冷页的情况），详见下文。
- 已加载的文件页是私有副本，不随之后的文件写入或截断更新；未加载页按缺页当时的文件内容及长度读取。因此不承诺映射是某一时刻的完整文件快照。没有共享页缓存、`MAP_SHARED`、写回或 `msync`。
- `munmap` 的起点必须页对齐，长度向上取整，范围必须完全位于同一现存区域。支持整个区域、首部、尾部和中间拆分；中间拆分需要一个空区域槽，槽不足时保持原映射不变。跨区域、空洞、重复解除或溢出均返回 -1。
- ELF 程序段仍由 `exec` 预先加载；本批的按需文件读取针对 `mmap`，不包含 ELF 按需执行或 swap。

## 生命周期、并发与失败处理

- 每个文件区域持有一个独立文件引用，关闭原 fd 或 unlink 不使映射失效。区域拆分增加引用，完整解除、exec 成功及 exit 释放引用；最后一个引用释放后，已 unlink 的文件可正常回收。
- fork 复制区域描述；已驻留的可写页复用 COW，只读页共享但不可升级为可写。未驻留页在各进程首次访问时独立加载。内核 `copyout` 同样触发 COW。
- fork 逐区域复制页表，只有所有复制成功后才持有文件引用。失败时清除子进程已复制的叶映射，`freeproc` 回收剩余页表；父进程可以继续写入，遗留的独占 COW 标志由已有快捷路径处理。
- exec 构建新地址空间失败时保留旧映射；成功时释放全部旧映射。exit 在进入僵尸状态之前解除映射和文件引用，wait 回收其余页表。
- 仍遵循一个进程只有一个执行线程的模型。区域发布、映射页表插入和解除在 `p->lock` 内完成，以配合其他核上的进程统计读取；不支持共享地址空间线程。返回用户态时 trampoline 的 `sfence.vma` 清除本核旧转换，无需跨核线程 TLB shootdown。
- 文件缺页可以睡眠。用户陷阱先保存 cause/地址，再开启中断，避免 I/O 睡眠期间陷阱寄存器被覆盖。`read`/`write` 和 `wait`/`waitx`/`waitpg` 在获取 inode、管道、控制台或等待锁之前预加载相关文件映射页，避免递归锁和持自旋锁睡眠。其他用户拷贝路径在无锁处加载；未来新增持锁用户拷贝也必须遵循该约定。
- `psinfo.sz` 包含堆/程序大小与映射区域长度，RSS 包含映射区驻留页；`sysinfo.vmfaults` 记录成功按需加载的页。
- 物理页不足时返回错误或终止缺页进程，不 panic；缺页分配失败不发布叶映射。已分配的空中间页表保留至 exec/exit，解除映射立即释放数据页。本批不引入页面回收或 swap。
- I/O 缓冲区预加载可以在实际传输之前失败；已预加载的页留在原映射中供后续访问，数据传输尚未开始。原有普通缓冲区的短读写规则保持不变。

## 验收入口

`mmaptest` 已加入 `test-xv6.py dedicated`，现有 Linux CI 的单核/三核矩阵会自动执行。测试始终通过认证后的 root Shell 运行。

```sh
python3.12 test-harness.py
python3.12 test-auth.py
python3.12 test-xv6.py mmaptest --cpus 1 --repeat 10
python3.12 test-xv6.py mmaptest --cpus 3 --repeat 10
python3.12 test-xv6.py dedicated --cpus 1
python3.12 test-xv6.py dedicated --cpus 3
python3.12 test-xv6.py usertests --cpus 1
python3.12 test-xv6.py usertests --cpus 3
python3.12 test-xv6.py crash --cpus 1
python3.12 test-xv6.py crash --cpus 3
CPUS=1 python3.12 test-stage3.py
CPUS=3 python3.12 test-stage3.py
```

专用用例覆盖：参数与溢出拒绝、映射表耗尽和拆分回滚、零填充与惰性分配、用户/内核 COW 写入、越界/解除后/只读页访问、偏移与 EOF、私有修改不写回、冷文件页作为同 inode I/O 和管道缓冲区、冷 wait 输出、文件拆分偏移及 120 轮文件引用回收、close/unlink 后访问、映射中的 exec 参数、exec/exit 回收、6 个并发进程的映射压力，以及 128 MiB 数据页需求下的 OOM 退出与恢复。

## 本地结果

本地 macOS ARM64，RISC-V GCC 16.2.0、QEMU 11.1.2、Python 3.12.14；clang-format 21.1.8 全树检查及 `git diff --check` 通过，内核与用户程序以 `-Wall -Werror` 构建通过。

| 集合 | CPUS=1 | CPUS=3 |
| --- | --- | --- |
| mmaptest 连续 10 轮，无自动重试 | 10/10 | 10/10 |
| dedicated：11 个程序 | 11/11 | 11/11 |
| 完整 usertests（非 quick） | ALL TESTS PASSED，137.34 秒 | ALL TESTS PASSED，139.305 秒 |
| crash：日志、孤儿文件、孤儿目录 | 全部通过 | 全部通过 |
| 阶段三登录/改密/持久性/无效镜像回归 | 全部通过 | 全部通过 |

宿主错误路径回归 21 项通过，PBKDF2 的 18 组参考向量通过。负向测试中的非法用户访问、OOM、磁盘满和 inode 耗尽诊断是预期输出，以程序退出状态及最终成功标记验收。测试结束后无遗留 QEMU 进程。

完整回归曾发现固定 1 GiB 堆上界破坏原有 `lazy_alloc`；最终实现改为仅阻止堆增长越过现存映射，并重新完成上述全部客户机验收。没有修改或跳过原有 usertests 来适配该限制。

最终专项汇总：`1791079461015535000-summary.json`（单核）、`1791079485758205000-summary.json`（三核）；完整 usertests 汇总：`1791079561228562000-summary.json`（单核）、`1791079703838838000-summary.json`（三核）。原始串口记录、CPU 配置和 JSON 汇总由测试框架保存在 `test-results/`（不入库）。远端 CI 状态以 GitHub Actions 为准，本地验收不代表远端 CI 已通过。

后续完整性审计补充了最大文件（266 KiB）完整映射与最后半页的回归，并修复长度过早取整导致的拒绝；本次复测结果见[项目完整性审计](integrity-audit.md)。
