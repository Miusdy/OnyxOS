# 开发环境能力计划（批次 14–20）

> 状态：**计划，尚未实施**。本文只定义范围、接口、验收与批次边界，不代表任何一项已经可用。
> 日期：2026-10-17。
> 基线：`105d33e`（批次 13 合并后的上游 `main`）。
> 依据：当前 [README](../README.md)、[开发文档索引](README.md)、[能力路线图](minios-roadmap.md)、[批次实施记录](batches.md)、[批次 13 计划](batch13-plan.md) 以及 `test-xv6.py`、`test-shell.py`、`Makefile` 的实际行为。
> 环境约定见 §2。本文档本身只新增 `docs/` 下的文件；实施时批次 14 会新增 `tools/` 目录，这是计划内的唯一新目录。不再为每个批次另写计划文档：各批实施记录统一追加到 [批次实施记录](batches.md)，最终验收另写 `docs/stage5-validation.md`。

---

## 1. 目标与判断

OnyxOS 已经完成“机制 + 自检”的基础建设：48 个系统调用、45 个用户程序，COW、mmap、信号、权限、目录边界都有对应的专用测试。当前缺口不在机制数量，而在**开发闭环**：

- 改一个用户程序要重建 `fs.img`、重启 QEMU、重新登录；
- 测试通过串口正则驱动，`test-shell.py` 已出现过约 13% 的时序偶发失败；
- `psinfo`、`freemem`、`fsinfo`、`klog` 都是只读快照，缺少 fd、VMA、锁和系统调用级的诊断；
- 失败统一返回 `-1`，没有 errno，OOM、COW 复制失败、日志提交失败难以主动触发；
- Shell 没有 `$?`、`&&`、`||`、`PATH`、shebang 和 `/etc/rc`，也没有 `mv`。

本计划把已有能力从“自检程序”推进为“开发工具”，只做三件事：

1. **可编程**：宿主机能把客户机当作可自动化的目标，支持热重载和结构化测试。
2. **可诊断**：一次失败能定位到进程、系统调用、路径、函数和内核不变式。
3. **可证伪**：失败路径可以按需注入，并用不变式检查验证回滚是否干净。

### 1.1 明确不做

- 不做 TCP/IP、用户线程、swap、页面回收、完整 VFS、`/proc`、GUI。
- 不改变现有系统调用编号，不改变磁盘格式；新增调用一律从下一个空闲号追加。
- 不把开发用控制通道、故障注入或调试接口暴露给普通启动路径。
- 不为每个批次创建虚拟环境、conda 环境或独立工具链。
- 不以“命令数量”或“自检程序数量”作为完成标准。

### 1.2 设计原则

1. **每批独立可用**：每批都能单独构建、单独验收、单独回滚，不依赖后续批次才成立。
2. **默认路径不变**：`make qemu` 的行为、账户、权限、磁盘格式和既有测试不受影响；开发能力由显式目标或 root 操作启用。
3. **先测量再断言**：新增计数器、检查器和性能结论必须有对照或可复现的测量，不把推断写成事实。
4. **失败语义写清楚**：每个新接口必须说明返回什么、失败后留下什么状态、并发下谁负责同步。
5. **不削弱既有断言**：不改 `user/usertests.c` 的既有断言，不通过放宽超时或跳过用例来“通过”。

---

## 2. 开发环境：一次准备，所有批次复用（硬约束）

本计划的所有批次在每台开发机上共用该机唯一的一套环境；macOS 与 Windows/WSL2 各自准备一次。**新开终端不等于新环境**：只激活已有环境，不创建新的 venv 或 conda 环境。

### 2.1 平台与环境对照

每台开发机只准备一套环境；macOS 与 Windows/WSL2 的路径和工具链前缀不同，规则相同。当前参考环境如下：

| 组件 | macOS（本机现状） | Windows（WSL2） | 用途 |
| --- | --- | --- | --- |
| 宿主 | macOS ARM64 | Windows 10/11 + WSL2，Ubuntu 24.04 LTS | 本地开发 |
| Shell | 终端中的 bash/zsh | WSL 内 bash；不在 PowerShell/CMD 中构建 | 所有命令 |
| Python | 3.12.15（`onyxos`） | 3.12.x（`onyxos`；Ubuntu 24.04 系统 Python 也可） | 测试与宿主工具 |
| clang-format | 21.1.8（`onyxos`） | 21.1.8（同一 `onyxos`） | `make fmt` |
| RISC-V GCC | `riscv64-elf-gcc` 16.2.0（Homebrew） | `riscv64-linux-gnu-gcc`（Ubuntu 包） | 内核与用户程序 |
| RISC-V binutils | `riscv64-elf-addr2line` 等 | `riscv64-linux-gnu-addr2line` 等 | 符号化 |
| QEMU | `qemu-system-riscv64` 11.1.2 | `qemu-system-riscv64` ≥ 7.2（Ubuntu 24.04 通常为 8.2） | 客户机运行 |
| Git | 仓库内 Git | WSL 内 Git，不使用 Windows Git 操作本仓库 | 版本管理 |

最低版本以 [AGENTS.md](../AGENTS.md) 为准；上表是当前参考值，不是硬编码的版本承诺。

进入任一平台的仓库根后，只激活已有环境并自检：

```sh
conda activate onyxos
python3 -c 'import sys; print(sys.executable); print(sys.version)'
clang-format --version
qemu-system-riscv64 --version
```

macOS 再核对：

```sh
riscv64-elf-gcc --version
```

Windows/WSL2 再核对：

```sh
riscv64-linux-gnu-gcc --version
uname -a          # 必须显示 Linux，而不是 Windows
```

非交互脚本或 CI 风格调用使用：

```sh
conda run -n onyxos python3 test-harness.py
```

每台开发机只准备一次；之后每个批次、每个新窗口只重复激活与版本自检，不重复安装。macOS 本机已有 `onyxos` 环境；Windows 开发者按 §2.2 在 WSL2 内一次性建立同名环境。若环境漂移，先按 `AGENTS.md` 修好这台机器上的这一套环境，不在批次内新建替代环境。

### 2.2 Windows 开发者：统一在 WSL2 内

`AGENTS.md` 已规定 Windows 使用 WSL2；本节把一次性准备、仓库位置、换行和工具链差异写具体。同一台机器固定使用一个 WSL2 发行版（Ubuntu-24.04）和一套 `onyxos`，不要在不同发行版之间切换构建。Windows 端不要求在 PowerShell 或 CMD 里直接构建。Git、Python、编译器、QEMU、测试和宿主工具全部在 WSL2 内执行；不混用 Windows 可执行文件。

#### 2.2.1 一次性准备（每台机器只做一次）

1. 用管理员 PowerShell 安装 WSL2 与 Ubuntu 24.04：

   ```powershell
   wsl --install -d Ubuntu-24.04
   wsl --set-default-version 2
   ```

   之后统一从 Windows Terminal 进入 WSL 终端；后续所有命令都在 WSL 内执行。

