# HachileiOS

> 一个在 xv6-riscv 之上逐步演进的 miniOS —— 以学习操作系统原理为目标。

---

## 项目定位

本项目以 MIT 6.1810 教学操作系统 **xv6-riscv**（`riscv` 分支，基线 HEAD `9e3161a`）为起点，逐步演进为一个 "miniOS"。目标不是做一个"看起来像操作系统"的演示，而是以**可快速追加、可独立验证**的小步前进方式，补全操作系统概念（进程、内存、锁、日志、CPU 统计），并在每一步都保留真实的工程权衡。

### 当前进度（2026-10-04）

已完成批次 1–8，以及 miniOS 路线图的**阶段一：自动化验证**和**阶段二：信号与终端作业控制**。阶段二新增最小信号接口、进程组、Ctrl-C/Ctrl-Z 前台作业控制、Shell 的 `jobs`/`fg`/`bg`，以及 `sigtest`。

批次 11 已实现**阶段三：用户身份与权限**：UID/GID、文件与目录权限、跨用户进程控制限制、特权设备/日志操作，以及口令认证登录和 root 改密。验收结果见[阶段三验证记录](docs/stage3-validation.md)。

阶段四已选择**虚拟内存方向**：新增 `mmap`/`munmap`、匿名与文件私有映射、按需加载、映射页 COW 和 `mmaptest`。单核/三核各 10 轮专项、完整 `usertests`、崩溃恢复及账户回归均通过，详见[阶段四验证记录](docs/stage4-validation.md)。

