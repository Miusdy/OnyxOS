# HachileiOS

> 基于 xv6-riscv 的教学 miniOS。

本项目以 MIT 6.1810 教学操作系统 **xv6-riscv**（`riscv` 分支，基线 `9e3161a`）为起点，逐批补全操作系统能力：进程与内存观测、调度、写时复制、信号与终端作业控制、用户身份与文件权限、虚拟内存映射。每批只增加一个可独立验证的功能，并在改动后跑完整回归。

- 各能力的实现与验收结论见[开发文档索引](docs/README.md)。
- 开发环境、验证流程与提交要求见 [AGENTS.md](AGENTS.md)。

---

## 文件结构

```
OnyxOS/
├── kernel/             内核源码
├── user/               用户程序与 Shell
├── common/             内核与用户共享的认证代码（PBKDF2 口令哈希）
├── mkfs/               文件系统镜像生成工具
├── docs/               设计文档与阶段验证记录
├── .github/workflows/  Linux / macOS CI
├── Makefile            构建、QEMU 启动、格式化入口
├── AGENTS.md           开发环境、验证与提交要求
├── test-harness.py     宿主错误路径测试
├── test-auth.py        口令哈希参考校验
├── test-stage3.py      登录、退避、文件隔离与重启持久性测试
├── test-xv6.py         QEMU 内核测试驱动（dedicated / usertests / crash）
├── README.md           本文档
├── README              xv6 原始说明，随 fs.img 打包，不是本文档
└── LICENSE / LICENSE.xv6   本项目与 xv6 的许可证
```

### 内核模块（`kernel/`）

| 文件 | 职责 |
| --- | --- |
| `entry.S` / `start.c` / `main.c` | 引导与初始化 |
| `proc.c` / `swtch.S` / `trap.c` | 进程、调度、信号投递与陷阱 |
| `vm.c` / `mmap.c` / `kalloc.c` | 页表、按需分页、映射、物理页分配与引用计数 |
| `fs.c` / `log.c` / `bio.c` / `file.c` / `pipe.c` | 文件系统、日志、块缓存与文件抽象 |
| `exec.c` / `sysfile.c` / `sysproc.c` / `syscall.c` | 程序加载与系统调用 |
| `console.c` / `uart.c` / `printk.c` | 控制台、串口、内核输出与日志环形缓冲区 |
| `virtio_disk.c` / `plic.c` | 磁盘驱动与中断控制器 |
| `spinlock.c` / `sleeplock.c` | 自旋锁与睡眠锁 |
| `syscall.h` / `defs.h` / `vm.h` / `proc.h` | 系统调用编号与函数原型 |
| `psinfo.h` / `fsstat.h` / `sysinfo.h` / `minios.h` / `mman.h` | 内核与用户共享的结构体与常量 |

### 用户程序（`user/`）