2. 在 WSL 内安装基础依赖：

   ```sh
   sudo apt update
   sudo apt install -y build-essential git make perl bc \
     gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu qemu-system-misc
   ```

   `qemu-system-misc` 在 Ubuntu 24.04 提供 `qemu-system-riscv64`；如果发行版把它拆到别的包，安装提供该命令的包即可，最终以 `qemu-system-riscv64 --version` 为准。

3. 一次性准备 Python 3.12 与 clang-format 21。推荐沿用项目的 `onyxos` 环境：

   ```sh
   conda create -n onyxos python=3.12 -y
   conda activate onyxos
   python -m pip install clang-format==21.1.8
   ```

   如果该 pip 包在你的镜像中不可用，使用 LLVM 21.1.8 官方构建中的 `clang-format` 二进制固定到 PATH，版本仍须为 21.x。

   如果团队在 WSL 内已经有同名的 `onyxos` 环境，直接激活并核对版本，不要重建。若已有满足版本要求的固定环境（不必叫 `onyxos`），直接复用并在团队内统一，不要为本计划再建第二套。WSL 内没有 conda 时，先一次性安装 Miniconda/Miniforge；也可以使用 Ubuntu 24.04 的系统 Python 3.12，但必须把 clang-format 21 固定到 PATH 上。不要用 Windows 侧的 Python，也不要为了装 clang-format 在系统 Python 里绕过 PEP 668，更不要为每个批次新建环境。

4. 核对版本：

   ```sh
   uname -a
   python3 --version                  # 3.12.x
   clang-format --version             # 21.x，不能是 Ubuntu 自带的 18.x
   qemu-system-riscv64 --version      # >= 7.2
   riscv64-linux-gnu-gcc --version
   cc --version
   ```

   `cc` 必须来自 `build-essential` 并支持 `-shared -fPIC`（`test-auth.py` 需要）；`gcc` 用于构建 `mkfs`。

以上是每台 Windows 开发机唯一允许安装系统包和创建 `onyxos` 的时点；之后进入任何批次都只激活环境，不重复安装或重建。

#### 2.2.2 仓库位置、换行与编辑器

- 仓库放在 WSL 的 Linux 文件系统中，例如 `~/src/OnyxOS`；不要放在 `/mnt/c/...`。DrvFs 上 Git 的可执行位、大小写、文件锁和 I/O 性能都不可靠，DevLink 需要的 UNIX 域套接字也应在 WSL 内使用。
- 在 WSL 内 `git clone`，不要在 Windows 侧用 Git 克隆后再从 WSL 访问；已有的 `/mnt/c` 克隆建议在 WSL 内重新 clone，不要试图在 Windows 侧“修好换行”。
- 进入仓库后设置：

  ```sh
  git config --local core.autocrlf false
  git config --local core.eol lf
  ```

- 编辑器使用 VS Code 的 Remote - WSL，或任何明确设置为 LF 的编辑器；不要让 Windows 工具把 `.c`、`.h`、`.md`、`.py` 改成 CRLF。
- 提交前执行 `git diff --check`；若出现 CRLF 或行尾空白，先修正，不要带着跨平台换行差异提交。

#### 2.2.3 工具链与命令差异

- `Makefile` 会自动探测 `riscv64-linux-gnu-`；若没有探测到，显式执行 `make TOOLPREFIX=riscv64-linux-gnu-`。
- `tools/symbolize.py` 和 `test-tools.py` 必须按 `TOOLPREFIX`、`riscv64-elf-`、`riscv64-linux-gnu-` 探测工具前缀；不能把 macOS 的 `riscv64-elf-` 写死。
- QEMU 在 WSL 内以 `-nographic` 运行，退出用 `Ctrl-A` 后按 `x`；宿主工具也在 WSL 内运行，不要用 Windows Python 去连 WSL 里的 UNIX socket。
- DevLink 的 `onyxctl.py`、`onyxfs.py`、`symbolize.py` 都在 WSL 内执行；Windows 侧的 Python 不参与构建、测试或 socket 连接。
- WSL2 的 KVM 是可选加速项：`/dev/kvm` 可用时可以更快，但所有测试必须在纯 TCG 下也能通过，不把 KVM 作为前置条件。
- `onyxfs put` 依赖的 `fcntl` 文件锁和原子替换按 Linux 语义实现；只在 WSL ext4 上验证，不在 `/mnt/c` 上承诺。

#### 2.2.4 验证与提交

Windows/WSL2 属于 Linux 环境，必须按 §2.5 跑完整的单核/三核验证链，不能只跑构建。没有 KVM 时 QEMU 走 TCG，测试可能比 macOS 慢；超时先测基线并确认不是环境问题，不要为此放宽断言或全局超时。PR 和提交说明中注明平台为 `Windows 11 + WSL2 Ubuntu 24.04`，并如实记录未执行项。Windows 本地通过同样不能代替 Linux CI；合并前以 PR 的 Linux 矩阵为准。

### 2.3 批次内禁止事项

首次准备（§2.1/§2.2）完成后，以下操作在每个批次、每个新窗口中**禁止**：

- `python3 -m venv ...`、`uv venv`、`virtualenv`；
- `conda create`、`conda env create`、为单个批次新建环境；
- `pip install`、`conda install` 安装本计划所需依赖；
- 在仓库内生成 `.venv/`、`venv/`、`environment.yml` 或 `requirements.txt`；
- 安装第二套 RISC-V 工具链或第二个 QEMU。

新增的宿主工具只使用 Python 标准库。若确实需要第三方库，必须先单独评审并更新 `AGENTS.md`，不能夹在某个批次里顺手安装。

### 2.4 工作区与并发

- 所有批次在同一个 checkout 上从最新 `main` 建分支；不复制仓库来“顺便新建环境”。
- 需要隔离验收时可以使用 `git worktree`，但 Python、格式化器、GCC、QEMU 仍共用 `onyxos` 和现有 PATH。
- 同一 checkout 的 QEMU 测试串行执行；`test-xv6.py`、`test-shell.py`、`test-stage3.py` 不得并发。
- 测试会重建 `fs.img`。需要保留的镜像先按 [USER_GUIDE](../USER_GUIDE.md) 第 13 节备份。
- `test-results/`、`fs.img`、`kernel/kernel`、`.venv/` 等不提交。

### 2.5 每批的验证入口

所有批次至少执行：

```sh
make fmt && git diff --exit-code
make kernel/kernel fs.img
python3 test-harness.py
python3 test-auth.py
```

涉及内核、调度、内存、信号、页表的批次还必须执行：

```sh
python3 test-stage3.py --cpus 1
python3 test-stage3.py --cpus 3
python3 test-xv6.py dedicated --cpus 1
python3 test-xv6.py dedicated --cpus 3
python3 test-xv6.py usertests --cpus 1
python3 test-xv6.py usertests --cpus 3
python3 test-xv6.py crash --cpus 1
python3 test-xv6.py crash --cpus 3
python3 test-shell.py --cpus 1
python3 test-shell.py --cpus 3
```