阶段一验收通过：19 项宿主测试、单核/三核各 10 轮共 120 项专用测试、
两种配置的完整 `usertests` 和全部崩溃恢复测试。阶段二单核/三核专用集合、
两种配置的完整 `usertests` 和崩溃恢复也全部通过；`sigtest` 另在单核连续通过
3 轮。详细阶段一结果及环境限制见
[阶段一验证记录](docs/stage1-validation.md)。远端构建和测试状态以
[GitHub Actions](https://github.com/Miusdy/OnyxOS/actions/workflows/test.yml)
为准，本地通过不代表远端 CI 已通过。

历史分支、PR 与文档的核对结果见[项目完整性审计](docs/integrity-audit.md)，文档入口见[开发文档索引](docs/README.md)。PR #4 的 Linux Agent OS 是独立实验，不属于本项目路线图的必需交付。

完整范围见 [miniOS 路线图](docs/minios-roadmap.md)；阶段三的设计、风险清单与分批计划见 [批次 11 计划](docs/batch11-plan.md)。

### 设计原则

- 每次只加一个能独立验证的能力
- 优先复用已有内核设施，避免大规模重写
- 并发/一致性上的权衡必须显式写进注释
- 每批改动后跑完整 `usertests` 回归

---

## 快速开始

**依赖**

- RISC-V GCC/binutils：`riscv64-elf-`、`riscv64-unknown-elf-` 或 `riscv64-linux-gnu-`
- `qemu-system-riscv64` ≥ 7.2、`make`、`bc`；宿主测试需要 Python ≥ 3.11。
- macOS/Homebrew：`brew install riscv64-elf-gcc qemu python@3.12`；用 `python3.12` 执行宿主测试。格式检查与 CI 对齐到 `clang-format` 21；可用 `make fmt CLANG_FORMAT=/path/to/clang-format` 指定可执行文件。

**构建与运行**

```sh
make qemu          # 构建内核 + fs.img 并启动 qemu
make clean         # 清理构建产物
```

默认启动参数由 `Makefile` 给出：`-m 128M -smp 3`（128 MiB 内存，3 个 hart）。

启动后先出现 `login:`。教学镜像预置 `root/root`（UID/GID 0）、`alice/alice`（1001）和 `bob/bob`（1002），斜杠前后分别是用户名和初始口令；**这些是公开的教学口令**。输入口令时不回显，认证成功后才进入 Shell。root 可用 `passwd NAME` 修改任一预置账户口令，Shell 内建 `exit` 退出并回到登录。普通账户可在 `/home/alice`、`/home/bob` 的各自私有目录中写文件。内核测试以认证后的 root 身份运行。

`make clean` 或删除 `fs.img` 后重建会重置账户和文件，旧数据须先备份。`0x10203040` 的旧 inode 镜像会被明确拒绝；同为 `0x10203041` 但没有账户文件的早期镜像会停在登录错误提示，需要备份后重建，不会自动绕过认证。

**新增的用户程序**

在 `$` 提示符下可直接运行：

| 命令 | 作用 |
| --- | --- |
| `ps` | 列出进程，含用户态/内核态 CPU 时间 |
| `free` | 物理内存总量 / 已用 / 空闲 |
| `dmesg` | 回放内核日志环形缓冲区（仅 root） |
| `top [n]` | 持续刷新进程表 + CPU 增量；给出 n 时刷新 n 次后退出 |
| `neofetch` | 一次性系统概览（含在线 hart 数、磁盘 I/O） |
| `cputest [ticks]` | CPU 时间统计的自检程序（默认累计 20 tick） |
| `df` | 文件系统用量（块 / inode） |
| `waitxtest [ticks]` | 子进程 CPU 时间回收的自检程序（默认 5 tick） |
| `help [command]` | 命令索引；列出可用命令并标注是 xv6 原版还是 miniOS 新增 |
| `priotest [ticks]` | 调度优先级与老化的自检程序（默认每核两个进程竞争 60 tick） |
| `kmemtest` | 分配器会计与泄漏的自检程序 |
| `mixstress` | 并发 fork/wait、管道、COW、文件创建/删除的组合压力测试 |
| `testrun program [args...]` | 宿主测试使用的退出状态报告器 |
| `cowtest` | 写时复制 fork 的自检程序（共享代价、写隔离、copyout 与三代共享）|
| `sigtest` | 信号屏蔽、处理器返回、进程组停止/继续自检 |
| `id` | 打印调用者的身份（`uid=0 gid=0`） |
| `chmod mode path` | 改文件权限位；属主或 uid 0 可用 |
| `idtest` | 身份规则的自检程序：fork 继承、exec 保留、降权不可逆 |
| `permtest` | 权限判定的自检程序，以负向用例为主 |
| `privtest` | 跨用户进程控制、特权操作、跨页进程快照和失败路径自检 |
| `mmaptest` | 匿名/文件私有映射、按需加载、COW、解除映射与 OOM 回收自检 |
| `login` | 由 init 启动的口令认证入口；直接执行要求 root |
| `passwd NAME` | root 修改预置账户口令，需两次输入一致 |
| `whoami` | 显示预置账户名，其他 UID 显示数值 |

镜像里共有 **44** 个用户程序（即 `UPROGS` 的 44 项）。不带参数运行 `help` 会按类别列出其中 **43 条**——除 `help` 自身以外的全部命令——并把 miniOS 新增的命令用 `*` 标出；`help ps` 则只显示该命令的用法与补充细节。`help` 本身没有把自己列进索引，但它与其他命令一样只是根目录里的普通程序。Shell 内建 `cd`、`jobs`、`fg PGID`、`bg PGID`、`exit`；其余命令通常通过 exec 运行。

```
$ ps
pid  ppid state  vsz  rss  usr sys prio name
1  0  sleep  16  16  0  0  5  init
2  1  sleep  20  20  0  0  5  sh
3  2  run  20  20  0  0  5  ps
(vsz/rss are KB, usr/sys are timer ticks, 1 tick = 100 ms)
```

默认构建里 `exec()` 与库函数 `sbrk()` 都立即分配内存，所以 `rss` 恰好等于 `vsz`；只有程序显式使用惰性 `sbrk`（`sbrklazy()`，目前仅 `usertests` 的惰性测试用到）时，`vsz` 才会远大于 `rss`。

---

## 已实现特性

### 总览

| 批次 | 特性 | 系统调用或程序 | 状态 |
| --- | --- | --- | --- |
| 1 | 进程表快照 | `psinfo()` (23) → `ps` | ✅ 已验证 |
| 1 | 空闲物理内存 | `freemem()` (24) → `free` | ✅ 已验证 |
| 2 | 内核日志环形缓冲区 | `klog()` (25) → `dmesg` | ✅ 已验证 |
| 2 | 周期刷新视图 | `top` | ✅ 已验证 |
| 2 | 系统概览 | `neofetch` | ✅ 已验证 |
| 3 | 每进程 CPU 时间 | `psinfo` 扩展字段 → `ps` / `top` / `cputest` | ✅ 已验证 |
| 4 | 驻留内存大小 | `psinfo` 扩展字段 → `ps` / `top` | ✅ 已验证 |
| 4 | 子进程 CPU 时间回收 | `waitx()` (26) → `waitxtest` | ✅ 已验证 |
| 4 | 文件系统用量 | `fsinfo()` (27) → `df` | ✅ 已验证 |
| 4 | 命令索引 | `help` | ✅ 已验证 |
| 5 | 数据块口径 | `fsinfo` 扩展字段 → `df` | ✅ 已验证 |
| 5 | 运行时在线 hart 数 | `sysinfo()` (28) → `neofetch` | ✅ 已验证 |
| 5 | 磁盘 I/O 与缓存命中 | `sysinfo()` (28) → `neofetch` | ✅ 已验证 |
| 5 | 按需分页计数 | `sysinfo()` (28) → `neofetch` | ✅ 已验证 |
| 6 | 调度器优先级 | `setprio()` (29) → `ps` / `priotest` | ✅ 已验证 |
| 7 | 页状态表与分配器自检 | `kalloc_stats()` → `sysinfo` (28) → `kmemtest` | ✅ 已验证 |
| 8 | 写时复制 fork | `kref()`/`kfree()` 引用计数 → `cowfault()` → `cowtest` | ✅ 已验证 |
| 11 | 身份字段与身份系统调用 | `getuid()` (40) / `getgid()` (41) / `setuid()` (42) / `setgid()` (43) → `id` / `idtest` | ✅ 已验证 |
| 11 | inode 属主与权限位（**改磁盘格式**） | `dinode` 增 `mode`/`uid`/`gid`，`FSMAGIC` 升级 → `chmod()` (44) / `chown()` (45) → `ls -l` / `chmod` | ✅ 已验证 |
| 11 | 权限检查生效 | `perm_ok()` + 目录搜索检查 → `permtest` | ✅ 已验证 |
| 11 | 特权边界与登录 | 跨 UID 拒绝、root 设备/日志、PBKDF2 登录、改密与持久性 | 见阶段三验收记录 |
| 11 | 镜像扩容 | `FSSIZE` 2000 → 2400 → 3200（程序和 `writebig` 余量） | 见阶段三验收记录 |

---

各批次的原始设计、改动与取舍见 [批次实施记录](docs/batches.md)；本节只保留总览。

## 新增系统调用

| 编号 | 名称 | 用户态原型 | 返回 |
| --- | --- | --- | --- |
| 23 | `SYS_psinfo` | `int psinfo(struct psinfo *buf, int max)` | 写入条目数，或 `-1` |
| 24 | `SYS_freemem` | `uint64 freemem(void)` | 空闲字节数 |
| 25 | `SYS_klog` | `int klog(char *buf, int max, uint64 *seq, uint64 *lost)` | 复制字节数，或 `-1` |
| 26 | `SYS_waitx` | `int waitx(int *status, uint64 *utime, uint64 *ktime)` | 子进程 pid，或 `-1` |
| 27 | `SYS_fsinfo` | `int fsinfo(struct fsstat *st)` | `0`，或 `-1` |
| 28 | `SYS_sysinfo` | `int sysinfo(struct sysinfo *info)` | `0`，或 `-1` |
| 29 | `SYS_setprio` | `int setprio(int pid, int prio)` | `0`，或 `-1` |
| 30 | `SYS_sigaction` | `int sigaction(int sig, void (*handler)(int))` | `0`，或 `-1` |
| 31 | `SYS_sigmask` | `int sigmask(int mask)` | `0` |
| 32 | `SYS_sigreturn` | `int sigreturn(void)` | 恢复被中断上下文 |
| 33 | `SYS_signal` | `int signal(int pid, int sig)` | `0`，或 `-1` |
| 34 | `SYS_killpg` | `int killpg(int pgid, int sig)` | `0`，或 `-1` |
| 35 | `SYS_setpgid` | `int setpgid(int pid, int pgid)` | `0`，或 `-1` |
| 36 | `SYS_getpgid` | `int getpgid(void)` | 当前进程组号 |
| 37 | `SYS_tcsetpgrp` | `int tcsetpgrp(int pgid)` | `0`，或 `-1` |
| 38 | `SYS_waitpg` | `int waitpg(int pgid, int *status)` | 组内直接子进程 pid，停止事件写入状态 `-2`，或 `-1` |
| 39 | `SYS_jobstate` | `int jobstate(int pgid)` | 不存在 `0` / 运行 `1` / 全部停止 `2` |
| 40 | `SYS_getuid` | `int getuid(void)` | 调用者的 uid |
| 41 | `SYS_getgid` | `int getgid(void)` | 调用者的 gid |
| 42 | `SYS_setuid` | `int setuid(int uid)` | `0`，或 `-1`（无权限或越界） |
| 43 | `SYS_setgid` | `int setgid(int gid)` | `0`，或 `-1`（同上） |
| 44 | `SYS_chmod` | `int chmod(const char *path, int mode)` | `0`，或 `-1`（非属主且非 uid 0） |
| 45 | `SYS_chown` | `int chown(const char *path, int uid, int gid)` | `0`，或 `-1`（仅 uid 0） |
| 46 | `SYS_ttyecho` | `int ttyecho(int enabled)` | root 切换口令输入回显，进程退出自动恢复 |
| 47 | `SYS_mmap` | `void *mmap(void *addr, uint64 len, int prot, int flags, int fd, uint64 off)` | 创建按需加载的匿名或文件私有映射 |
| 48 | `SYS_munmap` | `int munmap(void *addr, uint64 len)` | 解除整个或部分映射，失败时返回 -1 |

编号定义在 `kernel/syscall.h`，分发表在 `kernel/syscall.c`，实现在 `kernel/sysproc.c`、`kernel/sysfile.c` 和 `kernel/mmap.c`，用户桩由 `user/usys.pl` 生成。

---

## 文件清单

### 新增

| 文件 | 说明 |
| --- | --- |
| `kernel/psinfo.h` | `struct psinfo` 布局 + `PSTATE_*` / `PRIO_*` 常量，内核/用户共享（`_pad` 在批次 6 改为 `prio`，`sizeof` 不变） |
| `kernel/minios.h` | `MINIOS_MEM_TOTAL` 等共享常量 |
| `kernel/fsstat.h` | `struct fsstat` 布局（含 `nmeta` 元数据块数），内核/用户共享 |
| `kernel/sysinfo.h` | `struct sysinfo` 布局，内核/用户共享 |
| `user/ps.c` | 进程列表 |
| `user/free.c` | 内存用量 |
| `user/dmesg.c` | 内核日志回放 |
| `user/top.c` | 周期刷新视图 + CPU 增量 |
| `user/neofetch.c` | 系统概览 |
| `user/cputest.c` | CPU 时间统计自检 |
| `user/df.c` | 文件系统用量 |
| `user/waitxtest.c` | 子进程 CPU 时间回收自检 |
| `user/help.c` | 命令索引，标注每条命令的来源 |
| `user/priotest.c` | 调度优先级与老化的自检程序 |
| `user/kmemtest.c` | 分配器会计（守恒律）与泄漏的自检程序 |
| `user/cowtest.c` | 写时复制 fork 的自检程序：fork 代价、写隔离、copyout、三代共享 |
| `user/sigtest.c` | 信号屏蔽、用户处理器返回及进程组停止/继续自检 |
| `user/id.c` | 打印调用者的 uid / gid |
| `user/chmod.c` | 改权限位的命令行（八进制，非八进制字符直接拒绝） |
| `user/idtest.c` | 身份规则自检：fork 继承、exec 保留、降权不可逆、越界值被拒 |
| `user/permtest.c` | 权限判定自检，23 条断言，负向用例为主（见"验证"一节） |

### 修改

| 文件 | 改动 |
| --- | --- |
| `kernel/kalloc.c` | `kmem.nfree` O(1) 计数器；`freemem()`、`freemem_walk()`；批次 7 的每页状态表，批次 8 改为**引用计数**并新增 `kref()`/`krefcnt()`/`kshared()`/`kshared_walk()`/`krefs()`；`kalloc_stats()` |
| `kernel/vm.c` | `vm_rss()` 驻留页统计（递归遍历三级页表）；`vmfaults()` 按需分页计数；批次 8 的写时复制 `uvmcopy()`、`cowfault()` 与 `copyout()` 的 COW 分支 |
| `kernel/fs.c` | O(1) 块/inode 计数器；`fscount_scan()`、`fscount_walk()`、`fsinfo()`（含 `nmeta` 推导）；批次 11 的 `perm_ok()`、`namex()` 的目录搜索检查、`namei()`/`nameiparent()` 的身份参数 |
| `kernel/printk.c` | 无锁日志环形缓冲区；`kputc()` tee；`klog_read()` |
| `kernel/proc.h` | `struct proc` 增加 CPU 计数、进程组、信号待处理/屏蔽状态、处理器与恢复帧；增加 `STOPPED` 状态；批次 11 增加 `uid`/`gid` 与 `struct cred` |
| `kernel/proc.c` | 原有进程快照/调度/优先级能力；新增信号投递、组信号、终端前台组、停止/继续和 `waitpg()`；批次 11 的 `ksetuid()`/`ksetgid()` 与 `mycred()` |
| `kernel/trap.c` | CPU 时间计费、COW 缺页及返回用户态前的信号投递 |
| `kernel/console.c` | Ctrl-C/Ctrl-Z 前台组投递；中断控制台读取并清除未提交输入 |
| `kernel/riscv.h` | `PTE_COW`：Sv39 留给软件的 PTE 位 8 |
| `kernel/sysinfo.h` | `struct sysinfo`；批次 8 追加 5 个共享 / COW 字段 |
| `kernel/sysproc.c` | 观测/调度系统调用及信号、进程组、终端作业控制系统调用 |
| `kernel/main.c` | 每个 hart 进入 `scheduler()` 前调用 `cpu_online_inc()` |
| `kernel/bio.c` | `bcache_hits` / `bcache_misses` 计数器与 `bio_stats()` |
| `kernel/virtio_disk.c` | `disk_reads_cnt` / `disk_writes_cnt` 计数器与 `disk_stats()` |
| `kernel/syscall.h` / `.c` | 新系统调用编号与分发表 |
| `kernel/defs.h` | 新函数原型 |
| `user/user.h` / `usys.pl` | 新系统调用声明与桩 |
| `user/sh.c` | 前后台进程组、`jobs`、`fg PGID`、`bg PGID`；Shell 忽略前台信号并恢复提示符 |
| `Makefile` | 批次 1–8 的 12 个 miniOS 用户程序、阶段二的 `sigtest`，以及阶段三的 `id`/`chmod`/`idtest`/`permtest`/`privtest`/`login`/`passwd`/`whoami` |
| `kernel/fs.h` | `struct dinode` 增 `mode`/`uid`/`gid`（`NDIRECT` 12 → 10 以保持 64 字节）；`FSMAGIC`/`FSMAGIC_OLD`；`ACC_R`/`ACC_W`/`ACC_X` |
| `kernel/file.h` | 内存 inode 同步 `mode`/`uid`/`gid` |
| `kernel/stat.h` | `struct stat` 增 `mode`/`uid`/`gid`（用户可见 ABI） |
| `kernel/param.h` | `FSSIZE` 为 3200，保留完整回归的写入余量 |
| `kernel/exec.c` | `kexec()` 在读文件前检查执行位 |
| `kernel/sysfile.c` | `create()` 以调用者身份与 0644/0755 建 inode；`open`/`create`/`link`/`unlink`/`chdir` 的权限检查；`chmod()`/`chown()` |
| `mkfs/mkfs.c` | 初始 inode 的属主与 mode（按主机文件名判断是否程序） |
| `user/ls.c` | `-l` 打印 10 字符权限列、链接数、属主与大小 |
| `user/cputest.c` | 测量窗口下限 `MIN_TICKS`，避免给出与被测对象无关的 FAILED |
| `test-xv6.py` | `dedicated` 集合加入 `idtest`/`permtest`；`FSMAGIC` 从 `kernel/fs.h` 读取而不是写死 |

---

## 设计权衡

| 决策 | 理由 |
| --- | --- |
| 日志环形缓冲区无锁 | `panic()` 必须能在不获取任何锁的情况下输出；字节先写、索引后发（release/acquire）保证读者看到完整字节 |
| 定时器中断不取 `p->lock` | 计数器单写者；ISR 中加锁是纯开销。读者用 relaxed atomic 取单调快照 |
| 空闲内存用 O(1) 计数器而非 O(N) 遍历 | 高频调用（`top` 每轮一次）下 O(N) 不可接受；保留 `freemem_walk()` 作为可对照的参考实现 |
| `copyout` 前先填内核暂存页并释放所有锁 | `copyout` 可能缺页 → `vmfault` → `kalloc` → `kmem.lock`，在持锁期间调用有死锁风险 |
| `klog` 用用户态持有的 in/out 游标 | 内核无需为每个进程保存读取位置；`lost` 明确报告被覆盖的字节数 |
| `top` 刷新次数有界 | xv6 没有信号，用户程序无法被 shell 中断，因此不能无限循环 |
| RSS 走页表树而非逐页 `walk` | 惰性 `sbrk` 下 `sz` 可远大于实际映射量；树遍历成本只正比于真正在的东西 |
| `waitx` 在 `kexit()` 冻结账目 | `freeproc()` 会清零整个槽位，退出后无处可读；冻结同时把退出路径自身的内核开销包含进去 |
| 块/inode 用量用 O(1) 计数器 | 与 `kmem.nfree` 同一思路；`df` 可能被频繁调用。启动时由 `fscount_scan()` 从磁盘真值标定一次 |
| 计数器用 relaxed atomic 而非自旋锁 | 分配点已在各自的 inode/日志锁之内，此处加锁有锁序反转风险且得不到额外精度 |
| 保留 `fscount_walk()` 参考实现 | 与 `freemem_walk()` 同角色：导出、不被生产路径调用，用于对照校验 O(1) 计数器 |

---

## 验证

**自动回归入口**

需要 Python 3、QEMU ≥ 7.2、RISC-V GCC/binutils、GNU make 和 bc。

```sh
python3 test-harness.py                        # 宿主错误路径回归
./test-xv6.py dedicated --cpus 1 --repeat 10   # 连续验收，失败即停
./test-xv6.py dedicated --cpus 3 --repeat 10
./test-xv6.py cowtest --cpus 3                  # 单个专用测试
./test-xv6.py sigtest --cpus 1 --repeat 3      # 信号与进程组专项回归
./test-xv6.py mmaptest --cpus 3 --repeat 10    # 映射、COW 和资源回收专项
./test-xv6.py usertests --cpus 1               # 完整回归
./test-xv6.py usertests --cpus 3
./test-xv6.py crash --cpus 1
./test-xv6.py crash --cpus 3
```

`dedicated` 包含 cowtest、kmemtest、waitxtest、cputest、priotest、idtest、permtest、privtest、mmaptest、mixstress、sigtest。所有 QEMU 测试先认证为 root。

阶段三另有 `python3 test-auth.py`（哈希参考校验）和 `python3 test-stage3.py --cpus 1` / `--cpus 3`（真实登录、退避、改密、双向文件隔离、重启持久性、旧格式拒绝、损坏账户拒绝）。需要 Python ≥ 3.11，macOS 可用 `python3.12`。测试会重建 `fs.img`，必须先备份需要保留的数据；同一 checkout 的 QEMU 测试不得并发执行。
每个专用测试默认超时 120 秒，可用 `--timeout` 调整；完整 usertests 为
600 秒。`--repeat` 表示重复验证，绝不自动重试失败。
每次运行重新生成 fs.img，会覆盖镜像中的手工文件；同一目录不要并行测试。
串口、QEMU 命令、来宾命令、CPU 配置、提交及工作区状态和 JSON 汇总保存到
`test-results/`（可用 `--artifacts` 指定），`test-xv6.out` 保留最近一次串口。
Linux CI 在 1/3 核上分别执行全部测试并上传日志，任务上限 30 分钟；macOS 仅构建。
专用测试同时检查成功标记、退出码和 Shell 返回；异常时关闭 QEMU 进程组。

**手工回归测试**

```sh
make clean && make && make fs.img
```

内核在 `-Wall -Werror` 下零诊断，随后在 qemu 中运行完整 `usertests`：

```sh
$ usertests
...
ALL TESTS PASSED
```

**CPU 统计自检**

```sh
$ cputest
cputest: user-heavy: elapsed 20 ticks, user 20, sys 0
cputest:   user - elapsed = 0
cputest: sys-heavy : elapsed 20 ticks, user 1, sys 19
cputest:   sys = 95% of elapsed
cputest: OK
```

CPU 时间是采样值，与 uptime 存在核间偏斜；自检允许 4 tick 误差，不能保证每次 `user + sys == elapsed`。

**身份与权限自检**

```sh
$ id
uid=0 gid=0
$ ls -l
drwxr-xr-x 2 0 0 48 /
-rw-r--r-- 1 0 0 34 README
-rwxr-xr-x 1 0 0 <size> idtest
$ chmod 600 README ; ls -l README
-rw------- 1 0 0 34 README
$ chmod 8 README
chmod: not an octal mode: 8
$ idtest
idtest: OK
$ permtest
permtest: OK
```

`idtest` 以 root 启动后 fork：子进程降权、父进程守住 root —— 降权不可逆，一个进程没法既降权又验权。它覆盖 fork 继承、exec 保留（fork 一个孙进程去 `exec id`，输出经管道读回后解析）、降权后回不到 root、不能转成别的身份、no-op 形式仍合法、越界值被拒。

`permtest` 有 23 条断言（root 侧 4 条、降权子进程 19 条），**负向用例是重点**：读不到别人的 `0600`、写不进别人的目录、穿不过 `0700` 的目录、列不了 `0711` 的目录（但能穿过去读里面的文件）、执行不了没有 x 位的文件、不能 unlink 或 chmod 别人的文件。同时有正向对照 —— 一个"什么都拒绝"的模型能通过全部负向断言，所以该成功的也写成断言。有两条刻意的语义各占一条：`0004`（属主位决定，不回退到 other）与"root 执行一个 `0644` 文件必须失败"。

手工观察建议：拿旧镜像直接启动，内核会明确报"更早的 inode 格式，请重跑 mkfs"，而不是把它当损坏的镜像。

**文件系统用量**

```sh
$ df
              total  used  free
image  blocks  2000  1328  672
       bytes   2048000  1359872  688128
data   blocks  1953  1281  672
       bytes   1999872  1311744  688128
66% of the image is in use; 47 blocks are metadata
inodes  200 total, 32 used, 168 free
```

上面是早期版本镜像首次启动后的历史示例（当时根目录有 29 个用户程序）。当前 `FSSIZE = 3200`，有 44 个用户程序，实际余量以当前 `mkfs` 输出和 `df` 为准；批次 11 的镜像预算与历史数字不代表当前构建。

两种口径共享同一个 free 值：元数据块全部标记为已用，空闲块不可能落在元数据区，所以"整盘"与"数据块"两种视角下的 free 必定相同——这不是打印错误。

O(1) 计数器的正确性靠对照 O(N) 参考实现来校验：在内核里临时把 `fsinfo()` 改为调用 `fscount_walk()`，在同一次启动后两者必须完全一致（实测两侧都是 `1328` 块 / `32` inode）。这与批次 1 用 `freemem_walk()` 校验 `freemem()` 的做法相同。

**系统信息**

```sh
$ neofetch
   cpus      : 3 online of 8 max
   memory    : 128 MB total, 118000 KB free
   disk      : 412 block reads, 8 writes
   bcache    : 61 hits, 57 misses
   vmfaults  : 0
```

（示意输出，数值随运行状态变化）

`ncpu_online` 由每个 hart 在进入 `scheduler()` 前自增一次，因此它报的是**实际在调度**的 hart 数，而不是编译期上限 `NCPU`；用 `make qemu CPUS=1` 与 `CPUS=4` 各跑一次即可验证读数随之变化。

`disk_stats()` 计的是 `virtio_disk_rw()` 的真实传输次数，`bio_stats()` 计的是 `bget()` 的逻辑访问是否命中，两者的差额正是缓冲缓存省下的 I/O。**这一项没有 O(N) 参考实现可对照**（事件计数不存在可扫描的真值），只能靠同层恒真不变式（`hits + misses == bget()` 调用次数）与行为对照来验证。

**子进程 CPU 时间回收**

```sh
$ waitxtest
waitxtest: reaped child pid 9, status 0
waitxtest: child  saw user 5 sys 0
waitxtest: parent got user 5 sys 0
waitxtest: delta  user 0 sys 0
waitxtest: OK
```

`waitxtest` 让两条**互相独立**的路径读同一进程的两个计数器：子进程用 `psinfo()` 自测并通过管道回报，父进程用 `waitx()` 回收同一进程。两者必须在 `TOLERANCE = 4` tick 内吻合，且**只能单向偏** —— `kexit()` 的冻结晚于子进程的自读时刻，所以父进程可以多一点，绝不可能少。

**调度优先级**

```sh
$ priotest
priotest: urgent (prio 0): user 18 sys 2
priotest: slack  (prio 9): user 2 sys 0
priotest: OK (urgent 18 vs slack 2 ticks)
```

（示意输出，具体数值随运行状态变化）历史单核示例；当前测试按在线核心数创建每核一对进程，比较两组总 CPU 时间，要求每个进程都有进展，不断言固定倍率。

`prio` 列可直接在 `ps` 里观察：新进程都是 `PRIO_DEFAULT`(5)，`setprio` 之后立即改变。若把 `PRIO_LOWEST` 调到很大的值（例如把范围改成 0..99），可以观察到松弛进程的等待轮数随之线性增长 —— 老化比例由范围宽度决定。

**分配器自检**

```sh
$ kmemtest
kmemtest: pages total 32768, live 118, alloc calls 4210
kmemtest: mem total 131072 KB, free 132763 KB
kmemtest: conserved quantity is 133152768 bytes (free 32512 pages + live)
kmemtest: counters agree, 64 pages allocated and returned
kmemtest: round 1: live 118 -> 118 after 5 children
kmemtest: round 2: live 119 -> 119 after 5 children
kmemtest: round 3: live 119 -> 119 after 5 children
kmemtest: OK (conserved, no leak over 3 rounds)
```

（示意输出，具体数值随运行状态变化）第 3 行是守恒量：`mem_free + pages_live × 4096`，它来自两条互不相干的会计路径，因此它保持不变才算两条路径一致。

**双重释放检测的手工验证**：把 `kfree()` 的调用临时改成对同一页释放两次（例如在 `proc_freepagetable()` 之后再加一次 `kfree`），启动后应立刻看到 `panic: kfree: page was not allocated (double free?)`。这一步**不能**放进 `kmemtest`，因为它会停机。

**手动检查**

- `dmesg` 可回放内核启动日志；执行 `echo hello` 后再 `dmesg`，**不会**包含 `hello` —— 证明 tee 边界正确（用户态 `printf` 不属于内核日志）。实际上 `init: starting sh` 也不出现，因为那是用户程序输出。
- 环形缓冲区溢出路径：临时把 `KLOGSIZE` 改成 16，`dmesg` 会打印 `[dmesg: N bytes of older log were overwritten]`，数值与 `总字节数 − 保留字节数` 精确吻合。
- 后台跑 `cputest 200 &` 时 `top` 显示其 `dcpu` 接近满格、`busy% 100`，而 `init` / `sh` 为 0。
- `cowtest` 期望末行 `OK`。三处值得看：`fork cost` 必须远小于 32 页（复制式 fork 会是 32 页以上）；`shared` 在 fork 期间上升、子进程退出后回到 0；`store faults` 与 `needed a copy` 的差值就是"对方已退出、省掉一次拷贝"的次数。
- 批次 8 验证时的实测读数：`fork cost 10 pages for 32 resident pages (36 shared)`；`141 store faults, 72 of them needed a copy`，差值 69 即被"独占页免拷贝"快捷路径省下的拷贝次数；`neofetch` 稳态下 `sharing` 为 0（fork+exec 的必然结果）而 `refs` 持续增长。
- 手动确认 `copyout` 那条路径：临时把 `copyout()` 里的 `PTE_COW` 分支改成 `return -1`，`cowtest` 的第 3 项必须失败（子进程 `read` 报错），因为内核拒绝写共享页而不是取一份私有副本。
- 引用计数为 0 的页再次 `kfree()` 才会得到 `panic: kfree: page is already free (double free?)`。共享页的重复释放可能先错误地减少其他持有者的引用，因此不能用这条诊断承诺完整的所有权检查。

---

## 虚拟内存映射（阶段四）

`mmap(0, length, prot, flags, fd, offset)` 返回内核选择的页对齐地址，失败返回 `MAP_FAILED`；`munmap(addr, length)` 成功返回 0。常量由 `user/user.h` 引入。

```c
char *p = mmap(0, 8192, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
if (p != MAP_FAILED) {
  p[4096] = 'A';  // 首次访问才分配零页
  munmap(p, 8192);
}
```

文件映射用 `MAP_PRIVATE`、可读普通文件 fd 和页对齐偏移，首次访问按页读取；私有写入不回写文件，关闭 fd 或 unlink 后映射仍有效。fork 对已加载可写页使用 COW，exec 成功和 exit 自动释放映射。最后一个文件页的尾部填零，整页位于 EOF 之后的访问失败。

每进程 16 个区域、单次最多 64 MiB，地址区间为 1–2 GiB，映射放在现有堆之后，堆增长不能越过现存映射；无映射时仍支持原有大范围惰性堆。支持只读和读写映射，所有映射不可执行；不支持共享映射、写回、固定地址、`mprotect` 或 ELF 按需执行。解除映射可裁剪或拆分一个区域；中间拆分需要空槽。完整的并发、错误和文件变化语义见[阶段四验证记录](docs/stage4-validation.md)。

## 已知限制

- 教学配置为最多 64 个进程、每进程 16 个文件描述符、`FSSIZE = 3200` 的文件系统（3.125 MiB，数据区 3153 块）；扩大容量需要同时评估日志、缓存和内存。
- **权限模型是刻意的最小集**：没有 setuid/setgid 位、没有 sticky 位、没有补充组与 `/etc/group`、没有文件时间戳与 ACL。
- **降权不可逆**：只有"实际 uid"一个字段，没有 saved-uid，`setuid()` 到非 0 之后无法回到 root。
- **uid 0 绕过读写位，但不绕过执行位**：root 能读写任何文件；而一个 x 位全清的文件（含目录）对所有人不可执行、不可进入，需要用 `chmod` 从父目录改回来。
- **账户范围固定**：root/alice/bob 由 mkfs 预置，root 可改密；未提供账户增删和普通用户自助改密。教学口令公开，不应当作生产认证配置。
- **观测权限**：`psinfo()` 的身份与统计对所有用户可见；`dmesg` 的内核日志仅 root 可读。
- 信号接口是教学用子集，用户处理器需显式调用 `sigreturn()`，每个进程只保存一层处理器上下文，运行期间屏蔽其他可屏蔽信号。
- Shell 作业表最多保存 16 项；`fg`、`bg` 要求显式 PGID。尚无 POSIX 会话、后台终端读的 SIGTTIN 规则或完整终端行规程。
- `top` 无参数时持续刷新，可用 Ctrl-C 终止；`top n` 运行指定轮数后退出。

- **`PTE_COW` 可以在引用计数为 1 时仍然挂着**：子进程退出后父进程的页保持"只读 + COW"标记，下一次写由 `cowfault()` 的快捷路径就地修好、不产生拷贝。因此任何查看 PTE 的代码都不该假设"只读页一定没有 `PTE_COW`"，而 `psinfo()`/`vm_rss()` 这类只读遍历不受影响。
- **`sysinfo()` 多了一次 O(NPAGE) 的扫描**：`pages_shared_ref` 为了与 O(1) 计数器可比，必须在同一次调用里扫完 32 KiB 状态表，且全程持 `kmem.lock`。这是跨校验的代价，由 `neofetch` / `kmemtest` / `cowtest` / `priotest` 四个调用者承担。
- **`neofetch` 的 `sharing` 行在常规 shell 下通常读数为 0**：xv6 的 shell 走 fork + exec，子进程一 exec 就放弃了共享页。它非零的时候意味着**此刻有子进程正共享着父进程的页**；想看它动起来，请跑 `cowtest`。相比之下 `refs` 是累计值，会随每次 fork 单调增长。
- **页引用计数只有 1 字节**：上限 255，`kref()` 里用一次 `panic` 守住回绕。`NPROC = 64` 意味着上限远够用，但这是一个被写死的耦合 —— 若把 `NPROC` 提到 255 以上就必须换宽度。
- **页引用计数表占用 32 KiB 静态内存**，参与 COW 的共享、复制和释放判断，不能作为可选调试设施直接关闭。调整位宽必须容纳允许的最大共享引用数；每页 2 位不足以支持当前最多 64 个进程的共享。
- **`pages_total` 不等于 `kalloc` 管理的页数**：它含从不进入空闲链表的内核映像页，而内核映像占多少页并未导出。任何一致性检查都必须用增量。
- **页释放检查仅检测零引用再释放**：不能追踪共享页每个持有者的所有权。验证 panic 路径需要专门的内核故障注入，常规用户自检覆盖的是引用计数守恒和资源回收。
- **调度器每 tick 做一次 O(NPROC) 全表扫描**：择优与老化合并后仍是一次完整遍历。`NPROC = 64` 下成本可忽略，但它确实比原先"遇到第一个 `RUNNABLE` 就走"更贵。
- **优先级不继承**：`kfork()` 逐字段复制，子进程从 `PRIO_DEFAULT` 开始。若希望子进程继承父进程的优先级，需要显式调用 `setprio`。
- **老化按调度扫描推进**：优先级差影响竞争，实际获选顺序还取决于可运行进程数和多核时序，优先级范围宽度不等于固定的最大等待轮数。
- **进程控制按 UID 授权**：同 UID 或 root 可操作目标，GID 不授予进程控制权。
- **优先级范围刻意压窄（0..9）**：不同于 nice(1) 的 -20..19，窄范围让老化在可观测的时间内生效；代价是"优先级"只能表达一档粗略的差别。
- **`psinfo()` 的成本随进程规模增长**：快照期间会对每个进程持 `p->lock` 遍历页表树，因此映射页很多的进程会让 `psinfo()` 变慢，而 `top` 每轮都要做一次。这里不能改成「先释放锁再遍历」——那样页表可能被并发释放；这是必要权衡，不是疏漏。
- **RSS 是即时快照，且不区分共享页**：`rss` 统计的是快照瞬间程序、堆和 mmap 区域的驻留页，批次 8 的共享页会被计入**每个**映射它的进程 —— 与传统 `top` 一致，但意味着 `ps` 里两个进程的 `rss` 相加并不等于它们实际占用的物理内存。要看真实占用，请用 `sysinfo` 的 `pages_shared`。
- **`struct psinfo` 跨两页快照**：每项 72 字节；内核分配失败和用户拷贝失败均回收临时页。
- **`waitx` 的账目可能少一格 tick**：冻结发生在退出路径末尾，此后到 `sched()` 之间若有定时器中断，那一格不会被任何人记录。这是 tick 级采样精度的固有边界。
- **CPU 时间是采样得到**：内核把 tick 记给被中断的那个进程，因此单次连续占用不足 1 tick（100 ms）的进程可能显示为 0。
- **`uptime()` 与进程计费之间存在偏斜**：`uptime()`（全局 `ticks`）只在 hart 0 上递增，而被统计的进程可能运行在任意 hart 上，因此两者之间存在少量偏斜；`cputest` 的容差 `TOLERANCE = 4` 即为此设置。
- **日志是诊断输出，不是可靠通道**：环形缓冲区在拷贝过程中若发生回绕，输出的尾部可能是新旧文本的混合。

---

## 路线图

已完成批次 1–8 和批次 11 的五个实施批次，身份、文件权限、特权边界与认证登录见[批次 11](docs/batches.md#批次-11--用户身份与权限)及[阶段三验证记录](docs/stage3-validation.md)。

[miniOS 路线图](docs/minios-roadmap.md) 的阶段一至三已实施并通过本地验收，命令与结果见各阶段验证记录。阶段四选择虚拟内存方向，已实现匿名/文件私有映射与按需加载；接口、限制及验收见[阶段四验证记录](docs/stage4-validation.md)。存储、网络、用户线程和内存回收仍是独立候选。

---

## 许可证与致谢

本项目基于 **xv6-riscv**，其版权与许可证文本见 [`LICENSE.xv6`](LICENSE.xv6)：

> The xv6 software is Copyright (c) 2006-2024 Frans Kaashoek, Robert Morris, Russ Cox, Massachusetts Institute of Technology.

xv6 受 John Lions 的 *Commentary on UNIX 6th Edition* 启发，是 MIT 6.1810 的教学操作系统，参见 <https://pdos.csail.mit.edu/6.1810/>。

本项目自身新增的代码以 MIT 许可证发布，见 [`LICENSE`](LICENSE)。
