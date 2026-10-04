# AGENTS.md

面向在本仓库工作的编码代理，以及新加入的人：环境、依赖、构建与验证的硬约束。
本文件描述**当前事实**；与代码冲突时以代码和 `docs/` 为准。

## 1. 这是什么项目

xv6-riscv 上的教学 miniOS（上游 `Miusdy/OnyxOS`，Fork: `Ella-101/HachileiOS`）。
仓库里有**两套编号，不能互相换算**：

- **批次（batch）**：提交历史的顺序编号，只出现在分支名与 `docs/batches.md`；编号空缺不代表漏合并。
- **阶段（stage）**：能力范围与验收单位，定义在 `docs/minios-roadmap.md`（阶段一自动化验证 → 阶段二信号与作业控制 → 阶段三身份与权限 → 阶段四按方向扩展，已选虚拟内存）。

文档入口，按这个顺序读：

| 文档 | 什么时候读 |
| --- | --- |
| `README.md` | 想知道**当前行为**：快速开始、特性总览、系统调用表、已知限制 |
| `docs/README.md` | 文档索引与时效说明（哪些是当前契约、哪些是历史快照） |
| `docs/minios-roadmap.md` | 范围、阶段划分、实施与交付规则 |
| `docs/stage1-validation.md` / `stage3-validation.md` / `stage4-validation.md` | 接口契约、边界、验收矩阵（阶段二见路线图 §4） |
| `docs/integrity-audit.md` | 分支/PR 对照、已修缺陷、独立实验的判定 |
| `docs/batches.md` | 每个批次的原始设计与取舍（从 README 拆出） |

## 2. 运行时依赖（精确要求）

| 组件 | 要求 | WSL / Debian / Ubuntu | macOS | 自检 |
| --- | --- | --- | --- | --- |
| `qemu-system-riscv64` | **≥ 7.2** | `sudo apt install qemu-system-riscv` | `brew install qemu` | `qemu-system-riscv64 --version` |
| RISC-V GCC + binutils | 任一被探测到的前缀 | `sudo apt install gcc-riscv64-linux-gnu` | `brew install riscv64-elf-gcc` | `riscv64-linux-gnu-gcc --version` |
| 宿主 `cc` | 支持 `-shared -fPIC` | `sudo apt install gcc` | Xcode CLT | `cc --version` |
| `clang-format` | **20 ≤ v < 23**（CI 为 21 系列） | `sudo apt install clang-format` | `brew install clang-format` | `clang-format --version` |
| `bc` | 任意 | `sudo apt install bc` | 自带 | `bc --version` |
| `perl` | 任意（生成 `user/usys.S`） | 自带 | 自带 | `perl -v` |
| `make` / `sed` / `objdump` / `id` / `expr` | 自带 | 自带 | 自带 | — |

几点必须知道的原因：

1. **QEMU ≥ 7.2 是强制的**：`make qemu` 依赖 `check-qemu-version`，它用 `bc` 比较版本，低于 7.2 直接报错退出（`MIN_QEMU_VERSION` 在 `Makefile`）。
2. **工具链前缀是自动探测的**：`Makefile` 依次尝试 `riscv64-unknown-elf-`、`riscv64-elf-`、`riscv64-none-elf-`、`riscv64-linux-gnu-`、`riscv64-unknown-linux-gnu-`，全都找不到才报错。所以 Debian/Ubuntu 与 macOS 用不同包名都能工作。
3. **宿主 `cc` 是真依赖**：`test-auth.py` 用 `cc -shared -fPIC` 编译 `common/auth.c`，再与 Python `hashlib` 的 PBKDF2 对拍。
4. **编译带 `-Wall -Werror`**：任何警告都会变成构建失败（`CFLAGS` 在 `Makefile`）。`mkfs` 是用**宿主 `gcc`** 编的，不是交叉编译器。

### `clang-format` 为什么必须落在 20–22

三条实测结论，选错版本会得到错误判断：

- **< 20**：`.clang-format` 里的 `ReflowComments: Never` 在 LLVM 20 之前是布尔选项，配置解析直接失败，整棵树无法格式化。
- **21.x**：与 CI 一致，全树 0 dirty —— 这是判断"格式是否合规"的唯一可信版本。
- **≥ 23**：会把已经对齐的连续 `#define` 组反向拆开，产生假阳性。不要用它判断，也不要据此改格式。

## 3. Python：版本要求与来源

**来源不限** —— conda、发行版包、pyenv、venv 都行，只要满足 3.1 的要求。版本与平台是硬约束，用什么方式提供解释器由你决定。

### 3.1 硬性要求（与来源无关）