只改宿主工具、文档或纯用户态脚本的批次，可以只跑对应宿主测试、构建和一次 QEMU 冒烟；但合并前仍需以 PR 的 Linux 单核/三核 CI 为准。macOS 构建通过或 Windows/WSL2 本地通过都不能代替 Linux CI 的完整回归。

### 2.6 每批开始与结束

开始（每个批次、每个新窗口相同）：

1. `conda activate onyxos`，确认 `python3` 是 3.12.x；不创建新环境。
2. 从最新 `main` 建批次分支；不复制 checkout 来“顺便新建环境”。
3. 先跑一次基线 `make kernel/kernel fs.img`，确认环境本身可构建。Windows/WSL2 还要先确认 `uname -a` 显示 Linux。

结束：

1. 按 §2.5 跑本批适用验证链，记录实际命令、CPU 配置、耗时和未执行项。
2. `make fmt && git diff --exit-code`、`git diff --check` 通过。
3. 提交说明包含问题、行为变化、测试结果、仍有限制；不提交构建产物和 `test-results/`。
4. 在 `docs/batches.md` 追加本批实施记录，更新本批涉及的 README/用户指南。

---

## 3. 现状依据

| 缺口 | 现有证据 | 直接后果 |
| --- | --- | --- |
| 开发闭环依赖串口 | `test-xv6.py` 的 `wait_shell()` 等待 `login:`、`Password:`、`$` 三段提示；`QEMU(True)` 每个用例重建 `fs.img` | 改一个程序要重启；测试靠正则和超时同步 |
| 串口测试有时序脆弱性 | [功能修复日志](functional-fixes-2026-10-05.md) 记录 `test-shell.py --case jobs` 在 `CPUS=1` 连续 30 次失败 4 次（约 13%） | 偶发失败难以区分“真 bug”和“脚本时序” |
| 观测是只读快照 | `psinfo` 只有进程级字段；`klog` 是全局环形；`panic` 只打印一个字符串 | 无法回答“哪个 fd/VMA/系统调用/路径出错” |
| 失败不可区分 | `README.md` 明确三类资源耗尽都只返回 `-1`；[批次 7](batches.md) 说明双重释放只能靠代码审查；[批次 13](batch13-plan.md) 说明目录读失败会 panic | 错误路径缺少可重复的负向用例 |
| Shell 不是开发环境 | [USER_GUIDE](../USER_GUIDE.md) 第 8.7 节说明没有 `mv`；命令要写 `/ls`；没有 `$?`、`&&`、`||`、shebang、`/etc/rc` | 无法写启动脚本和条件命令 |
| 可复用的底座已存在 | `Makefile` 已带 `-fno-omit-frame-pointer`，并生成 `kernel.sym` / `user/*.sym`；`testrun` 已输出机器可读退出码；`klog` 已有 seq 游标；virtio-mmio 还有空槽 | 新能力可以复用现有骨架，不需要推翻重来 |

---

## 4. 批次总览

| 批次 | 主题 | 主要交付 | 新增系统调用 | 依赖 | 退出条件 | 风险 |
| --- | --- | --- | --- | --- | --- | --- |
| 14 | 宿主工具 | `tools/onyxfs.py`、`tools/symbolize.py`、`test-tools.py` | 无 | 无 | 能离线检查镜像、符号化地址；一次 QEMU 冒烟 | 低 |
| 15 | 系统调用飞行记录器 | 全局 trace 环、`trace` 命令、panic 转储 | `tracectl`、`traceread` | 14 | `tracetest` 通过；权限与路径可见 | 低–中 |
| 16 | 内核不变式检查 | `check` 命令、镜像/进程/VMA/fd 审计 | `check` | 14、15 | 干净镜像返回 0；坏镜像/坏状态能报出 | 中 |
| 17 | DevLink | virtio-serial 驱动、`/dev/agent`、`onyxctl`、`make dev`、热重载 | 无（复用 `read`/`write`） | 14 | 同一 QEMU 会话内 `put` + `exec` 新程序 | 中–高 |
| 18 | 采样剖析与 panic 回溯 | `prof`、符号化、`backtrace()` | `profctl`、`profdump` | 14 | `fslimit` 热符号可解释；panic 有回溯 | 中 |
| 19 | 故障注入 | `failctl`、`failtest`、按 pid 注入 | `failctl` | 16 | OOM/COW/copyout 注入后 `check` 干净 | 中 |
| 20 | Shell 与文件系统可用性 | `rename`/`mv`、`nanosleep`/`sleep`、`$?`/`&&`/`||`、shebang、`/etc/rc` | `rename`、`nanosleep` | 无 | 能跑启动脚本和条件命令；既有 shell 测试通过 | 中 |

顺序说明：先做 14 建立工具底座；15/16 让失败可定位；17 的收益最大但技术风险也最高，放在有诊断能力之后；18/19 依赖符号化与 `check`；20 相对独立，可最后做，也可以在 17 之前先做 20.1 作为快速收益。

如果资源只够先做三批，**14 + 15 + 17** 已能形成“检查镜像 → 追踪失败 → 热重载运行”的最小闭环；16、18、19、20 都可以在不改变前三批接口的前提下追加。

系统调用编号不预先写死。实施时从当前 `kernel/syscall.h` 的下一个空闲号开始按批追加，禁止重排既有编号；每批在 `kernel/syscall.h`、`kernel/syscall.c`、`kernel/defs.h`、`user/user.h`、`user/usys.pl` 五处同步登记，并用编译期断言或宿主测试防止漏项。

---

## 5. 批次 14 — 宿主工具与符号化

### 5.1 目标

在不改内核的前提下，让 `fs.img` 和地址成为可直接检查、可直接操作的开发产物。这一批也为 15/16/18 提供符号化和镜像构造工具。

### 5.2 交付物

- 新增 `tools/onyxfs.py`：
  - `ls`、`cat`、`stat`：只读浏览镜像；
  - `fsck`：检查 superblock、位图、inode、目录项、`nlink`、`. ` / `..`、越界地址；
  - `put`：把宿主文件写入镜像，仅允许“干净镜像”（日志未提交事务为 0），先写临时副本，`fsync` 后用 `os.replace` 原子替换；
  - `--json`：给后续脚本和测试使用的稳定输出。
- 新增 `tools/symbolize.py`：
  - 输入 `0x...`、`文件名:地址` 或 stdin，调用 `<prefix>addr2line -f -C -e <elf>`；
  - 工具前缀按 `TOOLPREFIX`、`riscv64-elf-`、`riscv64-linux-gnu-` 依次探测，macOS、WSL2 与 Linux CI 共用同一脚本；
  - 内核默认 `kernel/kernel`，用户程序默认 `user/_<name>`；
  - 找不到 `addr2line` 时给出可执行的诊断，不静默返回空。
