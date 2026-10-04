# AGENTS.md

本文件面向所有开发者与编码代理，仅规定共同的开发环境、验证与 Git 提交要求。
项目行为和设计约定见 [README.md](README.md) 与 [docs/README.md](docs/README.md)；构建与 CI 的实际入口见 `Makefile` 和 `.github/workflows/test.yml`。

## 1. 跨平台开发环境

| 平台 | 开发方式 |
| --- | --- |
| Linux | 在本机安装下列依赖，直接构建和测试。 |
| macOS | 使用 Xcode Command Line Tools 提供宿主编译器，通过 Homebrew 等方式安装其余依赖。当前 macOS CI 仅验证构建。 |
| Windows | 使用 WSL2 Linux 环境；Git、Python、编译器、QEMU 和构建命令均在 WSL 内运行，不混用 Windows 可执行文件。建议将仓库放在 WSL 的 Linux 文件系统中。 |

也可使用 Linux 虚拟机、容器或远程开发环境，但仓库、Python、构建工具和 QEMU 必须在同一环境内运行，并能创建和访问 UNIX 域套接字。个人路径、编辑器与环境管理方式不作为项目要求。

### 统一依赖

| 组件 | 要求 |
| --- | --- |
| RISC-V GCC + binutils | 支持 `rv64gc`；使用 `Makefile` 可自动探测的工具链前缀，或显式设置 `TOOLPREFIX`。 |
| QEMU | `qemu-system-riscv64` ≥ 7.2。 |
| 宿主 C 编译器 | 提供 `gcc`（构建 mkfs）和 `cc`（认证测试），后者支持 `-shared -fPIC`；macOS 可使用系统 Clang 提供的命令。 |
| Python | 推荐 3.12，最低 3.11；测试只使用标准库，无需安装第三方包。 |
| clang-format | 统一使用 21.x，与项目格式基准保持一致。 |
| 基础工具 | Git、GNU make、Perl、bc，以及 sed、grep、id、expr 等常用 POSIX 工具。 |

Linux 常用交叉工具链为 `riscv64-linux-gnu-`，macOS 常用 `riscv64-elf-`；安装方式不限，安装后须核对版本。不要假设包管理器的默认版本符合要求。

在实际用于构建的终端中检查：

```sh
command -v python3
python3 -c "import sys; print(sys.executable); print(sys.version)"
qemu-system-riscv64 --version
clang-format --version
make --version
cc --version
```

确保 `python3` 指向符合要求的解释器；也可在下列命令中显式使用 `python3.12` 或解释器完整路径。格式化工具不在默认 PATH 时，使用 `make fmt CLANG_FORMAT=/path/to/clang-format`。

## 2. 构建与验证

以下命令在仓库根目录的 Linux、macOS 或 WSL 终端执行：

```sh
make kernel/kernel fs.img
make qemu
```

需要冷构建时先运行 `make clean`；该操作会删除 `fs.img`，重建后镜像内的账户和文件恢复初始状态。

代码变更提交前执行格式化和相关测试。完整验证链如下；与 Linux CI 对齐时，分别在 `CPUS=1` 和 `CPUS=3` 环境下运行：

```sh
export CPUS=1                      # 再以 CPUS=3 重跑验证链
make fmt
python3 test-harness.py
python3 test-auth.py
make kernel/kernel fs.img
python3 test-stage3.py
python3 test-xv6.py dedicated
python3 test-xv6.py usertests
python3 test-xv6.py crash
```

格式化产生的必要变更应纳入提交；在干净提交上执行 `make fmt && git diff --exit-code` 应无差异。纯文档变更检查内容、链接与空白即可。

不要为了通过测试放宽 `user/usertests.c` 或既有断言。提交说明须如实区分已通过、失败与未执行的检查，并注明平台、CPU 配置和未执行原因；macOS 构建通过不能代替 Linux 完整回归。合并前以 PR 的 CI 结果为准。

## 3. 文件与 Git 提交要求

- 文本文件统一使用 UTF-8、LF 行尾，工作区也使用 LF，避免跨平台换行差异。建议在本仓库设置 `git config --local core.autocrlf false` 和 `git config --local core.eol lf`，并将编辑器设为 LF；修改配置不会自动转换已有文件，不要夹带全仓库行尾重写。
- C 代码遵循 `.clang-format`，不提交无关格式变更。
- 不提交构建产物、测试输出、密钥、个人绝对路径、编辑器配置或本地环境文件（如 `.DS_Store`、`.vscode/`、`.venv/`、`.python-version`）。个人忽略规则可写入 `.git/info/exclude` 或全局 Git ignore。
- 从上游最新 `main` 创建独立分支，一个分支处理一个明确任务；不要直接在 `main` 上提交。普通任务使用 `codex/<主题>`，已有批次任务可沿用 `batchN-<主题>`。
- 通过自己的 fork 推送分支，再向上游 `main` 提交 PR；不绑定特定贡献者的 fork，也不强制本地远程名称。同步 `main` 使用快进合并，保留其他人的工作，未经协商不覆盖共享分支历史。
- 每次提交保持改动集中；提交说明包含 **问题、行为变化、测试结果、仍有限制**。没有行为变化或限制时明确写明。行为或接口发生变化时同步相关文档。
- 提交前检查 `git status --short`、`git diff --check` 和 `git diff --cached`，确认暂存区仅包含本次任务的文件；PR 合并前通过现有 Linux 单核/三核测试与 macOS 构建检查。
