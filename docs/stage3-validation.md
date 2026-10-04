# 阶段三验证接口与结果

日期：2026-10-03。实施基线为 main `9b42dfd`，补齐批次 11 的第四、五批，并保留前三批的身份和 inode 权限实现。远端最终结果以本变更的 GitHub Actions 为准。

## 环境与入口

本地 macOS ARM64，RISC-V GCC 16.2.0、QEMU 11.1.2、Python 3.12.14；格式化与 Linux CI 使用 clang-format 21 系列。宿主 Python 3.9 不支持 `unittest.enterContext`，宿主测试要求 Python ≥ 3.11。本地使用 `python3.12`，以下 `python3` 均指符合版本要求的解释器。

```sh
make kernel/kernel fs.img
python3 test-harness.py
python3 test-auth.py
python3 test-xv6.py dedicated --cpus 1
python3 test-xv6.py dedicated --cpus 3
python3 test-xv6.py usertests --cpus 1
python3 test-xv6.py usertests --cpus 3
python3 test-xv6.py crash --cpus 1
python3 test-xv6.py crash --cpus 3
python3 test-xv6.py privtest --cpus 1 --repeat 10
python3 test-xv6.py privtest --cpus 3 --repeat 10
python3 test-stage3.py --cpus 1
python3 test-stage3.py --cpus 3
```

这些测试会重建 `fs.img`，有数据时先备份。同一 checkout 共享镜像，不得并发运行 QEMU 测试。串口日志、命令、CPU 配置、提交和工作区状态保存在 `test-results/`，该目录不进入版本控制。CI 在两个 CPU 配置中运行宿主测试、参考哈希、真实登录、专用集合、完整 usertests 和崩溃恢复，并上传日志。

## 验收映射

| 路线图要求 | 实现与验证证据 |
| --- | --- |
| UID/GID、特权身份、继承 | `idtest`：降权、禁止提权和切换其他身份、边界值、fork/exec 保留身份；真实登录分别得到 UID 1001/1002 |
| 文件和目录权限 | `permtest`：owner/group/other 不回退、目录读/搜索/修改、执行位；实际 alice/bob 会话双向拒绝读取、写入，拒绝越权删除；重启后检查内容、UID 和 0600 权限 |
| 进程与设备控制 | `privtest`：两不同 UID 同时存在，重复拒绝 kill/signal/killpg/setprio/setpgid/tcsetpgrp，验证同 UID 和 root 正向操作、混合组只投递有权限成员、普通用户 mknod/chown/ttyecho 拒绝 |
| 账户与认证 | login 始终认证，密码不回显；错误口令、未知用户、超长口令、确认不一致拒绝；连续失败 1/2 秒退避，Ctrl-C 不能缩短等待；root 改密后旧口令拒绝，新口令重启后仍可用 |
| 密码存储 | 每账户独立 16 字节宿主随机盐；PBKDF2-HMAC-SHA256 10,000 轮；18 组密码/盐组合与 Python hashlib 完全一致，涵盖 55/56/63 字节边界；数据库 root:0600，普通用户不能读取或给它建立硬链接 |
| 磁盘兼容性 | 实际将测试镜像 magic 改成旧值，启动明确拒绝；账户文件权限错误或截断损坏时拒绝登录，无 root Shell 回退；测试结束恢复镜像 |
| 统计和日志边界 | UID/GID 随 psinfo 返回，统计公开、klog 仅 root；普通用户 ps 可用；实际填充超过 56 个进程，验证第二快照页、两种 copyout 失败及临时页回收 |
| 资源与回归 | 失败路径循环 128 次；子进程均回收；原有 COW/内存/调度/信号/压力测试继续通过；非 root 前台 top 可被真实 Ctrl-C 终止且 Shell 保留身份 |

进程权限判断与目标操作持同一 `p->lock`。组成员可并发变化，因此投递时逐个重新判定 UID；组内至少一个允许成员被投递返回 0，无允许成员返回 -1。终端中断使用记录的前台 UID，不读取碰巧运行的进程身份。

`psinfo` 每项 72 字节，两页分别分配、失败逐一回收；不依赖物理连续性。账户数据库 212 字节，`passwd` 保持固定大小、不先 truncate，以一次 filewrite 日志事务更新；`/etc/pwlock` 防止并发改密。崩溃遗留锁需 root 检查并删除。

## 本地结果

| 集合 | CPUS=1 | CPUS=3 |
| --- | --- | --- |
| dedicated：10 个程序 | 全部通过 | 全部通过 |
| 完整 usertests（非 quick） | ALL TESTS PASSED | ALL TESTS PASSED |
| crash：日志、孤儿文件、孤儿目录 | 全部通过 | 全部通过 |
| privtest 连续 10 轮，无自动重试 | 10/10 | 10/10 |
| test-stage3：登录/持久性与无效镜像 | 全部通过 | 全部通过 |

宿主回归 21 项通过，PBKDF2 18 组独立参考组合通过。`usertests` 中故意触发的用户页错误、磁盘满、inode 耗尽是预期用例，最终以 `ALL TESTS PASSED` 为准。

本次还修复了 macOS 暴露的宿主清理问题：`crash()` 已停止 QEMU 后，退出上下文再次给同一进程组发信号可能得到 EPERM。`stop()` 现为幂等操作，并有专门的宿主回归；两种 CPU 的崩溃恢复实测通过。

## 实现边界

- 固定预置 root/root、alice/alice、bob/bob，口令公开，仅用于开发与测试；root 可改密，无账户增删、自助改密、setuid 位或 saved-uid。
- 哈希工作因子低于生产要求，非生产认证方案。客户机无安全随机源，每账户盐在构建时随机生成、改密时保留。
- 单终端，无完整 POSIX 会话、后台终端读 SIGTTIN 或跨登录会话的终端隔离；本阶段验收文件、进程控制与内核日志访问边界。
- 已打开的描述符继续有效，之后降权或 chmod 不撤销已有访问。进程统计公开，内核日志仅 root。
- 老 inode 镜像不自动迁移；同格式但缺少账户库也拒绝登录。重建前须自行保存旧数据。
- `FSSIZE=3200`、位图一块、日志/缓存大小未变。实际程序大小随工具链变化，容量通过完整磁盘满与写入回归验证。