- 新增 `test-tools.py`：
  - 用标准库 `unittest`；
  - 用 `mkfs/mkfs` 在临时目录构造镜像，覆盖正常读取、坏 magic、截断镜像、脏日志、`put` 前后一致性；
  - 用 `nm` 找一个已知符号，验证 `symbolize.py` 输出；`nm` 同样按上面的工具前缀探测；
  - `--qemu` 额外跑一次真实客户机冒烟：`put` 一个文本文件，启动 QEMU，`cat` 回读。
- 不改 `kernel/`、`user/`、`mkfs/`，不改磁盘格式。

### 5.3 接口与约束

布局常量以 `kernel/fs.h`、`kernel/param.h`、`mkfs/mkfs.c` 和镜像 superblock 为唯一来源：`FSMAGIC`、`BSIZE`、`NDIRECT`、`DIRSIZ` 直接从共享头解析，`NINODES`/`FSSIZE` 从 `mkfs` 与 `param.h` 读取，运行时以 superblock 的 `size`/`ninodes` 为准；`NINDIRECT`、`MAXFILE`、`IPB`、`BPB` 按头文件中的同一公式计算。不在 Python 里硬编码数字；解析失败时明确报错，不“猜一个默认值”。`test-tools.py` 用 `mkfs` 生成的真实镜像交叉校验这些推导。

`onyxfs put` 的限制：

- 只支持普通文件，不支持目录、设备节点和硬链接；
- 单文件不超过 `MAXFILE * BSIZE`；
- 镜像必须没有未提交日志事务；约定先退出 QEMU 再写入。`put` 用临时副本 + 原子替换，运行中的 QEMU 持有旧 inode，不会把新镜像写坏，但它之后的写入不会进入新镜像；工具可选检测 `fs.img.lock`，检测不到时不承诺并发写安全；
- 失败时保留原镜像，不留下半写状态。

### 5.4 验收

```sh
make mkfs/mkfs kernel/kernel fs.img
python3 test-tools.py
python3 tools/onyxfs.py fsck fs.img
python3 tools/onyxfs.py ls fs.img
python3 tools/symbolize.py --kernel "$(riscv64-elf-nm kernel/kernel | awk '$3 == "panic" {print "0x" $1; exit}')"
python3 test-tools.py --qemu
```

WSL2/Linux 上把示例中的 `riscv64-elf-nm` 换成 `riscv64-linux-gnu-nm`；`symbolize.py` 本身会按前缀探测，示例只是先取一个已知符号地址。

通过标准：

- `fsck` 对干净镜像返回 0；对人为改坏的副本返回非 0，并指出具体 inode/块；
- `put` 后重新 `fsck` 为 0，启动 QEMU 能读到新文件，原文件不受影响；
- 脏日志镜像上 `put` 明确拒绝；
- `symbolize.py` 能把 `panic`、`syscall` 等已知符号的地址还原为函数名；
- 现有 `make qemu`、`test-harness.py` 不受影响。

### 5.5 风险与边界

- 宿主直接改镜像有损坏风险：用临时副本 + 原子替换 + `fsck` 兜底；脏镜像不在宿主重放日志。
- 布局常量解析依赖 C 头文件格式；`test-tools.py` 必须覆盖“解析失败即报错”，而不是静默使用旧值。
- 工具不替代 `mkfs`；正常构建仍走 `make fs.img`，`onyxfs put` 只用于开发和调试。

---

## 6. 批次 15 — `trace` 系统调用飞行记录器

### 6.1 目标

在系统调用边界记录“谁、什么时候、调用了什么、参数是什么、返回什么”，并在 panic 时保留现场。它解决的是没有 errno、失败只有一个 `-1` 的问题。

### 6.2 接口

新增共享头 `kernel/trace.h`：

```c
struct trace_entry {
  uint64 tick;
  uint64 ret;
  uint64 args[3];
  int pid;
  int ppid;
  ushort uid;
  ushort syscall;
  uchar mode;    /* user / kernel */
  uchar flags;   /* enter / exit */
  uint32 seq;    /* 写入序号，读者校验是否被覆盖 */
  char path[32]; /* 仅路径类调用，失败则清零 */
};

int tracectl(int op, int pid);                 /* op: OFF / ON / CLEAR */
int traceread(struct trace_entry *buf, int max, uint64 *lost);
```

- `tracectl` 对 pid 的权限与 `signal` 一致：root 或同 UID；`pid=0` 表示只控制调用者。`ON` 设置目标进程的 `trace` 标志，fork 继承、exec 保留；`OFF` 清除；`CLEAR` 清空全局环，仅 root 可用。
- `traceread` 读取全局环，返回条目数，并通过 `lost` 回填因回绕而丢失的条目数；不持有锁跨 `copyout`。每个条目带 uid，普通调用者只能拿到自己 uid 的条目，root 可读全部；这样全局环不会把其他用户的路径泄露给普通用户。
- 记录点在 `kernel/syscall.c` 的入口和返回处；只记录 `trace` 标志为真的进程，未开启时没有写入。入口记 enter + 参数 + 路径，返回记 exit + 返回值。
- 环使用与 `klog` 相同的无锁发布方式：写者写条目后 release 发布索引，读者 acquire 读取；环回绕时由新条目覆盖旧条目，并计数 `lost`。每个条目带 `seq`，写者在写入前后更新序号，读者校验前后一致，遇到正在覆盖的条目就跳过；panic 路径允许输出不完整。
- 记录路径类调用时，用有界 `copyinstr` 最多取 31 字节；失败就把 `path` 清零，不影响系统调用本身。

`trace` 用户程序：

```sh
trace /permtest
trace -f /mixstress        # 包含子进程
trace -p 12                # 附着到已存在进程（需要权限）
trace -e open,chmod /permtest
```

输出格式固定为 `pid ppid syscall(args) = ret`，路径以引号显示；`-o file` 写入文件，便于宿主分析。系统调用名来自新增的共享表，内核与用户程序共用，避免两边各抄一份。

### 6.3 panic 转储

在 `panic()` 打印完原因后，追加当前进程最后 32 条 trace（如果当前进程的 trace 标志为真且环已启用）。panic 路径只读环、不加锁、不分配内存；如果环正在被覆盖，允许输出不完整，并在末尾标记 `trace: partial`。

### 6.4 验收

新增 `user/tracetest.c`，加入 `DEDICATED`：

1. 子进程 `open("/no/such/file")` 失败，父进程读环，断言存在 `open` 且 `ret == -1`、路径包含 `/no/such/file`；
2. `fork` 后子进程的 syscall 带正确 `ppid`；
3. `trace -f` 能覆盖孙进程；
4. 关闭 trace 后，环不再增长；
5. 非 root 读取其他 UID 的环返回 -1；
6. 记录 1,000 次以上系统调用，环回绕后仍能返回最近条目，`lost` 单调增加。