- `sh.c`：Shell，内建 `cd`、`jobs`、`fg PGID`、`bg PGID`、`exit`，支持前台/后台进程组。
- miniOS 新增命令与自检程序：见下方[常用命令](#常用命令)。
- 镜像内置 **44** 个用户程序，完整清单见 `Makefile` 的 `UPROGS`。

---

## 快速开始

### 依赖

| 组件 | 要求 |
| --- | --- |
| RISC-V GCC + binutils | 支持 `rv64gc`；`Makefile` 自动探测常见前缀，也可用 `TOOLPREFIX=` 指定 |
| QEMU | `qemu-system-riscv64` ≥ 7.2 |
| 宿主 C 编译器 | `gcc`（构建 mkfs）、`cc`（认证测试，需支持 `-shared -fPIC`）|
| Python | ≥ 3.11，推荐 3.12；测试仅用标准库 |
| 其他 | GNU make、Perl、bc |
| clang-format | 21.x，与项目格式基准一致 |

macOS 可用 `brew install riscv64-elf-gcc qemu python@3.12`。

### 构建与运行

```sh
make qemu          # 构建内核与 fs.img，并启动 QEMU
make clean         # 清理构建产物，会一并删除 fs.img
```

QEMU 默认参数为 `-m 128M -smp 3`，可用 `make qemu CPUS=1` 改变 hart 数。

### 登录与账户

启动后先出现 `login:`。镜像预置三个账户，格式为 `用户名/初始口令`：

| 账户 | 口令 | uid | 家目录 |
| --- | --- | --- | --- |
| `root` | `root` | 0 | `/` |
| `alice` | `alice` | 1001 | `/home/alice` |
| `bob` | `bob` | 1002 | `/home/bob` |

输入口令时不回显，认证成功后才进入 Shell。root 可用 `passwd NAME` 修改任一预置账户口令。内核测试以认证后的 root 身份运行。

### 常用命令

在 `$` 提示符下可直接运行：

| 命令 | 作用 |
| --- | --- |
| `help [command]` | 命令索引；`*` 标出 miniOS 新增命令 |
| `ps` | 进程列表，含 `vsz`/`rss` 与用户态/内核态 CPU 时间 |
| `top [n]` | 周期刷新进程表与 CPU 增量，给出 n 时刷新 n 次后退出 |
| `free` | 物理内存总量 / 已用 / 空闲 |
| `df` | 文件系统用量（块 / inode） |
| `neofetch` | 系统概览（在线 hart 数、磁盘 I/O、缓存命中、缺页） |
| `dmesg` | 回放内核日志环形缓冲区（仅 root） |
| `id` / `whoami` | 查看当前身份 |
| `chmod mode path` | 修改权限位，属主或 uid 0 可用 |
| `passwd NAME` | root 修改预置账户口令 |
| `login` | 口令认证入口，由 `init` 启动，直接执行要求 root |
| `cputest` / `waitxtest` | CPU 时间统计与子进程账目回收自检 |
| `priotest` | 调度优先级与老化自检 |
| `kmemtest` | 分配器会计与泄漏自检 |
| `cowtest` | 写时复制 fork 自检 |
| `sigtest` | 信号屏蔽、处理器返回与进程组停止/继续自检 |
| `idtest` / `permtest` / `privtest` | 身份规则、权限判定与特权边界自检 |
| `mmaptest` | 映射、按需加载、COW 与资源回收自检 |
| `mixstress` | fork/wait、管道、COW、文件操作的组合压力测试 |

---

## 能力概览

| 能力 | 接口 / 命令 |
| --- | --- |
| 进程表快照、CPU 时间、RSS、优先级 | `psinfo()` → `ps` / `top` |
| 空闲物理内存 | `freemem()` → `free` |
| 内核日志环形缓冲区 | `klog()` → `dmesg` |
| 在线 hart 数、磁盘 I/O、缓存命中、缺页 | `sysinfo()` → `neofetch` |
| 文件系统用量 | `fsinfo()` → `df` |
| 子进程 CPU 时间回收 | `waitx()` → `waitxtest` |
| 调度优先级与老化 | `setprio()` → `priotest` |
| 分配器会计与泄漏检查 | `kalloc_stats()` → `kmemtest` |
| 写时复制 fork | 页引用计数 + `cowfault()` → `cowtest` |
| 信号与终端作业控制 | `sigaction` / `sigmask` / `signal` / `killpg` / `setpgid` / `waitpg` 等 → `sigtest`、Shell `jobs`/`fg`/`bg` |
| 用户身份与文件权限 | `getuid` / `getgid` / `setuid` / `setgid` / `chmod` / `chown` → `id` / `idtest` / `permtest` / `privtest` |
| 口令认证登录 | `login` / `passwd` / `whoami` |
| 虚拟内存映射 | `mmap()` / `munmap()` → `mmaptest` |

### 新增系统调用

编号定义在 `kernel/syscall.h`，分发表在 `kernel/syscall.c`，实现在 `kernel/sysproc.c`、`kernel/sysfile.c` 和 `kernel/mmap.c`，用户桩由 `user/usys.pl` 生成。

| 编号 | 名称 | 用户态原型 |
| --- | --- | --- |
| 23 | `SYS_psinfo` | `int psinfo(struct psinfo *buf, int max)` |
| 24 | `SYS_freemem` | `uint64 freemem(void)` |
| 25 | `SYS_klog` | `int klog(char *buf, int max, uint64 *seq, uint64 *lost)` |
| 26 | `SYS_waitx` | `int waitx(int *status, uint64 *utime, uint64 *ktime)` |
| 27 | `SYS_fsinfo` | `int fsinfo(struct fsstat *st)` |
| 28 | `SYS_sysinfo` | `int sysinfo(struct sysinfo *info)` |
| 29 | `SYS_setprio` | `int setprio(int pid, int prio)` |
| 30 | `SYS_sigaction` | `int sigaction(int sig, void (*handler)(int))` |
| 31 | `SYS_sigmask` | `int sigmask(int mask)` |
| 32 | `SYS_sigreturn` | `int sigreturn(void)` |
| 33 | `SYS_signal` | `int signal(int pid, int sig)` |
| 34 | `SYS_killpg` | `int killpg(int pgid, int sig)` |
| 35 | `SYS_setpgid` | `int setpgid(int pid, int pgid)` |
| 36 | `SYS_getpgid` | `int getpgid(void)` |
| 37 | `SYS_tcsetpgrp` | `int tcsetpgrp(int pgid)` |
| 38 | `SYS_waitpg` | `int waitpg(int pgid, int *status)` |
| 39 | `SYS_jobstate` | `int jobstate(int pgid)` |
| 40 | `SYS_getuid` | `int getuid(void)` |
| 41 | `SYS_getgid` | `int getgid(void)` |
| 42 | `SYS_setuid` | `int setuid(int uid)` |
| 43 | `SYS_setgid` | `int setgid(int gid)` |
| 44 | `SYS_chmod` | `int chmod(const char *path, int mode)` |
| 45 | `SYS_chown` | `int chown(const char *path, int uid, int gid)` |
| 46 | `SYS_ttyecho` | `int ttyecho(int enabled)` |
| 47 | `SYS_mmap` | `void *mmap(void *addr, uint64 len, int prot, int flags, int fd, uint64 off)` |
| 48 | `SYS_munmap` | `int munmap(void *addr, uint64 len)` |

### 虚拟内存映射用法

```c
char *p = mmap(0, 8192, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
if (p != MAP_FAILED) {
  p[4096] = 'A';   /* 首次访问才分配零页 */
  munmap(p, 8192);
}
```

文件映射使用 `MAP_PRIVATE`、可读普通文件 fd 与页对齐偏移，首次访问按页读取，私有写入不回写文件。完整语义与限制见[阶段四验证记录](docs/stage4-validation.md)。

---

## 测试

需要 Python 3、QEMU ≥ 7.2、RISC-V GCC/binutils、GNU make 与 bc。

```sh
python3 test-harness.py                        # 宿主错误路径回归
python3 test-auth.py                           # 口令哈希参考校验
python3 test-stage3.py --cpus 1                # 登录、退避、隔离、持久性
python3 test-stage3.py --cpus 3

./test-xv6.py dedicated --cpus 1 --repeat 10   # 专用测试连续验收
./test-xv6.py dedicated --cpus 3 --repeat 10
./test-xv6.py usertests --cpus 1               # 完整回归
./test-xv6.py usertests --cpus 3
./test-xv6.py crash --cpus 1                   # 崩溃恢复
./test-xv6.py crash --cpus 3
```

- `dedicated` 集合：`cowtest`、`kmemtest`、`waitxtest`、`cputest`、`priotest`、`idtest`、`permtest`、`privtest`、`mmaptest`、`mixstress`、`sigtest`。
- 所有 QEMU 测试先认证为 root；每次运行都会重新生成 `fs.img`。
- 每个专用测试默认超时 120 秒，完整 `usertests` 为 600 秒，可用 `--timeout` 调整。
- `--repeat` 只做重复验证，失败不自动重试。
- 日志与 JSON 汇总写入 `test-results/`，最近一次串口输出保留在 `test-xv6.out`。
- 同一 checkout 的 QEMU 测试不能并发执行。
- CI 在 Linux 上以单核/三核各跑一遍完整测试（上限 30 分钟），macOS 仅验证构建。

设计与验收记录：[阶段一](docs/stage1-validation.md)、[阶段三](docs/stage3-validation.md)、[阶段四](docs/stage4-validation.md)、[批次实施记录](docs/batches.md)、[项目完整性审计](docs/integrity-audit.md)、[路线图](docs/minios-roadmap.md)。

---

## 已知限制

- **教学配置容量**：最多 64 个进程、每进程 16 个文件描述符、`FSSIZE = 3200`（3.125 MiB）。扩容需同时评估日志、缓存与内存。
- **权限模型是最小集**：没有 setuid/setgid 位、sticky 位、补充组、ACL 与文件时间戳。
- **降权不可逆**：只有实际 uid 一个字段，没有 saved-uid，`setuid()` 到非 0 后无法回到 root。
- **uid 0 绕过读写位，但不绕过执行位**：root 能读写任何文件；x 位全清的文件（含目录）对所有人不可执行、不可进入。
- **账户范围固定**：root/alice/bob 由 mkfs 预置，不提供账户增删与普通用户自助改密。
- **观测权限**：`psinfo()` 的身份与统计对所有用户可见；内核日志 `dmesg` 仅 root 可读。
- **信号是教学子集**：用户处理器需显式调用 `sigreturn()`，每进程只保存一层处理器上下文，处理期间屏蔽其他可屏蔽信号。
- **Shell 作业表最多 16 项**：`fg`/`bg` 要求显式 PGID；没有 POSIX 会话与 SIGTTIN 规则。
- **mmap 限制**：每进程 16 个区域、单次最多 64 MiB、地址区间 1–2 GiB，不支持共享映射、写回、固定地址、`mprotect` 与可执行映射。
- **CPU 时间与内存是采样值**：单次连续占用不足 1 tick（100 ms）的进程可能显示为 0；`uptime()`（仅 hart 0 递增）与被统计进程之间存在核间偏斜，自检容差为 4 tick。
- **`pages_total` 含内核映像页**：不等于 `kalloc` 管理的页数，一致性检查必须用增量。
- **页引用计数只有 1 字节**：上限 255，与 `NPROC = 64` 耦合；提高 `NPROC` 需同时换宽度。计数表占 32 KiB 静态内存，不能关闭。
- **`psinfo()` 成本随进程规模增长**：快照期间对每个进程持 `p->lock` 遍历页表，映射页多的进程会让 `top` 变慢。
- **调度器每 tick 做一次 O(NPROC) 全表扫描**：优先级不继承，子进程从默认优先级开始，需显式 `setprio`。
- **日志是诊断输出，不是可靠通道**：环形缓冲区在拷贝期间回绕时，输出尾部可能是新旧文本的混合。

---

## 声明

**用途声明**：本项目是教学与学习用途的操作系统实现，不是生产系统。预置账户口令公开且固定，认证与权限实现为教学规模的最小集，不应当作生产安全配置使用。

**数据声明**：`make clean` 或删除 `fs.img` 后重建会重置镜像内的账户与文件。运行测试同样会重新生成 `fs.img`，需要保留的数据必须先备份。

**兼容性声明**：磁盘格式随权限支持升级过 `FSMAGIC`。旧 inode 格式（`0x10203040`）的镜像会被内核明确拒绝；同为 `0x10203041` 但缺少账户文件的早期镜像会停在登录错误提示。两者都需备份数据后重建镜像，内核不会自动绕过认证。

**许可证与致谢**：本项目基于 **xv6-riscv**，其版权与许可证文本见 [`LICENSE.xv6`](LICENSE.xv6)。

> The xv6 software is Copyright (c) 2006-2024 Frans Kaashoek, Robert Morris, Russ Cox, Massachusetts Institute of Technology.

xv6 受 John Lions 的 *Commentary on UNIX 6th Edition* 启发，是 MIT 6.1810 的教学操作系统，参见 <https://pdos.csail.mit.edu/6.1810/>。本项目自身新增的代码以 MIT 许可证发布，见 [`LICENSE`](LICENSE)。