| 项 | 要求 | 依据 |
| --- | --- | --- |
| 版本 | **3.12**（基准）；**绝对下限 3.11** | 宿主测试用 `unittest.enterContext`，3.10 及更早没有；本项目验收记录都跑在 3.12.14 上（见 `docs/stage3-validation.md`、`docs/stage4-validation.md`、`docs/integrity-audit.md`） |
| 平台 | **Linux 侧（WSL）的解释器**，不得是 Windows 解释器 | `test-xv6.py` 用 `socket.AF_UNIX` 连 QEMU monitor、直接 `make qemu`，并依赖 `/mnt/...` 路径语义 |
| 第三方包 | **零依赖**，只用标准库 | 四个测试脚本逐文件核对，非标准库 import = 0 |
| 能起外部进程 | 必须能 `subprocess` 起 `make`、`qemu-system-riscv64`、`cc` | 测试脚本就是这么调度的 |
| 本地进程 | 解释器必须是本机进程，不能是容器/远端包装 | 需要访问 `/mnt/d/HachileiOS` 并创建 UNIX 域套接字 |
| 权限 | 不需要 root | — |

**任何来源都要做到这一条**：让 `python3` 指向符合上述要求的解释器，或者显式用它的完整路径调用。同一个 shell 里存在多个 Python 时，不要靠猜。

### 3.2 各来源的等价做法（选一个即可）

| 来源 | 建立 | 让 `python3` 指向它 | 说明 |
| --- | --- | --- | --- |
| conda | `conda create -n onyxos python=3.12 -y` | `conda activate onyxos` | 环境要在 **WSL 内**创建；Windows 侧那份 conda 不可用（见 3.3） |
| 发行版包 | `sudo apt install python3.12 python3.12-venv` | 直接用 `python3.12 ...`；或调整 `PATH` / `update-alternatives` 让 `python3` 指向它 | Ubuntu 24.04 及以后默认 `python3` 就是 3.12；22.04 是 3.10，需要另装 |
| pyenv | `pyenv install 3.12.14` | `pyenv local 3.12.14` | 会生成 `.python-version`，**不要提交** |
| venv | `python3.12 -m venv .venv`，再 `source .venv/bin/activate` | 激活后 `python3` 即为它 | 本项目零依赖，venv 可用但非必需 |

无论选哪个：**不要把环境目录或版本文件提交进仓库**（`.venv/`、`.python-version`、conda 环境目录）。本仓库的 `.gitignore` 只忽略构建产物与 `.workbuddy/`，这些都会以未跟踪文件的形式冒出来。

### 3.3 必须避免

1. **Windows 侧的解释器** —— 本机的 `D:\miniconda`（base 是 3.14）、`D:\python 3.12.10`、`Python314` 都是 Windows 构建。在 WSL 里调 `/mnt/d/.../python.exe` 走的是 interop，进程实际是 Windows 进程：UNIX 域套接字连不上、`/mnt/d` 路径语义也会出错，而且版本不符合要求。
2. **`pip install` 任何东西**，或新增 `requirements.txt` —— CI 不执行 `pip install`，多出来的依赖在 CI 上直接缺包。
3. **依赖标准库某个小版本的特有行为** —— CI 的 `python3` 来自 `ubuntu-26.04` 运行器，未钉小版本。

### 3.4 动手前的自检

```sh
which python3
python3 -c "import sys; print(sys.executable); print(sys.version)"
# 期望：Linux 侧路径（/usr/bin/... 、~/.pyenv/... 、.../envs/onyxos/bin/... 之一），版本 3.12.x
```

再确认环境真的够用：

```sh
python3 test-harness.py      # 宿主错误路径
python3 test-auth.py         # PBKDF2 与 hashlib 对拍（会调用 cc）
python3 test-stage3.py       # 登录/退避/改密/重启持久性/无效镜像
./test-xv6.py dedicated      # 11 个专用自检程序
```

## 4. 构建与验证

只有 `make` 一个入口。**以下命令在 WSL 里敲**：

```sh
make clean && make && make fs.img     # 冷构建；make 会自动重建 fs.img
make qemu                             # 启动（CPUS 默认 3，128 MiB，-nographic）
```

进入 xv6 之后（**以下命令在 guest 里敲，不是在 WSL**）：先登录（`root` / `root`），再执行客机程序。

与 CI 对齐的完整验证链（CI 就是按这个顺序跑的）：