panic 转储无法在不触发停机的情况下放进 CI：用一次临时 `panic("trace test")` 的本地构建验证并记录在批次实施记录中，不把故意 panic 的用例加入常规回归。

### 6.5 风险与边界

- 环是静态内存：1,024 条 × 约 96 字节 ≈ 96 KiB。若实测影响内存镜像，改为 `tracectl` 时按页分配，panic 时若未分配就跳过转储。
- 路径拷贝会增加系统调用开销，但只在 `trace_on` 时发生；关闭时每 syscall 只多一次分支。
- trace 不是可靠审计日志：环会覆盖，panic 输出允许部分丢失；不要把它当安全审计或持久化日志。

---

## 7. 批次 16 — `check` 内核不变式检查

### 7.1 目标

把“测试没打印 FAIL”提升为“已知内核不变式没有被破坏”。它检查的是内核自身的账目和引用关系，不修复、不修改状态。

### 7.2 接口

```c
int check(char *buf, int max);   /* 返回 0 表示未发现违规；-1 表示检查本身失败 */
```

- root-only；`buf` 以文本报告输出，包含 `violations=N` 和计数摘要；
- `check` 用户程序打印报告；`check --stats` 只打印计数，便于宿主做前后差值；
- 报告写入固定大小的内核缓冲区，`copyout` 在锁外执行。

### 7.3 第一批检查项

| 类别 | 检查 |
| --- | --- |
| 进程 | `state` 合法；pid 在范围内；parent 指针有效；同一 pid 不重复 |
| VMA | `start < end`；数量 ≤ `NVMA`；区域不重叠；地址在允许区间；文件引用非空 |
| fd | `ofile` 引用计数 > 0；类型合法；fd 索引 < `NOFILE` |
| inode | 类型合法；`nlink` 与目录项计数一致；块地址在数据区内；`.`/`..` 存在且正确 |
| 文件系统 | 位图与 `fs_nused_blocks`/`fs_nused_inodes` 一致；`sb.size`/`sb.nblocks` 自洽；日志头合法 |
| 分配器 | `kmem.nfree` 与空闲链表长度一致；`pages_live` 与守恒式一致 |

第一批不做全页表物理页引用审计：它成本高，且需要处理跨核并发；把“页表叶子与 `page_state` 引用计数逐页对照”留作 16.2 的可选扩展，先保证进程/VMA/fd/inode/位图这五类能稳定工作。

`nlink == 0` 但仍在 inode 表中的孤儿是崩溃恢复的合法中间态：`check` 在报告里列出它，但不计为违规；只有 `nlink > 0` 且与目录项计数不一致时才计为违规。

### 7.4 并发与语义

`check` 是**诊断快照，不是原子快照**。系统空闲时结果最可靠；在 `dedicated` 中每次只运行一个用例，天然接近空闲。实现时必须：

- 每次只持有一个进程的 `p->lock`，释放后再处理下一个；
- 不持 inode 锁跨目录扫描；按 inode 号顺序逐个加锁；
- 不持任何锁跨 `copyout`；
- 发现不一致时继续扫描，最多记录 16 条，避免第一条就返回导致信息不足；
- 明确写出“检查期间状态可能变化，因此报告是 best-effort”。

### 7.5 验收

新增 `user/check.c`，先作为独立 `DEDICATED` 用例；稳定后再在 `test-xv6.py` 增加 `--check` 选项，在指定专用用例成功路径末尾附加 `testrun check`，默认关闭，避免新检查器自身的假阳性影响既有 CI。

1. 全新镜像：`check` 返回 0，`violations=0`；
2. 在同一个 QEMU 会话里先跑 `fslimit 650`、`mixstress`、`cowtest`，再跑 `check`：返回 0，fd/VMA/inode 计数回到基线；
3. 用批次 14 的 `onyxfs` 库函数在临时副本上改一个 inode 的 `nlink`，启动后 `check` 必须报出该 inode；
4. 构造一个位图计数不符的镜像，启动后 `check` 必须报出；
5. 对越界块地址等“读目录阶段就会失败”的损坏，若现有实现先 panic，则记录为已知限制，不在本批承诺 `check` 能救回无法读取的镜像；
6. `check --stats` 在 `mixstress` 前后对比，不出现 fd、VMA、inode 的单调增长；
7. 单核/三核各跑一次；`check` 单次耗时目标 < 1 秒。

### 7.6 风险与边界

- `check` 可能因为并发变化产生假阳性：报告必须带“best-effort”说明，测试只在空闲/单用例环境断言 `violations=0`。
- 全量 inode/目录扫描会变慢：默认镜像只有 200 个 inode，可接受；若未来增大 `NINODES`，需要分页或采样。
- `check` 不修复任何东西；修复仍走 `mkfs`/正常文件操作，避免在诊断路径引入写风险。

---

## 8. 批次 17 — DevLink：宿主与客户机的控制通道

### 8.1 目标

把客户机从“只能手敲的串口终端”变成“可被宿主机脚本驱动的目标”。这一批的验收不是“多了一个命令”，而是**同一个 QEMU 会话内改程序、传程序、运行程序，不重启、不重建镜像**。

### 8.2 分步交付

这一批技术风险最高，拆成四个可独立验收的子步，任何一步失败都不阻塞 14–16：

#### 17.1 设备探针

- 在第二个 virtio-mmio 槽（QEMU virt 布局为 `0x10002000`、IRQ 2）探测 virtio console/serial，设备 ID 3、version 2；
- 只打印设备 ID、队列数和特性位，不做协议；
- 新增 `make dev` 目标，QEMU 参数追加 `-chardev socket,...`、`-device virtio-serial-device,bus=virtio-mmio-bus.1`、`-device virtserialport,...`；
- 普通 `make qemu` 不加载该设备。

验收：`make dev` 启动后内核打印探针结果；`make qemu` 行为完全不变。

#### 17.2 `/dev/agent` 字符设备

- 新增 `kernel/virtio_console.c`，实现 RX/TX 两个队列，不启用 multiport；若 QEMU 强制 multiport，再补控制队列；在 `plic.c` 使能 IRQ 2；
- 在 `devsw[]` 注册 `AGENT` 主设备号，提供 `agentread`/`agentwrite`；用 `sleep`/`wakeup` 处理空/满；
- `init` 在启动时创建 `/dev` 目录（已存在则忽略），执行 `mknod("/dev/agent", AGENT, 0)` 和 `chmod("/dev/agent", 0600)`；节点已存在时忽略 `mknod` 失败，`chmod` 每次执行。随后 fork/exec `agent`；设备不存在时 `agent` 打开 `/dev/agent` 失败并退出，节点本身和登录流程不受影响；
- 新增 `user/agent.c`，先实现回显：收到一帧原样返回。

验收：`onyxctl ping` 能在 100 ms 内收到 `pong`；拔掉 `make dev` 的 virtio-serial 参数后，`agent` 退出且普通启动不受影响。

#### 17.3 协议与 `onyxctl`

新增 `tools/onyxctl.py`，只使用标准库：

| 操作 | 语义 |
| --- | --- |
| `ping` | 探活，返回协议版本 |
| `exec` | 执行命令，返回退出码、stdout、stderr、耗时 |
| `put` | 上传文件到客户机路径，设置权限 |
| `get` | 下载文件并校验长度/摘要 |
| `stat` | 查大小、类型、权限 |
| `klog` | 按 seq 游标增量拉内核日志 |

帧格式：`magic` + `type` + `flags` + `length` + `request id`，单帧上限 4 KiB，支持分片；宿主侧每个请求有超时和重试上限，失败时打印请求 id 和已收字节数，不无限等待。

`exec` 请求携带 argv 数组，不做 shell 解析：agent 先按原样 `exec`，失败后尝试 `/<name>`，与后续 Shell 的默认命令查找保持一致；引号、管道和变量展开由调用者自己拆分，agent 不实现。

`exec` 第一版把 stdout/stderr 重定向到客户机临时文件（agent 启动时创建 `/tmp`），退出后回传并删除临时文件；这样实现简单，且足以支撑测试。流式输出作为 17.3 的可选扩展，不能阻塞本批验收。

验收：`onyxctl exec "testrun cowtest"` 返回 0；`put` 一个文本文件后 `get` 回来逐字节一致；`stat`、`klog` 正常；非 root 无法直接打开 `/dev/agent`。

#### 17.4 热重载

新增 `make dev-reload` 或 `onyxctl watch`：

1. 监听 `user/*.c` 的修改时间；
2. `make _<name>`；
3. `put` 到 `/tmp/<name>.<pid>`；
4. `exec` 该二进制；
5. 失败时保留客户机日志和宿主构建输出。

验收：修改 `user/echo.c` 的一行输出，保存后在同一个 QEMU 会话里看到新输出；从保存到输出 < 2 秒；不重启 QEMU、不重建 `fs.img`。

### 8.3 测试与 CI

新增 `test-devlink.py`：

- 启动 `make dev`，使用临时 socket 路径；
- 等待 agent 就绪，执行 `ping`；
- `exec testrun cowtest`，断言退出码和 `cowtest: OK`；
- `put`/`get` 一个随机内容文件，比较 SHA-256；
- `exec` 一个刚构建的 `_echo`，验证热重载路径；
- 结束时杀掉 QEMU 进程组，断言没有遗留进程。

`test-devlink.py` 作为一个独立的 CI 步骤（Linux 1/3 都跑；macOS 仍只构建；Windows/WSL2 本地按 Linux 链跑，不新增 Windows CI）。它不得替换或削弱现有串口测试：`test-shell.py` 继续走真实终端语义，`test-xv6.py` 保留串口路径，DevLink 是新增路径。

### 8.4 安全边界

- 只有 `make dev` 加载 virtio-serial 设备；普通 `make qemu` 没有设备，`agent` 自然退出。
- `/dev/agent` 仅 root 可读写；socket 文件权限 0600，位于临时目录或 `test-results/`，测试结束删除。
- agent 不接受网络连接，不做认证协议；它的安全边界是“宿主本地 socket + root 客户机进程”。文档必须写明这是开发接口，不是生产远程管理通道。
- 不在 agent 中实现任意内核内存读写；所有文件操作仍走现有权限检查。

### 8.5 风险与回退

- **最大风险**是 virtio-serial 的特性协商和队列初始化。17.1 先只验证设备可见；若控制队列/多端口在时间盒内无法稳定，退化为单端口、只做 `exec`/`put`，把 `get`/`stat` 延后，不阻塞 18–20。
- 若设备方案整体不可行，不改用“串口多路复用”伪装控制通道；宁可把 17 标记为未完成，也不把不可靠的时序通道引入测试主链。
- 不允许为了 DevLink 修改既有 48 个系统调用的语义，也不允许把开发设备默认暴露给普通启动。

---

## 9. 批次 18 — 采样剖析与 panic 回溯

### 9.1 目标

回答“时间花在哪个函数”，并把 panic 从“一行字符串”变成可符号化的现场。

### 9.2 `prof`

- 新增 `kernel/prof.c` / `kernel/prof.h`：固定大小哈希表（建议 2,048 或 4,096 项），记录 `pc`、`pid`、`count`；
- 在 `clockintr()` 中采样 `r_sepc()`；`sstatus.SPP` 区分用户态/内核态；
- `profctl(op, pid)` 控制开始/停止/清空；`profdump(buf, max)` 返回条目；两者仅 root 可用，避免跨用户采样成为侧信道；
- `prof` 用户程序支持 `prof run cmd`、`prof start`、`prof stop`、`prof dump`；
- 输出只给地址和计数，符号化由批次 14 的 `tools/symbolize.py` 完成。

采样口径：默认每 100 ms tick 一次，长用例足够；短用例需要更细粒度时，再单独增加采样定时器，不在本批承诺。哈希碰撞允许合并，报告必须写“近似采样”，不声称精确调用计数。

### 9.3 panic 回溯

- 在 `printk.c` 增加 `backtrace()`：从当前 `s0` 帧指针沿内核栈回溯，打印 `pc`、`ra`、`sp`，限制最大帧数（如 32）；
- `Makefile` 已有 `-fno-omit-frame-pointer`，不需要改编译选项；
- 每帧校验栈地址在 `[kstack, kstack + 2*PGSIZE)` 内、地址对齐、单调向高地址；异常时停止并打印 `backtrace: truncated`；`myproc() == 0` 或栈范围未知时只打印原始 `pc`/`ra`/`sp` 并停止；
- `panic()` 在打印原因后调用 `backtrace()`，再由宿主用 `symbolize.py` 还原函数名；
- 不在内核里嵌入符号表，避免增大镜像。

### 9.4 验收

1. `prof run /fslimit 650` 的 top 符号集中在 `dirlookup`、`writei`、`bmap` 等目录/文件路径，能解释批次 13 的 O(n²) 推断；
2. `prof run /cputest` 能区分用户态自旋与内核态系统调用；
3. 关闭 prof 时，`clockintr()` 只多一次全局标志判断；
4. 用一次临时 `panic("backtrace test")` 构建验证：输出至少包含 `panic`、调用者和 3 帧以上；该验证记录在批次实施记录中，不进入 CI；
5. `symbolize.py --kernel <panic 地址>` 能还原到函数名。

### 9.5 风险与边界

- 采样只有 PC，没有调用栈；不能替代 gdb，也不能给出精确的每函数耗时。
- 100 ms 采样对 5 秒级用例偏粗；报告必须给出样本数，样本太少时明确写“不足以判断”。
- 帧指针回溯在 panic 现场可能被破坏；允许截断，不把不完整回溯当成错误。

---

## 10. 批次 19 — 故障注入与失败路径验证

### 10.1 目标