```sh
make fmt && git diff --exit-code      # 格式关：格式化后必须无差异
python3 test-harness.py
python3 test-auth.py
make kernel/kernel fs.img
python3 test-stage3.py
./test-xv6.py dedicated               # 11 个程序，含 privtest / mmaptest
./test-xv6.py usertests               # 全量回归，本机约 2–4 分钟（视核数与工具链）
./test-xv6.py crash                   # 日志/孤儿文件/孤儿目录
```

`./test-xv6.py` 支持 `--cpus {1,3}`（或 `CPUS=1`）与 `--repeat N`；
**`--repeat` 只重复，不会重试失败**，不要指望它把 flaky 压下去。

## 5. 改动的连带项

改了下面这些，必须同时处理右边一列，否则 CI 或镜像会坏：

| 改动 | 连带项 |
| --- | --- |
| 磁盘格式（`struct dinode`、`struct superblock` 布局） | 升 `FSMAGIC` 并**保留旧值** `FSMAGIC_OLD`，让 `fsinit()` 能说"旧 inode 格式，请重跑 mkfs"；同步 `mkfs/mkfs.c`。`sizeof(struct dinode)` 必须整除 `BSIZE`（`mkfs` 有断言，当前 64） |
| 新增用户程序 | `Makefile` 的 `UPROGS`、`user/help.c` 的命令索引、必要时 `test-xv6.py` 的 `DEDICATED` |
| 新增系统调用 | `kernel/syscall.h` 编号、`kernel/syscall.c` 分发表、`user/user.h` 声明、`user/usys.pl` 桩，四处一致 |
| 任何会变大镜像的改动 | **先算容量**：`FSSIZE` 3200 块、元数据 47 块、`writebig` 需要 267 块（`MAXFILE` 266 + 1 间接块）。`mkfs` 只挡单文件 `MAXFILE`，**不挡整体撑满** —— 撑满会静默写坏镜像 |
| 文档 | README/计划文档与代码同步；验收口径写进对应的 `docs/stage*-validation.md` |

**不要为了让测试通过而修改或放宽 `user/usertests.c` 与既有断言** —— 它是基线（路线图与阶段四记录都明确写了这一点）。

## 6. 行尾与格式

- **仓库里存 LF**，本机 `core.autocrlf=true`，所以**工作区是纯 CRLF**。
- 用脚本往 CRLF 文件插入多行文本时，**必须把 `\n` 转成 `\r\n`**。否则不只是行尾混杂：`clang-format` 会把 `\r` 算进列宽，报出指向注释的格式违规，极难联想到真实原因。
- 核实行尾要用二进制读（`grep -c $'\r$'` 不可靠）。
- 新建文件后立刻确认：`CRLF > 0 且 LF_only == 0`。

## 7. 提交与交付

- **一个批次一个分支**，分支名 `batchN-<主题>`；上游自身用 `codex/*`，两套别混。
- 提交说明要含四件事：**问题 / 行为变化 / 测试结果 / 仍有限制**（路线图 §7.5）。
- 走 fork → PR → 上游 `main`。**推 fork 就会触发 CI**（三 job：`macos` 只构建、`linux (1)`、`linux (3)`）。
- 判断"上游最新是什么"：看 `main`，不看分支 —— 分支只在它的 PR 未合并期间领先。用
  `api.github.com/repositories/1375477284/compare/main...<branch>` 的 `ahead_by` 判断。
- **不要在 `main` 上直接提交**：那样本地 `main` 就不再能与上游 `merge --ff-only`。

## 8. 开发机注意事项（描述本机，不是仓库的硬要求）

- 开发机是 Windows + WSL，项目在 WSL 的 `/mnt/d/HachileiOS`（Windows 侧为 `D:\HachileiOS`）。
- **Windows 侧没有 RISC-V 工具链**（无交叉 gcc / qemu / make），所以构建与测试**只能由人在 WSL 里做**；代理这一侧只能静态审查，不要声称"已验证通过"。
- 代理的 Bash 命令**可能被执行两次**：脚本要写成幂等，并且**只以文件里的实际内容判定成败**，不要只看退出码。
- `.git/refs/remotes/**` 在本机写不进去：不要依赖远程跟踪引用。看上游用 `FETCH_HEAD` 或 `git ls-remote`；
  `git fetch` 也**拒绝写入当前检出的分支**，需要拉取时先取到临时分支再 `merge --ff-only`。
- `git checkout` / `git branch` 之后**核对文件数**（`ls <dir> | wc -l` 与 `git ls-files <dir> | wc -l`），本机出现过工作区文件丢失。
- 本机可用的 Python（仅用于跑辅助脚本，不能用来跑本仓库的测试）：由工具管理的解释器与 venv；**这些不是本仓库的 Python 来源**，见 §3。