让 OOM、COW 复制失败、`copyout` 失败、mmap 缺页失败等路径可以按需、可重复地触发，并用批次 16 的 `check` 验证失败后没有泄漏或半截状态。

### 10.2 接口

```c
int failctl(const char *name, int skip, int times, int flags);
```

- root-only；`name` 为故障点名；`skip` 表示前 N 次放行；`times` 表示注入次数；`flags` 可带 pid 过滤；
- 默认关闭；配置由自旋锁保护，热路径只读一个全局 `fail_active`，关闭时开销为一次分支；
- 第一批故障点：`kalloc`、`mmap_fault`、`copyout`、`bio_read`、`bio_write`；
- `writei`、`dirlink`、`log_commit` 作为第二批，只有在失败语义已经写清楚、且不会把文件系统置于未定义状态时才加入；
- 注入失败返回调用点原本的错误值（通常是 0 或 -1），不额外 panic；是否 panic 由调用点决定。

### 10.3 `failtest`

新增 `user/failtest.c`，按场景组织：

| 场景 | 注入 | 断言 |
| --- | --- | --- |
| mmap 缺页 OOM | `mmap_fault` | 进程以非 0 退出；`mem_free` 回到基线；无残留 VMA |
| COW 复制失败 | `kalloc` + 子进程写共享页 | 父进程数据不变；共享页计数回到基线 |
| `copyout` 失败 | `copyout` | 系统调用返回 -1；不留下半写用户缓冲区（或按已写字节数返回） |
| 块读取失败 | `bio_read` | 上层返回 -1；不 panic、不破坏缓存 |
| 块写入失败 | `bio_write` | 按既有日志语义处理；若当前实现会 panic，记录为已知限制，不强行断言“干净” |

每个场景结束后：父进程关闭注入、调用 `check`、比较 `mem_free`/`fsinfo` 基线、回收子进程。`failtest` 加入 `DEDICATED`，单核/三核都跑。

### 10.4 验收

1. `failctl` 非 root 调用返回 -1；
2. 注入次数用尽后自动关闭，后续系统调用恢复正常；
3. `failtest` 在单核/三核通过，且没有 panic、僵尸或泄漏；
4. 关闭所有故障点后，完整 `usertests`/`crash` 继续通过；
5. 在批次实施记录中列出“能让内核 panic 的注入点”，不把它们伪装成已处理。

### 10.5 风险与边界

- 故障注入是开发设施，不是产品功能；默认关闭，只允许 root，且不承诺覆盖所有错误路径。
- `kalloc` 注入可能影响内核自身；用 pid 过滤把注入限制在目标子进程，测试框架自身不参与注入。
- 日志/目录写入的部分失败语义可能不干净；如果实测会 panic，就记录为发现的问题，另开批次修复，不在本批放宽断言。

---

## 11. 批次 20 — Shell 与文件系统可用性

### 11.1 目标

让 OnyxOS 内部可以写启动脚本、条件命令和简单的文件维护流程，而不是每条命令都手敲绝对路径。

### 11.2 `rename` 与 `mv`（20.1）

新增 `rename(old, new)` 系统调用和 `mv` 用户程序：

- 仅支持同一文件系统内的普通文件；目录重命名明确返回 -1，文档写明限制；
- 目标存在时替换目标文件；源不存在、父目录无写权限、跨目录越权均失败；
- 目录项更新与 `nlink` 调整在同一个日志事务内完成，崩溃恢复后要么旧名字存在，要么新名字存在；
- 不改变磁盘格式。

验收：`mv a b` 后 `a` 不存在、`b` 内容不变；目标已存在时被替换；无权限时返回 -1。崩溃一致性优先复用“提交后立即停止 QEMU”的方式验证；如果本批没有新增 `rename` 崩溃注入用例，就在批次记录里写明“只做了日志事务审查与普通回归，未实测 rename 崩溃路径”，不把未测写成已验证。

### 11.3 `nanosleep` 与命令查找（20.2）

- 新增 `nanosleep(ticks)`（或 `sleep(ticks)`），按现有 tick 睡眠，不依赖 RTC；用户态提供 `sleep` 命令，参数以秒为单位并按 100 ms/tick 换算，不足 1 tick 按 1 tick 处理；
- Shell 在执行不含 `/` 的命令时，先按原样尝试，再尝试 `/<name>`；保留显式路径优先；
- 不引入完整 `PATH` 环境变量，避免在 20 批同时改 `exec` ABI。

验收：`sleep 1` 返回；`ls` 不再必须写 `/ls`；`/ls` 仍然可用；现有脚本和测试不受影响。

### 11.4 Shell 条件、`$?`、shebang 与 `/etc/rc`（20.3）

- Shell 增加 `$?`、`&&`、`||`；优先级为 `;`/`&` 最低，`&&`/`||` 次之，`|` 最高；
- `exec` 识别 `#!`：读取脚本首行，拆出解释器和可选参数，构造新的 argv 后执行解释器；限制解释器路径长度和参数数量；
- `init` 在启动 `login` 前执行 `/etc/rc`（存在时）；失败不阻塞登录，只打印错误；
- `mkfs` 生成一个最小 `/etc/rc`（例如创建 `/tmp` 和默认目录），并设置 root 所有、不可被普通用户修改；agent 由 `init` 启动，不在 `/etc/rc` 里重复启动；
- 不实现 `export`、`$VAR` 展开、引号、通配符和完整 POSIX 会话；这些留给后续独立批次。

验收（新增到 `test-shell.py`）：

1. `false && echo bad` 不输出 `bad`；`false || echo ok` 输出 `ok`；
2. `$?` 在成功/失败后分别是 0/非 0；
3. `sh /test.sh` 能通过 shebang 运行，脚本参数正确；
4. `/etc/rc` 在登录前执行，且普通用户不能改写；
5. 既有语法、内存、追加、作业控制用例全部通过；
6. `make fmt`、`git diff --check` 通过。

### 11.5 风险与边界

- Shell 解析器改动容易回归：20.3 单独提交，先加测试再接解析；不改变 99 字符输入上限和 100 个解析节点上限。
- `rename` 只做普通文件，避免目录 `..` 更新和跨目录环路；目录重命名另开批次。
- `#!` 是 `exec` 的新分支，必须限制递归（解释器自身也是脚本时拒绝）和参数长度，避免栈/缓冲区越界。

---

## 12. 跨批次验收矩阵

| 批次 | 宿主测试 | 构建 | dedicated 1/3 | usertests 1/3 | crash 1/3 | test-shell 1/3 | 新增入口 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 14 | `test-tools.py` | 必跑 | 可选冒烟 | — | — | — | `test-tools.py --qemu` |
| 15 | — | 必跑 | 必跑 | 必跑 | 必跑 | 必跑 | `tracetest` |
| 16 | — | 必跑 | 必跑 | 必跑 | 必跑 | 必跑 | `check` |
| 17 | `test-devlink.py` | 必跑 | 必跑 | 必跑 | 必跑 | 必跑 | DevLink 冒烟 |
| 18 | `test-tools.py` | 必跑 | 必跑 | 必跑 | 必跑 | 必跑 | `proftest` |
| 19 | — | 必跑 | 必跑 | 必跑 | 必跑 | 必跑 | `failtest` |
| 20 | `test-tools.py` | 必跑 | 必跑 | 必跑 | 必跑 | 必跑 | shell 新用例 |

说明：

- “必跑”指该批合并前至少执行一次；涉及并发/内存/信号/页表的批次按 §2.5 的完整链执行。
- 每批新增的专用测试必须能在单核/三核各跑一遍；不允许只在一个 CPU 配置下通过就合并。
- 新增用例的单例耗时目标 < 120 秒；`dedicated` 总时长目标 < 150 秒（批次 13 后约 103 秒）。超过时先优化用例，不直接放宽超时。
- 不修改 `user/usertests.c` 的既有断言；不为新功能降低任何旧测试的标准。

---

## 13. CI 与时间预算

- `.github/workflows/test.yml` 的 Linux 矩阵继续使用 `CPUS=1`/`3`；macOS 仍只验证构建；Windows 开发者不新增 Windows CI，统一在 WSL2 内按 Linux 链验证。
- 批次 14 的 `test-tools.py` 在 `make mkfs/mkfs` 之后运行，不需要 QEMU；`--qemu` 冒烟可放在 Linux 单核 job。
- 批次 17 的 `test-devlink.py` 作为独立步骤，放在 `dedicated` 之后；失败时上传 socket 日志和 QEMU 串口输出。
- 批次 18/19 的新用例加入 `DEDICATED`，单例超时沿用 120 秒；`failtest` 若接近上限，拆成 `failtest kalloc` / `failtest io` 两个子场景，不提高全局超时。
- 每个批次在 PR 中记录 `dedicated` 各用例秒数，和批次 13 的基线（`fslimit` 约 57–63 秒）对照；新增开销必须可见。

---

## 14. 文档、分支与提交

### 14.1 分支

每批从最新 `main` 建独立分支，命名沿用现有约定：

```text
batch14-host-tools
batch15-trace
batch16-check
batch17-devlink
batch18-prof
batch19-failpoints
batch20-shell-fs
```

一个分支只处理一个批次；不把多个批次混在一个 PR。跨批次的公共头文件或工具改动，放在最先需要它的批次里，后续批次复用。

### 14.2 提交说明

每个提交说明包含：**问题、行为变化、测试结果、仍有限制**。没有行为变化或限制时明确写“无”。涉及接口/磁盘/权限的批次必须同步文档。

### 14.3 每批完成后更新

- `README.md`：新增命令、系统调用、已知限制；
- `docs/batches.md`：追加该批的设计、改动、取舍和验收结果；
- `docs/README.md`：如新增独立文档，补索引；
- `USER_GUIDE.md`：涉及 `mv`、`sleep`、`ls` 默认查找、`/etc/rc` 的用户可见行为；
- `Makefile` 的 `UPROGS`、`user/help.c` 的命令索引和镜像容量：新增用户程序必须同步登记，`mkfs` 后确认 `fs.img` 未超预算；不把调大 `FSSIZE` 当作默认手段；
- `docs/minios-roadmap.md`：在批次 14 启动时增加“开发环境能力”方向，批次 20 完成后更新状态；
- 计划结束时新增 `docs/stage5-validation.md`，记录环境、命令、单核/三核结果、未执行项和剩余限制。

### 14.4 不提交

`fs.img`、`kernel/kernel`、`test-results/`、`test-xv6.out`、`.venv/`、`.DS_Store`、`.vscode/`、个人绝对路径和任何环境文件。

---

## 15. 风险与回退

| 风险 | 影响 | 应对 |
| --- | --- | --- |
| DevLink 的 virtio-serial 协商失败 | 批次 17 阻塞 | 17.1 先做设备探针；失败则只保留控制通道的最小 `exec`，文件传输延后；不把不可靠通道接入主测试链 |
| trace/prof 静态内存过大 | 内核镜像/内存变化 | 先按 1,024 条/2,048 桶估算；必要时改为 `tracectl`/`profctl` 时按页分配 |
| `check` 在并发下假阳性 | 测试不稳定 | 明确 best-effort；只在空闲/单用例环境断言零违规；报告带时间与进程数 |
| failpoint 注入打挂内核 | 调试设施变成故障源 | 默认关闭、root-only、pid 过滤；先做 `kalloc`/`copyout`/`mmap_fault`，日志写入点后置 |
| Shell 解析器回归 | 既有作业控制/语法测试失败 | 20.3 单独提交；先写测试再改解析；不改既有上限 |
| 宿主工具写坏镜像 | 丢失镜像内容 | 临时副本 + 原子替换 + `fsck`；脏镜像拒绝写入；测试先备份 |
| CI 时间超限 | PR 无法合并 | 新用例单例 < 120 秒；dedicated 目标 < 150 秒；超时先拆用例 |
| 每批重复建环境 | 环境漂移、浪费时间 | 本文 §2 的硬约束；只用 `onyxos`，不新建 venv/conda env |

回退原则：每批只新增接口和开发设施，默认路径不变；若某批失败，回退该批分支即可，不影响已完成批次。任何需要改既有 48 个系统调用语义的方案都退回重新设计。

---

## 16. 完成定义

本计划完成需同时满足：

1. 批次 14–20 全部合并，且每批有独立 PR、CI 结果和批次实施记录；
2. `make qemu` 的默认启动、账户、权限、磁盘格式和既有测试不变；
3. `make dev` + `onyxctl` 能在同一 QEMU 会话内完成“改程序 → 传程序 → 运行”，不重建 `fs.img`；
4. `trace` 能定位一次权限失败的系统调用和路径；`check` 在干净镜像上返回 0；`prof` 能给出可符号化的热点；`failtest` 能注入并验证至少三类失败；
5. Shell 能执行 `/etc/rc`、`&&`/`||`/`$?` 和 shebang 脚本，`mv` 可用；
6. 单核/三核的 `dedicated`、`usertests`、`crash`、`test-shell` 和宿主测试全部通过，未放宽既有断言；
7. `docs/stage5-validation.md` 记录环境、命令、结果、未执行项和剩余限制；
8. 全程只使用每台开发机已有的 `onyxos` 环境（macOS 本机或 Windows/WSL2），没有为任何批次新建虚拟环境。

最小演示链（批次 14、15、17 完成后即可执行）：

```sh
make dev
python3 tools/onyxctl.py ping
python3 tools/onyxctl.py put user/_echo /tmp/echo
python3 tools/onyxctl.py exec "/tmp/echo hello"
python3 tools/onyxctl.py exec "/trace /tmp/echo hello"
```

以上为计划文本。实施从批次 14 开始；每批开始前只需激活已有环境并从最新 `main` 建分支；Windows 开发者在 WSL2 内完成全部命令。
