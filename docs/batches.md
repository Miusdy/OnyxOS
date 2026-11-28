# 批次实施记录

本文件按提交顺序记录每个实施批次的原始设计、改了哪些文件、以及为什么那样取舍。
它从仓库首页 `README.md` 拆出，以免首页被历史细节淹没；功能总览仍在
[README](../README.md)，能力边界与验收入口见 [开发文档索引](README.md)。

批次编号是提交历史的顺序编号，与路线图里的“阶段”不是同一套编号，两套编号不能直接对应，批次编号的空缺也不表示漏合并。

### 批次 1 — 进程与内存可观测性

**`psinfo()` 系统调用（`SYS_psinfo = 23`）**

- 原型：`int psinfo(struct psinfo *buf, int max);`
- 在内核中**一次性快照整个进程表**到两个独立的 `kalloc` 页，释放锁后按实际条目数 `copyout` 给用户，返回写入的条目数。
- 布局定义在 `kernel/psinfo.h`，被内核和用户态**共同包含**，因此两边的结构体布局必须完全一致。

| 字段 | 含义 |
| --- | --- |
| `uid` / `gid` | 进程身份与主组；统计对所有身份可见 |
| `pid` / `ppid` | 进程号 / 父进程号 |
| `state` | `PSTATE_UNUSED` … `PSTATE_ZOMBIE`（包含 `PSTATE_STOPPED`，数值与 `enum procstate` 对齐） |
| `sz` | 虚拟内存大小（**不是**常驻内存 RSS） |
| `rss` | 常驻内存大小：`va < sz` 的已映射页数 × `PGSIZE`（批次 4 加入） |
| `u_ticks` / `k_ticks` | 用户态 / 内核态累计 tick（批次 3 加入） |
| `name[16]` | 进程名，保证 NUL 结尾 |

`struct psinfo` 现为 72 字节。每个 4096 字节页放 56 条记录，最多 64 个进程需要两页；内核使用编译期断言保证容量。两页不要求物理连续，用户缓冲区仍是连续的记录数组。第二页分配失败及任一次 `copyout` 失败都会释放已分配的页。

**`freemem()` 系统调用（`SYS_freemem = 24`）**

- 原型：`uint64 freemem(void);` —— 返回空闲物理内存字节数。
- 实现为 O(1)：`kernel/kalloc.c` 中的 `kmem.nfree` 计数器在 `kalloc`/`kfree` 时增减。
- **参考实现** `freemem_walk()` 以 O(N) 遍历空闲链表，用于校验 O(1) 计数器（`free` 不直接使用它，它是对照组）。

**`ps` / `free` 程序**

- `ps` 把状态码翻译成可读字符串（`sleep` / `run` / `zombie` …）。
- `free` 用 `kernel/minios.h` 的 `MINIOS_MEM_TOTAL` 计算"已用"，以 字节 / KB / 页 三种单位输出。

---

### 批次 2 — 内核日志与系统概览

**无锁日志环形缓冲区**（`kernel/printk.c`）

- 16 KiB 环形缓冲区，`printk` 的**每一个字符**都被复制一份进去。tee 点位于 `printk.c` 内部的 `kputc()`，**不在** `consputc()` —— 因此控制台的**输入回显不会被记入日志**。
- 刻意做成**无锁**：`panic()` 必须在不能获取任何锁的情况下也能记录日志。
- 写者先写字节、再用 **release** 存储发布写索引；读者用 **acquire** 加载索引，保证读到的字节已写完。

```c
static void
klog_putc(int c)
{
  uint64 w = __atomic_load_n(&klog_w, __ATOMIC_RELAXED);
  klogbuf[w % KLOGSIZE] = c;
  __atomic_store_n(&klog_w, w + 1, __ATOMIC_RELEASE);
}
```

**`klog()` 系统调用（`SYS_klog = 25`）**

- 原型：`int klog(char *buf, int max, uint64 *seq, uint64 *lost);`
- `*seq` 是**输入/输出游标**：绝对单调递增的字节索引，由用户态保存，因此**内核不需要为每个进程维护任何读取状态**。
- 游标被环形缓冲区覆盖时，通过 `*lost` 报告"永远看不到的字节数"。
- 读取时**不持锁**跨 `copyout`（`copyout` 可能触发缺页 → `kalloc` → `kmem.lock`）。

**程序**

- `dmesg` —— 用 in/out 游标分块排空环形缓冲区；有界轮数（`MAXROUNDS`）保证在内核持续打印时也能终止。
- `top [n]` —— 每轮 ANSI 清屏刷新，显示进程表 + 空闲内存 + uptime，默认持续刷新，可用 Ctrl-C 结束；指定 `n` 时刷新 n 次后退出。
- `neofetch` —— 一次性打印架构、CPU 上限、内存、进程数、uptime。
- `kernel/minios.h` —— 内核与用户态共享的系统常量（`MINIOS_MEM_TOTAL`）。

---

### 批次 3 — CPU 时间统计

**内核侧**（`kernel/trap.c` 的 `clockintr()`）

定时器中断是**每个 hart 各自触发**的，而全局 `ticks` 只在 hart 0 上递增，因此不能用它来归属 CPU 时间。我们在每次时钟中断时，把这一格记到**当前 hart 上正在运行的进程**头上：

```c
struct proc *p = myproc();
if (p) {
  if (r_sstatus() & SSTATUS_SPP)
    __atomic_fetch_add(&p->k_ticks, 1, __ATOMIC_RELAXED);
  else
    __atomic_fetch_add(&p->u_ticks, 1, __ATOMIC_RELAXED);
}
```

- **用户态 / 内核态拆分**：用 `sstatus.SPP` 判断陷入来源（0 = 用户态，1 = 监督态）。
- **不需要 `p->lock`**：计数器只有一个写者（当前运行 `p` 的那个 hart），读者只需要一个单调快照，因此用 **relaxed atomic** 即可；在定时器中断里加自旋锁纯属浪费。
- **槽位复用要清零**：`allocproc()` 在 `found:` 处把两个计数器清零，否则回收的进程槽会继承上一个进程的时间。`kfork()` 不复制这两个字段，子进程从 0 开始。

**用户侧**

- `ps` 增加 `usr` / `sys` 两列（累计 tick）。
- `top` 额外维护上一轮快照，按 pid 匹配后输出增量 `dcpu` 与 `busy% = (du+dk)×100/elapsed`。pid 被复用时计数器会倒退，此时丢弃基线并显示 `-`。
- `cputest` —— 两阶段自检程序，把进程自身的时间统计与全局 `uptime()` 交叉验证：
  - **阶段 1（用户态密集）**：在用户态批量自旋，断言 `du ≈ elapsed`。
  - **阶段 2（内核态密集）**：批量调用较重的 `psinfo()`，断言 `sys` 占主导（用户程序永远无法达到 100% 系统时间，因为发起系统调用的循环本身就是用户代码）。

---

### 批次 4 — 补齐三个观测盲点

批次 4 不引入任何新机制，只是把批次 1–3 已经铺好的骨架填完整，所以每一项都能独立验证。

**RSS：驻留内存大小**

`ps` / `top` 原先只能报 `sz`，即 `growproc()` 交出去的**虚拟**地址空间；在惰性分配下它会远大于实际占用。`struct psinfo` 新增 `rss` 字段，由 `kernel/vm.c` 的 `vm_rss()` 计算。

```c
uint64
vm_rss(pagetable_t pagetable, uint64 sz)
{
  uint64 acc = 0;
  if (pagetable == 0 || sz == 0)
    return 0;
  rsswalk(pagetable, 2, 0, PGROUNDUP(sz), &acc);
  return acc;
}
```

三个决定都能追溯到成本与正确性：

- **遍历页表树，而不是逐页调用 `walk()`。** 惰性 sbrk 下 `sz` 可以很大而几乎没有映射，按虚拟页循环的成本正比于 `sz / PGSIZE`；像 `freewalk()` 那样下降三级页表，成本只正比于真正映射的页数加上承载它们的页表页。
- **只统计 `va < sz` 的叶子。** 因此位于地址空间顶端的 trampoline 与 trapframe 页被自然排除；用户栈在 `sz` 之下，属于 RSS；栈保护页未被映射，自然不计入。
- **调用者持有 `p->lock`**（`psinfo()` 正是如此），因此遍历期间页表不会被拆掉；本函数只读。

> 阶段三加入 UID/GID 后，进程快照已扩展为两页，见前面的 `psinfo()` 接口说明。

**`waitx()` 系统调用（`SYS_waitx = 26`）**

- 原型：`int waitx(int *status, uint64 *utime, uint64 *ktime);`
- 语义等同 `wait()`，额外回填子进程整个生命周期的用户态 / 内核态 tick。三个指针都可以传 0。
- 子进程账目在 `kexit()` 中**冻结**到 `xutime` / `xktime`（位置紧邻既有的 `xstate`），由 `kwait()` 在仍持 `p->lock` 时读出。之所以必须冻结：`freeproc()` 会在父进程收割的瞬间把整个槽位清零，之后就无值可读。
- 冻结点刻意放在退出路径**全部清理之后**：`fileclose()` 与 `iput()` 都在内核态执行并已计入 `k_ticks`，父进程理应看到这部分开销。
- `allocproc()` 同时清零这两个字段，与 `u_ticks` / `k_ticks` 采用同一条理由：回收的槽位不得继承上个进程的账目。

**`fsinfo()` 系统调用（`SYS_fsinfo = 27`）**

- 原型：`int fsinfo(struct fsstat *st);` —— 布局定义在共享头 `kernel/fsstat.h`。
- 块用量是 **O(1)**：`balloc()` / `bfree()` 处增减 `fs_nused_blocks`，`ialloc()` / `ifree()` 处增减 `fs_nused_inodes`。这与 `kmem.nfree` 是同一套思路。
- **启动时用 `fscount_scan()` 校准一次**，把读数直接从磁盘扫出来的真值灌进计数器。计数器只维护增量，标定不可省；事务回滚可以让它偏高一格，重启即可清除。
- 同理保留 **O(N) 参考实现 `fscount_walk()`**，用于在之后校准/对照——与 `freemem_walk()` 的角色一致（它被导出但不被任何生产路径调用）。
- 用 relaxed atomic 而非自旋锁：分配点本身已在各自的锁（inode 锁、日志锁）之内，加锁反而有锁序反转风险。
- `total` 用 **`sb.size`（整盘块数）而非 `sb.nblocks`（数据块数）**：mkfs 把 boot / super / log / inode / 位图块也在同一张位图里标记为已用，用 `nblocks` 会重复扣减元数据并少报可用空间。

---

### 批次 5 — 系统信息与运行时计数器

批次 5 先补齐批次 4 遗留的两个口径问题（`df` 的元数据口径、编译期 hart 数），再把新增的**事件计数**统一到一个系统调用下。

**`sysinfo()` 系统调用（`SYS_sysinfo = 28`）**

- 原型：`int sysinfo(struct sysinfo *info);` —— 布局在共享头 `kernel/sysinfo.h`。
- 与 `struct psinfo` 的关键差别：它是**单个结构体而非进程表快照**，因此**没有页大小约束**，可以随时追加字段。

| 字段 | 来源 |
| --- | --- |
| `ncpu_online` | 每个 hart 进入 `scheduler()` 前自增的计数器（`kernel/proc.c`） |
| `ncpu_max` | 编译期上限 `NCPU` |
| `mem_total` / `mem_free` | `PHYSTOP - KERNBASE` / `freemem()` |
| `disk_reads` / `disk_writes` | `virtio_disk_rw()` 入口，按读写分流（`kernel/virtio_disk.c`） |
| `bcache_hits` / `bcache_misses` | `bget()` 命中分支 / 需要取空槽的分支（`kernel/bio.c`） |
| `vmfaults` | `vmfault()` 中 `mappages()` 成功之后（`kernel/vm.c`） |

- **所有计数器都是 relaxed atomic，理由相同**：计数点已经在各自的临界区内（`bget()` 在 `bcache.lock` 内），而读者只要一个单调快照，在此加锁只会引入锁序反转风险。
- **`vmfaults` 计在成功映射之后**，因此它等于"新映射的页数"，可以直接与 `rss` 的增长对照；计在函数入口则会把 `va >= psz` 与 `ismapped()` 这两种无效调用也算进去。
- **`ncpu_online` 报的是"已进入调度器"的 hart 数**，不是 `-smp` 配置值：hart 0 因先完成设备初始化而最后到达。观测真实在线比回显一个编译期常量更有价值。

**`nmeta`：`df` 的数据块口径**

`struct fsstat` 增加 `nmeta`，由 `fsinfo()` 用 `sb.size - sb.nblocks` 得出 —— 这正是 mkfs 自己的算法（`nblocks = FSSIZE - nmeta`），复用它比在核心里重算 inode 块与位图块数更不容易漂移。`df` 现在同时打印"整盘"与"数据块"两组读数。

> 两种口径的 `free` 值必然相同：元数据块在同一张位图里被标记为已用，空闲块不可能落在元数据区。这不是打印错误。

**验证上的一个新情况**

`disk_reads` / `bcache_hits` 这类**事件计数没有可扫描的 O(N) 真值**，因此无法沿用 `freemem_walk()` / `fscount_walk()` 那套对照法，只能靠同层恒真不变式（`hits + misses == bget()` 调用次数）与行为对照。这是本项目首个无法用参考实现校验的能力。

---

### 批次 6 — 调度器优先级

批次 5 为止，本项目新增的每一个系统调用都是**只读**的。批次 6 引入第一个**写型**系统调用，并第一次改动调度语义 —— 这使它不同于前面所有批次：观测能力只读内核状态，随时可以整体回滚；而调度策略会改变每一个进程的运行机会。

**`setprio()` 系统调用（`SYS_setprio = 29`）**

- 原型：`int setprio(int pid, int prio);` —— **数值越小越紧急**，范围 `PRIO_HIGHEST`(0)..`PRIO_LOWEST`(9)，常量定义在共享头 `kernel/psinfo.h`。
- **权限检查**：阶段三已将优先级修改限制为同 UID 或 root，在目标进程锁内检查。
- **fork 不继承优先级**：`kfork()` 是逐字段复制而非 `*np = *p`，所以新进程从 `PRIO_DEFAULT`(5) 开始。这让"每个进程起点平等"成为可预测的语义，也让优先级只能通过显式 `setprio` 改变。

**调度器：单遍择优 + 老化**

`struct proc` 增加两个字段：`prio`（静态，由用户设置）与 `cur_prio`（动态，调度器实际比较的值）。`scheduler()` 从"取第一个 `RUNNABLE`"改为"取 `cur_prio` 最小者"，并在**同一次扫描**中给每个仍可运行却未被选中的进程把 `cur_prio` 减一 —— 这就是老化。

每次扫描会降低可运行进程的 `cur_prio`，被选中的进程在切换前将其重置为静态 `prio`。持续等待的进程因此逐渐提高调度优先级，`PRIO_AGING_FLOOR` 限制下降范围。实际等待还取决于可运行进程数、并列顺序和各核扫描时机，不能承诺每 10 轮必然获选或固定的实时性上界。

三个值得写下来的工程决定：

1. **为什么老化不是可选项**：`kernel/trap.c` 每个 tick 都调用 `yield()`，因此调度器每 tick 重新择优。纯静态优先级意味着低优先级进程只能在高优先级进程全部阻塞时才运行 —— 在 `usertests` 那种并发子进程密集的负载下，这会直接表现为间歇性失败。
2. **择优与老化合并成一遍**：唯一后果是"被选中者自己也被减了一"，而它在切换前会被重置，所以结果与两遍扫描等价，却省掉一次 O(NPROC) 遍历。
3. **`best` 指针必须重新校验**：`proc` 数组是静态的，指针始终有效；但选中后必须**重新持锁确认 `state == RUNNABLE`** —— 该进程可能在这一遍扫描期间睡眠、退出，甚至其槽位已被回收复用。这是 `scheduler()` 里唯一容易写错的地方。

**自检**

`priotest` 让两个子进程各在用户态自旋相同的 tick 数，父进程把其中一个设为最紧急、另一个设为最松弛，再用 `waitx()` 比较两者的 CPU 时间。它同时断言两件事：紧急进程必须明显领先（优先级生效），且松弛进程必须拿到 CPU（老化生效）。**只看前者无法区分"调度正确"与"低优先级被饿死"。**

---

### 批次 7 — 页状态表与分配器自检

`kalloc()` 原有的防护只看**参数**：指针是否页对齐、是否落在 `[end, PHYSTOP)` 之内。它看不见**所有权**错误 —— 同一页被 `kfree()` 两次时，该页会被同一张空闲链表串两次，之后 `kalloc()` 会把同一块内存交给两个不同的调用者，而现场毫无提示。批次 7 用一张每页 1 字节的状态表，把这种静默损坏变成即时 `panic`。

**状态表**（`kernel/kalloc.c`）

- `page_state[NPAGE]`，其中 `NPAGE = (PHYSTOP - KERNBASE) / PGSIZE` = 32768，即 **32 KiB**。
- 只有两个取值：`PAGE_FREE`（在空闲链表上）与 `PAGE_ALLOC`（已交出、未归还）。
- 两侧互为镜像：`kfree()` 要求当前是 `PAGE_ALLOC`，`kalloc()` 要求取出的页是 `PAGE_FREE`；任一不符即 `panic`。
- **成本与取舍**：32 KiB 是被描述内存的 0.025%。这是**调试设施**，生产内核不需要它 —— 保留它是本项目的刻意选择，换取"双重释放导致链表损坏"从静默故障变成立即停机。
- **不需要原子操作**：`page_state` 只在持有 `kmem.lock` 时被读写（`kalloc()`/`kfree()` 本就在锁内操作链表），因此它不引入新的同步开销。
- `kinit()` 先把整张表置为 `PAGE_ALLOC` 再调用 `freerange()`：于是"释放一个从未分配过的页"就只剩 `kinit` 自己这一次**合法**的 `ALLOC→FREE` 迁移，不必在 `kfree()` 里开特例。

**计数器与自检**

`kmem` 增加 `kalloc_calls` / `kfree_calls` 两个累计计数，`kinit()` 在 `freerange()` 之后清零，所以差值表示"自启动以来交出的页数"。`kalloc_stats()` 经 `sysinfo()` 导出三个新字段：

| 字段 | 含义 |
| --- | --- |
| `pages_total` | 全部物理页 = 32768，**含**从不归 `kalloc` 管的内核映像页 |
| `pages_live` | 已交出未归还的页数 = `kalloc_calls - kfree_calls` |
| `alloc_calls` | 累计 `kalloc()` 成功次数 |

`kmemtest` 交叉检查**两条互相独立**的会计路径：

1. **守恒律**：`mem_free`（来自 O(1) 的 `nfree` 计数）与 `pages_live`（来自调用计数）之和是一个常量 —— `nfree + live` 恒等于内核映像之后的页数 —— 所以分配前后必须完全不变。
2. **泄漏检测**：子进程 `sbrk` 64 页后退出，`pages_live` 必须回落到基线（容差 4 页）。

> ⚠️ 守恒律只能用**增量**判断，不能写成 `pages_live == pages_total - pages_free`：`pages_total` 含内核映像页而 `nfree` 不含，两者起点不同。这是本批次最容易写错的一处，`kmemtest.c` 的注释里也写明了。

**无法从用户态测试的部分**：双重释放检测会 `panic`，触发即整机停机，所以它只能靠代码审查与"临时改坏一处再跑"来验证。

---

### 批次 8 — 写时复制 fork

原先的 `uvmcopy()` 在 fork 时逐页 `kalloc` + `memmove`：父进程有多少页，fork 就复制多少页。批次 8 让 fork 改为**共享父进程的物理页并把它们标记为只读**，第一次写才复制。批次 7 的那张每页状态表在这里从"两态标志"升级为**引用计数** —— 正是它当初被留下的理由。

**引用计数**（`kernel/kalloc.c`）

| 值 | 含义 |
| --- | --- |
| `0` | 在空闲链表上 |
| `1` | 已交出，独占 |
| `n > 1` | 被 n 个页表映射 |

- `kalloc()` 要求取出的页计数为 `0` 并置为 `1`；`kfree()` 要求计数非 `0`，减一后**只有归零才真正回链表**。已经归零的页再次释放会立即 `panic`；共享页只记录总引用数，不能识别某个持有者重复释放自己的引用，引用仍大于零时不保证立即报错。
- `kref()` 在 fork 使用的 `uvmcopyrange()` 里被调用：把引用计数加一。它 `panic` 而不让 `uchar` 回绕；`NPROC = 64` 保证一页最多被 64 个进程共享，所以 1 字节够用，不必用 `int` 让每页多占 3 字节。
- 计数器语义随之修正：`pages_live` 现在是 `kalloc_calls − pages_released`，**不是** `− kfree_calls`。共享页的 `kfree()` 只减引用、不回链表；若仍按 `kfree_calls` 计，`live` 会一路向下漂移，`mem_free + live` 这条守恒律就不再成立（`kmemtest` 正盯着它）。

**写时复制**（`kernel/vm.c`、`kernel/trap.c`）

- `uvmcopy()` 对**可写**页：先 `kref()`，再在子进程页表里以 `PTE_COW` 且清 `PTE_W` 映射，最后清掉父进程页表里的 `PTE_W`。对**只读**页（exec 载入的正文段）：只 `kref()`，不标 `PTE_COW` —— 写它仍然是错误，不会悄悄换来一份可写副本。
- 两侧都清掉 `PTE_W` 是这套方案的核心不变式：**只要引用计数 ≥ 2，就没有任何人能就地写这一页**。写触发 `scause = 15`，`cowfault()` 分配一份私有副本、改写 PTE，再 `kfree()` 掉本进程持有的那个引用。
- **内核也是写者**：`copyout()` 写入用户内存，因此必须走同一条路。若只处理用户态缺页，`read()`、`sysinfo()` 这类系统调用会把字节直接写进父进程的页里，fork 承诺的隔离就白给了 —— 这是最容易被漏掉的一处。
- `PTE_COW` 取 `1L << 8`：Sv39 把 PTE 的第 8、9 位留给软件。它在 `PTE_FLAGS`（`0x3FF`）之内，因此 `uvmcopy()` 复制 flags 时会顺带把它带过去 —— 这正是"一个已被共享的页再被 fork 时仍能继续 COW"的原因。

**独占页不拷贝**

`cowfault()` 里有一条快捷路径：若引用计数已是 `1`，说明共享它的那个进程已经退出、这一页只属于当前进程，那就把 `PTE_W` 加回去，一个字节也不必拷。少了它，shell 每跑完一条命令后第一次写自己的每个页都会白拷一次，恰恰是 COW 想省掉的开销。读取计数与改写 PTE 不是原子的，**也不需要是**：一个页只可能在**本进程被 fork** 时增加引用，而 xv6 的进程是单线程的，此刻不可能发生；并发的退出只能让计数下降，而要释放一页必须经过 0，我们还持有映射，它到不了 0。

**可观测性**

`struct sysinfo` 追加 5 个字段（它没有页大小约束，加字段是安全的；批次 5 的那张字段表只覆盖当时交付的 9 个）：

| 字段 | 来源 |
| --- | --- |
| `pages_shared` | O(1) 计数器：引用计数 ≥ 2 的页数 |
| `pages_shared_ref` | **同一个量的 O(N) 参考实现**，在**同一个** `sysinfo()` 调用里扫一遍状态表 |
| `kref_calls` | `kref()` 累计调用次数（fork 带来的引用总数）|
| `cow_faults` | 被 `cowfault()` 解决的写缺页数 |
| `cow_copies` | 其中真正拷了一页的次数 |

`pages_shared` 与 `pages_shared_ref` 是本项目第三组"O(1) 计数器 vs O(N) 参考实现"（前两组是 `freemem`/`freemem_walk`、`fsinfo`/`fscount_walk`），区别在于这两个数由**同一次系统调用**返回，因而可以互相比较 —— 分成两次调用会引入时间窗。代价是每次 `sysinfo()` 都要在持 `kmem.lock` 时扫 32 KiB 状态表，`neofetch`、`kmemtest`、`cowtest` 和 `priotest` 会承担这个代价。`cow_faults` 与 `cow_copies` 的差值本身即观测量：它统计"因对方已退出而省掉的拷贝次数"。

**自检**

`cowtest` 用已有的观测设施验证四条断言，而不是靠肉眼：

1. **fork 的代价**：先把 32 页读起来，再 fork 一个子进程；子进程报告自己已就绪后**阻塞**在管道上，父进程在"子进程确实活着"的时刻测 `mem_free`。复制式的 fork 会掉 32 页以上，实测应在 16 页以内（页表 + 内核栈 + trapframe）。
2. **写隔离**：子进程写满 32 页后，父进程必须仍读到自己的字节；子进程侧 `cow_copies` 至少增加 32。
3. **copyout 也走 COW**：子进程用 `read()` 把 4 KiB 读进一个与父进程**共享**的页 —— 若 `copyout()` 漏了这条路径，父进程的页会被写脏。
4. **三代共享**：孙进程写满全部页，父与子都必须看到自己的字节。

每一步都断言 `pages_shared == pages_shared_ref`，并在子进程退出后断言共享页数回到基线 —— 引用计数漏减会比内存泄漏更隐蔽。

三个值得写下来的工程决定：

1. **失败路径比成功路径难写**：`uvmcopy()` 中途失败时不能简单回滚。已经 `kref()` 过的页要 `kfree()` 回去，而父进程的 `PTE_W` 只有在该页计数确实回到 `1` 时才能恢复 —— 若它正被更早的一次 fork 共享着，恢复成可写就等于把那个孩子的页也交了出去。
2. **`PTE_COW` 留在单引用的页上是合法状态**：子进程退出后父进程的页仍是"只读 + COW"标记，直到下一次写被 `cowfault()` 的快捷路径修好。这个状态不产生任何拷贝，所以不是代价，但读 PTE 的代码不该假设"只读页一定没有 `PTE_COW`"。
3. **守恒律需要重新对齐口径**：把 `pages_live` 从"`kfree()` 调用次数"改成"真正回链表的次数"不是美化，而是让第二章那套校验继续成立的必要条件；计数器的定义跟着语义走。

### 批次 11 — 用户身份与权限

路线图阶段三的五个实施批次。它和前面所有批次的性质不同：**这是第一次改动磁盘格式**，而 `fs.img` 是持久状态，改错了不是重新编译能修的；同时它引入的是**安全语义** —— 漏一处检查的症状不是崩溃，而是静默的越权：程序照常运行，只是不该读的读到了。因此每一步都要求配对的负向用例，只测"正向能用"等于没测。

**身份字段与四个系统调用**（`SYS_getuid` = 40、`SYS_getgid` = 41、`SYS_setuid` = 42、`SYS_setgid` = 43）

`struct proc` 增加 `uid`/`gid`；`kfork()` 显式继承、`kexec()` 不重置，所以身份跨 fork/exec 保留。`setuid()`/`setgid()` 只有一条规则：uid 0 可设任意值，其他进程只能"设成自己已有的值"（no-op 形式仍然合法）。**降权单向不可逆** —— 没有 saved-uid，也没有 setuid 位，降权之后回不去。

`setgid()` 单独成一个调用，而不是"组跟随用户"：若 `gid` 恒等于 `uid`，权限模型的 group 一档只会是 owner 档的副本、永远授不出新的权限，等于死代码。两者都要改时，顺序是**先 `setgid` 后 `setuid`**。

**磁盘格式：inode 的属主与权限位**

`struct dinode` 增加 `mode`/`uid`/`gid` 三个 16 位字段。**只加 6 字节不是一个选项**：`mkfs` 有 `assert((BSIZE % sizeof(struct dinode)) == 0)`，而 `IPB = 16` 依赖 `sizeof(dinode) == 64`。所以这 6 字节从 `addrs[]` 里腾出来 —— `NDIRECT` 12 → 10，`sizeof` 仍是 **64**，磁盘布局一个字节都没动（`IPB`、`IBLOCK`、`nmeta` 的推导、`df` 的口径全部继续成立）。代价是单文件上限少 2 KiB（`MAXFILE` 268 → 266，即 266 KiB），由符号自动吸收，测试套件不受影响。

`FSMAGIC` 从 `0x10203040` 升到 `0x10203041`，**并保留旧值**：`fsinit()` 认出旧镜像时会说"这是更早的 inode 格式，请重跑 mkfs"，而不是一句无信息量的 `invalid file system`。**不提供自动迁移** —— 本项目唯一的持久文件系统是每次构建由 `mkfs` 重新生成的 `fs.img`，没有需要跨格式保留的数据；这是一个明确的取舍，不是遗漏。

`mkfs` 把初始文件的属主写 0，mode 由**主机文件名的前导 `_`** 判断（`_cat` → 0755，`README` → 0644），**不读宿主 `stat`** —— WSL 的 drvfs 挂载会把所有文件都报成 0777。

**权限判定只有一份**（`kernel/fs.c` 的 `perm_ok()`）

- 按 owner → group → other 选**唯一**一类，**不回退**：属主命中就只看属主位。若写成"属主不通过再看 group、group 不通过再看 other"，一个被属主设成 `0600` 的文件会被同组或其他人读到 —— 这是静默提权，不是便利。
- **uid 0 绕过读写位，但不绕过执行位**：没有任何 x 位的文件不是程序，`exec` 必须拒绝。这条规则同样用于目录搜索，所以一个 x 位全清的目录对所有人都不可进入（可从父目录 `chmod` 救回）。用同一条规则而不是两条，是刻意的。
- 判据是"**每一位都被授出**"，即 `((mode >> shift) & acc) == acc`。用 `& acc` 的非零值当允许是错的：`O_RDWR` 请求 R|W 时，一个 `0400` 的文件会因为"读那一位命中"而**放行写入**。

**检查点**：`namex()` 穿过每一级目录都要 x；`create()` **只在新条目**时才要父目录的 w（已存在的名字由文件自身的 mode 决定）；`sys_link()`/`sys_unlink()` 要父目录 w；非 root 建立硬链接还必须是属主或对源 inode 同时有读写权限，防止给别人的账户文件等敏感 inode 增加链接；`sys_chdir()` 要目标目录 x；`kexec()` 要执行位；`sys_open()` 按 `omode` 推出需要的位，**`O_TRUNC` 也算一次写**。**权限只在 `open()` 时判定一次**，已打开的描述符不复查 —— 这是 POSIX 行为，也是唯一能避免"检查与使用之间 mode 被改"的做法。

**进程与特权边界**：`kill`、`signal`、`setprio`、`setpgid` 仅允许 root 或同 UID 调用者控制目标，身份读取和操作都在目标进程锁内。非 root 加入已有组或设置终端前台组时，组必须存在且所有当前成员属于自己。`killpg` 对混合身份组只向有权限的成员投递，至少投递一次返回 0，否则返回 -1；终端中断使用保存的前台 UID 再检查成员，不能依赖中断发生时碰巧运行的进程身份。`mknod`、`klog` 和密码输入的 `ttyecho` 仅 root 可用。`psinfo` 保持公开，`ps` 增加 UID/GID 列。

**账户与认证**：`mkfs` 创建三个固定账户、私有主目录和 root 所有的 `/etc/passwd`（0600）。账户数据库是带版本标识的固定长度二进制格式，定义在 `common/auth.h`；与原计划的文本格式不同，不兼容宿主 Unix passwd 文件。口令使用 **PBKDF2-HMAC-SHA256，10,000 轮、16 字节独立盐、32 字节输出**；盐来自构建主机的 `/dev/urandom`，哈希实现用 Python `hashlib` 交叉校验。登录失败不透露用户名是否存在，等待 1/2/4/8 秒（后续固定 8 秒）；账户文件缺失、权限错误或内容无效均拒绝登录。输入上限为 63 字节，超长、空口令和两次输入不一致均拒绝。

`init` 每次启动 `login`，认证后先降 GID、再降 UID，关闭账户文件并清理缓冲区后 exec Shell；没有默认免密 root 通道。测试脚本显式输入教学 root 口令。`passwd NAME` 仅 root 可用，固定长度数据库以一次 write 日志事务原位更新、不先截断，因此崩溃恢复看到旧值或新值。目录锁 `/etc/pwlock` 串行化改密；异常退出遗留锁时由 root 检查后 `rm /etc/pwlock`。每账户盐在改密时保留，因为客户机没有安全随机源。

这是教学认证实现：预置口令公开、工作因子低于生产用途、没有安全随机设备、账户增删、自助改密、setuid 程序、登录会话隔离或密码恢复服务。它验证最小访问控制语义，不声称生产级安全。`whoami` 使用固定账户名映射。

**镜像预算**：`FSSIZE` 为 3200 块（3.125 MiB），元数据 47 块，数据区 3153 块，位图仍只需一块，日志/缓存容量未变。新增认证和权限测试程序后，本地镜像约使用 2049 块（含元数据），剩余约 1151 块；`writebig` 需要 266 个数据块及 1 个间接块。编译器不同会改变镜像使用量，实际以 mkfs/df 和完整回归为准。


### 功能检查修复 — 2026-10-05

对照 README 和路线图的功能复查发现四项缺陷：前台 Ctrl-Z 后 Shell 等待不返回；父 Shell 中的解析树未释放；语法错误结束整个登录会话；`>>` 覆盖文件开头而非追加。

本次在既有内核和 Shell 上修复：停止通知遵循 wait_lock / p->lock 锁序并防止锁交接期间丢失继续/终止信号；解析分配统一追踪、在失败或 fork 后释放，解析错误返回主循环；新增 O_APPEND，在 inode 锁保护下按写入事务选择 EOF。系统调用号、磁盘格式及预置程序数量保持不变。

新增 `test-shell.py` 并接入 Linux 单核/三核 CI；`sigtest` 增加父进程先等待再停止子进程，`mixstress` 增加独立 fd 并发追加、记录完整性及 dup/大写入检查。现有 `usertests` 和既有断言未放宽。完整验证结果、开发中发现及剩余限制见[功能检查修复日志](functional-fixes-2026-10-05.md)。

### 批次 14 — 宿主工具与符号化

目标是让 `fs.img` 和地址成为宿主可以直接检查、直接操作的对象，同时给后续批次（15 trace、16 check、18 prof）留下共用的底座。本批不改 `kernel/`、`user/`、`mkfs/`，不改磁盘格式，也不新增系统调用和用户程序；新增目录 `tools/`。

**交付**

| 文件 | 内容 |
| --- | --- |
| `tools/onyxfs.py` | `ls` / `cat` / `stat` / `fsck` / `put`，`--json` |
| `tools/symbolize.py` | `0x...`、`文件:地址`、stdin → `<prefix>addr2line -f -C -e <elf>` |
| `test-tools.py` | `unittest`：布局推导、坏 magic/旧 magic、截断、脏日志、位图泄漏、inode 越界、`size` 范围内缺失的直接块/间接块/间接项、`put` 复用含旧数据的空闲块、`put` 前后一致性与文件模式、头解析失败、符号化；`--qemu` 追加一次客户机回读 |

**布局常量不硬编码**

块大小、直接块数、目录项名长、`FSMAGIC`、`LOGBLOCKS`、`FSSIZE`、`NINODES` 都在每次运行时从 `kernel/fs.h`、`kernel/types.h`、`kernel/param.h`、`mkfs/mkfs.c` 解析；`NINDIRECT`、`MAXFILE`、`IPB`、`BPB` 按头文件里的同一公式重新计算。`struct dinode`、`struct dirent`、`struct superblock` 的字段偏移也由解析出的字段类型按 C 对齐规则推出，所以 `mode`/`uid`/`gid` 这类后来追加的字段不需要在 Python 里再写一遍。缺宏、未知类型、结构体大小与 mkfs 的假设不符时直接报错并指出文件名与名字，不退回默认值；`test-tools.py` 通过复制头文件并删掉 `#define BSIZE` / 改名 `struct dinode` 来验证这一点（退出码 2，不是 1）。

**`fsck` 的判据**

`fsck` 以"干净镜像"为标准，退出码 0 表示无错误：

1. superblock：magic（旧格式给出专门的提示）、`size`/`nblocks`/`nlog`/`logstart`/`inodestart`/`bmapstart` 与头文件推导值一致、文件长度足够、`ninodes` 与 `mkfs` 的 `NINODES` 一致；
2. 日志：头部的 `n` 必须在 `[0, nlog)`，非 0 表示有未提交事务，按错误报出（宿主不重放日志）；
3. 位图：元数据块必须占用、镜像末尾之后的位必须为 0、被 inode 引用的块必须在数据区内且已标记；
4. inode：类型合法、`nlink ≥ 1`、大小不超过 `MAXFILE * BSIZE`、设备节点大小为 0、目录大小是目录项大小的整数倍；
5. 地址：直接块与间接块都要落在数据区并被位图标记，同一块被两处引用报"已被谁使用"；`size` 范围内的直接块、间接块或间接表项缺失一律报错（文件系统没有稀疏文件语义）；
6. 目录：从根遍历，`. ` 与 `..` 的位置和指向、重名、空名、带 `/` 的名字、指向空闲 inode 的项、同一目录被两个父目录引用；
7. `nlink`：目录按 `1 + 子目录数`、普通文件按目录项引用数核对；分配了却不可达的 inode 单独报出；
8. 泄漏：位图标为占用但没有任何 inode 引用的数据块逐个报出。

所有信息都带 inode 号、块号或路径，测试据此断言"指出具体 inode/块"。

**`put` 的安全边界**

`put` 先跑一次 `fsck`，不干净直接拒绝；然后**在内存里**分配 inode 与数据块、写间接块、往根目录找空槽或扩展目录、置位图，再对重建后的 bytearray 完整跑一次 `fsck`，只有仍然干净才落盘。落盘用同目录临时文件 + `fsync` + `os.replace`，并保留原镜像的文件模式（`mkstemp` 默认 0600），失败时删掉临时文件、原镜像不动。因此"拒绝"和"失败"都不会留下半写状态，`test-tools.py` 用逐字节 hash 断言这一点。

限制是刻意的：只写根目录、只加普通文件、不覆盖同名文件、不支持目录/设备/硬链接；模式固定 `0644`、属主 `uid 0`；单文件不超过 `MAXFILE * BSIZE`，名字不超过 `DIRSIZ` 字节。运行中的 QEMU 持有旧 inode，所以替换后客户的写入落在旧镜像上，约定先退出 QEMU 再 `put`。

**`symbolize.py`**

前缀按 `--toolprefix`、`TOOLPREFIX`、`riscv64-elf-`、`riscv64-linux-gnu-`、`riscv64-unknown-elf-`、`riscv64-none-elf-`、`riscv64-unknown-linux-gnu-` 依次探测，同一脚本在 macOS、WSL2 与 Linux CI 上通用。`NAME:地址` 里的 `NAME` 先按路径，再按 `user/_NAME` 解析；内核默认 `kernel/kernel`。找不到 `addr2line` 时列出所有尝试过的名字并提示 `--toolprefix`，退出码 2；地址没有符号时打印 `??` 并以退出码 1 结束，不会静默返回空。

**验收结果**

本机（WSL2 / Ubuntu 26.04 / Python 3.12.16 / QEMU 10.2.1 / riscv64-linux-gnu-gcc 15.2.0）：

| 命令 | 结果 |
| --- | --- |
| `make fmt && git diff --exit-code` | 无差异（本批不涉及 C 源文件） |
| `make mkfs/mkfs kernel/kernel fs.img` | 通过，3200 块 / 47 元数据块 |
| `python3 test-harness.py` | 21 项通过 |
| `python3 test-auth.py` | 1 项通过 |
| `python3 test-tools.py` | 33 项通过，约 6.2 秒 |
| `python3 test-tools.py --qemu` | 34 项通过，约 7.3 秒（含一次客户机启动与 `cat` 回读） |
| `python3 tools/onyxfs.py fsck fs.img` | 退出码 0，`clean` |
| `python3 tools/symbolize.py --kernel 0x80000af2` | `panic at ./kernel/printk.c:231` |
| `python3 test-shell.py --cpus 1` | syntax / memory / append / jobs 全部 PASS |

未执行：三核 QEMU 回归与 Linux CI。本批不涉及内核、页表、信号与磁盘格式，按计划 §2.5 只要求宿主测试、构建和一次 QEMU 冒烟；合并前仍以 PR 的 Linux 单核/三核 CI 为准。

### 批次 15 — 系统调用飞行记录器

目标是让一次失败能回答“谁、在什么时候、调用了什么、参数和路径是什么、返回什么”，并在 panic 时保留现场。默认路径不变：没有进程开启 trace 时，每个系统调用只多一次标志分支，不写环、不复制路径。

**交付**

| 文件 | 内容 |
| --- | --- |
| `kernel/trace.h` | 共享的 `struct trace_entry`、`TRACE_*` 常量、系统调用名表 `TRACE_SYSCALLS(X)` |
| `kernel/trace.c` | 1,024 条记录的全局环、入口/返回记录、`tracectl` / `traceread`、panic 转储 |
| `kernel/syscall.c` | 在入口与返回处记录，路径类调用在入口有界复制路径 |
| `kernel/proc.[ch]` | 进程增加 `trace` 标志；`allocproc` 清零，`fork` 继承，`exec` 保留 |
| `kernel/syscall.h`、`defs.h`、`sysproc.c`、`user/usys.pl`、`user/user.h` | 登记 `SYS_tracectl = 49`、`SYS_traceread = 50` 及用户桩 |
| `user/trace.c` | `trace` 命令：`-f`、`-p PID`、`-e call,...`、`-o file` |
| `user/tracetest.c` | `DEDICATED` 自检：失败调用、ppid、按 pid 附着、关闭、权限与过滤、回绕、并发写者、并发 CLEAR、双目标隔离 |

**记录与环**

`struct trace_entry` 为 96 字节（`tick`、`ret`、`args[3]`、`pid`、`ppid`、`uid`、`syscall`、`mode`、`flags`、`seq`、`path[32]`），环共 96 KiB 静态内存。记录点只在 `syscall()`：入口写 ENTER、参数与路径，返回写 EXIT 与返回值；进程在本次调用中关闭 trace 时，只留入口、不写出口。

写者由 `trace_lock` 串行化：每个记录在持锁期间领取唯一索引、把槽位的 `seq` 置 0、填完记录后以 release 写入最终序号，再以 release 前移写索引，因此两个 hart 不会写到同一个槽位，较晚完成的旧写者也不能把索引改回去。普通读取逐槽加锁复制，释放锁后校验窗口期望序号，再按 UID 过滤并复制到用户态。panic 转储保持无锁，复制前后校验序号。已被覆盖的窗口槽位直接跳过。路径保留前 31 字节，定长字段的剩余字节清零。环回绕时新记录覆盖旧记录，`traceread` 通过 `lost` 返回自上次 `TRACE_CLEAR` 以来被覆盖的记录总数（单调增加），并在 `trace` 命令中按增量提示。

路径类调用（`open`、`mknod`、`unlink`、`link`、`mkdir`、`chdir`、`exec`、`chmod`、`chown`）在入口用最多 31 字节的 `fetchstr` 复制路径；复制失败就把路径清空，不影响系统调用本身。EXIT 记录复用栈上已复制的路径，不再第二次读取用户内存，因此 `exec` 成功后也能显示被执行的路径。

**权限与读取**

- `tracectl(op, pid)`：`op` 为 `TRACE_ON` / `TRACE_OFF` / `TRACE_CLEAR`；`pid = 0` 表示调用者自己。控制另一个进程的规则与 `signal()` 一致：root 或同 UID；`TRACE_CLEAR` 仅 root，并在 `trace_lock` 下清零索引与全部 `seq`，已进入 `trace_record()` 的写者会先完成，因此清空后不会再有旧索引的记录被发布。
- `traceread(buf, max, lost)`：返回最近的至多 `max` 条记录（最旧到最新），不带游标；调用者用记录中的 `seq` 去重和续读。非 root 调用者只会收到 `uid` 等于自己的记录，其他 UID 的记录在内核里过滤掉，不进入用户缓冲区。任何锁都不跨 `copyout`。
- 与计划文字的差异：计划写“非 root 读取其他 UID 的环返回 -1”。实现选择“过滤后不可见，只有自己的记录时返回 0”。理由是返回 -1 会让非 root 的 trace 结果取决于 root 是否恰好也在 trace，而真正的安全属性是不泄露；跨 UID 的 `tracectl` 仍然返回 -1，`tracetest` 同时验证这两点。
- 另一个差异是 `-f`：`fork` 继承 `trace` 标志是内核规则，子进程和孙进程总会被记录，所以 `-f` 只是把这一行为写清楚，不改变行为；`trace` 命令只对被跟踪目标开启标志，自身的轮询循环不被记录。

**panic 转储**

`panic()` 打印原因后，如果当前进程的 `trace` 为真，从环中取该 pid 的最后 32 条记录按时间顺序打印。路径只读环、不加锁、不分配内存；环回绕或遇到正在改写的槽位时在末尾打印 `trace: partial`。因为会停机，这项能力用临时构建（在 trace 的 `open` 入口插入一次 `panic("trace test")`）验证，验证后立即还原，不加入常规回归。实际输出：

```
panic: trace test
trace: last 1 records for pid 4
trace: 4 3 enter open("/no/such/file",0,1ac8)
```

**`trace` 命令**

输出固定为 `pid ppid name(args) = ret`；路径类调用的第一个参数显示为带引号的路径，其余参数以十六进制显示，返回值按十进制显示。`-e` 只过滤显示（记录仍在环里），`-o` 写入文件而不是控制台，`-p PID` 附着到已存在的进程并持续跟踪它及其后代。命令模式在子进程 `exec` 前开启标志，父进程每 tick 排空一次环，通过成功的 `fork` 返回值登记子 PID，即使子进程被重新收养也能继续追踪。所有已识别目标退出后，最后排空一次环，复查新发现的后代，并返回直接目标的退出码。

排空时命令只打印目标及其后代：目标 pid 在启动时加入集合，之后按记录顺序检查 `ppid`，父进程已知的记录把它的 `pid` 也加入集合。因此同一台机器上并行运行的多个 `trace` 不会互相混入输出，root 也不会把其他 UID 的追踪记录写进某个目标的结果；集合最多保存 1,024 个 pid。`-f` 只说明这一继承行为。

**验收结果**

本机（WSL2 Ubuntu 26.04 / Python 3.12.16 / QEMU 10.2.1 / riscv64-linux-gnu-gcc 15.2.0 / clang-format 21.1.8，QEMU 走 TCG）：

| 命令 | 结果 |
| --- | --- |
| `make fmt && git diff --exit-code` | 无差异 |
| `make kernel/kernel fs.img` | 通过，3200 块 / 47 元数据块 |
| `python3 test-harness.py` | 21 项通过 |
| `python3 test-auth.py` | 1 项通过 |
| `python3 test-xv6.py dedicated --cpus 3` | 13 项全部 PASS，`fslimit` 48.9 s，`tracetest` 22.6 s |
| `python3 test-xv6.py dedicated --cpus 1` | 13 项全部 PASS，`fslimit` 45.8 s，`tracetest` 20.9 s |
| `python3 test-xv6.py usertests --cpus 3` | `ALL TESTS PASSED`，197.9 s |
| `python3 test-xv6.py usertests --cpus 1` | `ALL TESTS PASSED`，181.6 s |
| `python3 test-xv6.py crash --cpus 3` | PASS，7.7 s |
| `python3 test-xv6.py crash --cpus 1` | PASS，7.3 s |
| `python3 test-stage3.py --cpus 1` / `--cpus 3` | 两项均 PASS |
| `python3 test-shell.py --cpus 1` / `--cpus 3` | syntax / memory / append / jobs 全部 PASS |
| 临时 panic 构建 + 手工启动 | 转储出 pid/ppid、`open` 路径与参数；还原后重新构建通过 |

手工 `/trace -e open /ls /` 能看到 `open("//etc",0x0,0xE) = 4` 这类带路径的记录；`/ls` 的逐字符写和目录读会产生上千条记录，环回绕时输出 `[trace: 724 older records lost]`，符合“诊断流不是可靠审计日志”的定位。

新增用例的有效性单独验证过：临时把 `trace_record()` 还原成无锁的 load/store 后，三核 `tracetest` 在“并发写者发布每条记录”处失败；临时去掉 `trace` 命令的后代过滤后，在“第一个输出不包含另一个目标”处失败。两处随后均还原。

未执行：Linux CI 与 macOS 构建（本机为 WSL2）；合并前仍以 PR 的 Linux 单核/三核 CI 为准。

**限制**

- 环是固定 96 KiB 静态内存。目标程序的写/读很密集时会很快覆盖旧记录；记录丢失以 `lost` 和 `[trace: ... lost]` 明示，不保证完整。
- `TRACE_CLEAR` 会让并发 `traceread` 的 `seq` 游标失效：清空后序号从 1 重新开始，正在运行的 `trace` 命令需要重启。写者本身不再与 CLEAR 竞态。
- `ppid` 在记录时未持有 `wait_lock`，并发被收养时可能显示新的父进程；这与 trace 的 best-effort 定位一致。
- `trace` 命令轮询周期为一个 tick，目标在两次轮询之间产生超过 1,024 条记录时会丢记录；`-o` 也只写实际读到的部分。
- `trace -p` 会等待目标及已识别的后代退出，不能中途停止；需要提前结束时由目标自身退出或从另一个终端杀掉目标。
- 不把 trace 当作安全审计或持久化日志：环会覆盖，panic 输出允许不完整，非 root 只能看到自己的记录。

### 批次 16 — 内核不变式检查

目标是把“测试没打印 FAIL”提升为“内核自己的账目和引用关系没有被破坏”。`check` 不修复任何东西，只做一次只读扫描；默认启动路径、系统调用编号顺序和磁盘格式都不变，新增的只有一个 root-only 系统调用和一个用户程序。

**交付**

| 文件 | 内容 |
| --- | --- |
| `kernel/check.c` | 进程表 / VMA / fd / inode 与目录 / 位图 / 日志头 / 页分配器的只读扫描，4 KiB 固定报告缓冲 |
| `kernel/sysproc.c`、`kernel/syscall.h`、`kernel/syscall.c`、`kernel/defs.h` | `SYS_kcheck`（51）与内核入口 `kcheck()` |
| `kernel/fs.c`、`kernel/kalloc.c`、`kernel/log.c`、`kernel/file.c` | 快照函数 `fscount_get()`、`kmem_snapshot()`、`log_check()`、`file_snapshot()` |
| `kernel/param.h`、`mkfs/mkfs.c` | `NINODES` 移到 `param.h`，mkfs 与内核共用同一处常量 |
| `user/check.c` | `check` 命令：打印报告；`--stats` 只打印 `stats:` 与 `violations=` 两行；有违规时退出码 1 |
| `user/user.h`、`user/usys.pl`、`Makefile`、`user/help.c` | 用户桩、`UPROGS` 与帮助索引登记 |
| `test-xv6.py` | `check` 加入 `DEDICATED`；新增 `--check`，在每个专用用例成功后于同一会话执行 `testrun check --stats` |
| `test-tools.py` | `--qemu` 下新增两个坏镜像用例：改 `README` 的 `nlink`、清掉超级块在位图中的位 |

**检查项与判据**

| 类别 | 判据 |
| --- | --- |
| 进程 | `state` 在 `UNUSED..ZOMBIE` 内；非 `UNUSED` 的 `pid > 0`；同一 pid 不重复；`parent` 为空或落在 `proc[]` 内 |
| VMA | 已用槽位 `start < end`、页对齐、落在 `[MMAPBASE, MMAPEND)`；`prot` 为 `READ` 或 `READ|WRITE`；`offset` 页对齐；匿名映射 `offset == 0`；文件映射的 `file->ref > 0`；两两不重叠 |
| fd | 槽位指针属于 `ftable`；`ref > 0`；`type` 在 `FD_PIPE..FD_DEVICE` 内 |
| inode | `type` 为 `T_DIR`/`T_FILE`/`T_DEVICE`；`size <= MAXFILE*BSIZE`；直接块、间接块与间接块内的地址都落在数据区 `[nmeta, size)`；目录必须有 `.`（指向自己）和 `..`；`..` 必须等于目录项所在的那个父目录 |
| nlink | 普通文件：`nlink` 等于所有命名的目录项数量；目录：`nlink` 等于 `1 + 子目录数`（与 mkfs/mkdir 的记账方式一致） |
| 文件系统 | 位图块数与 `fs_nused_blocks`、inode 数与 `fs_nused_inodes` 对照；`size`/`nlog`/`logstart`/`inodestart`/`bmapstart` 与 `FSSIZE`、`LOGBLOCKS`、`NINODES` 自洽；**元数据块必须标为已分配** |
| 分配器 | `kmem.nfree` 等于实际遍历到的空闲页数；`nfree + live + 内核映像页 == NPAGE` |
| 日志 | 内存中的日志头长度在 `0..LOGBLOCKS` 内，目的块号都在镜像范围内 |

`nlink == 0` 的孤儿按计划只列出、不计违规。目录的 `nlink` 规则与 `onyxfs.py fsck` 保持一致：目录自身只由 `.` 计一次，每个子目录的 `..` 再加一次，目录自己的 `..` 不会给父目录多加一次。

**并发与语义**

- `wait_lock` 只在进程表快照期间持有，`p->lock` 一次只持有一个，读完 `state/pid/parent/vmas/ofile` 就释放。
- 完全不持 inode 锁：目录内容用 `bread()` 按块读原始数据。这样即使某个 inode 的 `nlink` 在这期间降到 0，也不会经由 `iput()` 触发 `itrunc()`——检查器修改状态就等于销毁证据。
- 不持任何锁跨 `copyout`；报告先写进静态缓冲区，最后一次性拷出。
- 同一时刻只允许一个扫描：`busy` 用原子交换置位，第二个调用直接返回 -1，而不是等锁或覆盖第一份报告。
- 违规继续扫描、最多记录 16 条，其余只计数；孤儿最多列 8 条，两者都由 `stats:` 行给出总数。

**与计划的差异**

1. 系统调用与用户态桩命名为 `kcheck`，命令仍是 `check`。仓库里 `cowtest`、`idtest`、`mixstress`、`mmaptest`、`priotest`、`privtest` 各自定义了全局 `check()` 辅助函数，直接用同名会同时撞上 `usys.S` 的桩和编译期声明；重命名 245 处测试调用不值得，接口本身没有变化。
2. 计划写“按 inode 号顺序逐个加锁”，实现改成完全不加 inode 锁、只读原始块。理由同上：加锁读取孤儿 inode 会让 `iput()` 走截断路径。
3. 计划要求“位图与 `fs_nused_blocks`/`fs_nused_inodes` 一致”。这两组计数在启动时由 `fscount_scan()` 按位图和 inode 表重新校准，运行中天然一致，单靠对比查不出问题；因此额外加入“元数据块必须已分配”的布局判据，并把验收第 4 条的“位图计数不符”具体化为“清掉元数据位后必须报出”。块泄漏仍由宿主 `onyxfs.py fsck` 负责。
4. 页数守恒加入了内核映像页：`NPAGE` 覆盖全部 RAM，而 `kinit()` 只释放 `end` 之后的内存，映像自身占用的页既不在空闲链表也不在 `live` 里。

**验收结果**

本机（WSL2 / Ubuntu 26.04 / QEMU 10.2.1 / riscv64-linux-gnu-gcc 15.2.0 / Python 3.12.16）：

| 命令 | 结果 |
| --- | --- |
| `make fmt && git diff --exit-code`（提交后） | 无差异 |
| `make kernel/kernel fs.img` | 通过，3200 块 / 47 元数据块；镜像用到 2434 块，余 766 块 |
| `python3 test-harness.py` / `test-auth.py` | 21 项 / 1 项通过 |
| `python3 test-tools.py` | 30 项通过 |
| `python3 test-tools.py --qemu` | 33 项通过；坏 nlink 镜像报 `violation: inode 2: nlink 2 but 1 directory entries name it`，坏位图镜像报 `violation: metadata block 1 is marked free in the bitmap`，两者退出码均为 1 |
| `python3 test-xv6.py dedicated --cpus 1` | 14 项全部 PASS，`check` 1.492 s，`fslimit` 48.165 s，`tracetest` 21.775 s |
| `python3 test-xv6.py dedicated --cpus 3` | 14 项全部 PASS，`check` 1.599 s，`fslimit` 56.532 s，`tracetest` 22.763 s |
| `python3 test-xv6.py usertests --cpus 1` / `--cpus 3` | `ALL TESTS PASSED`，197.824 s / 199.933 s |
| `python3 test-xv6.py crash --cpus 1` / `--cpus 3` | PASS，7.362 s / 7.68 s |
| `python3 test-stage3.py --cpus 1` / `--cpus 3` | 均 PASS |
| `python3 test-shell.py --cpus 1` / `--cpus 3` | syntax / memory / append / jobs 全部 PASS |
| 同会话 `check --stats` → `fslimit 650` → `mixstress` → `cowtest` → `check --stats` | 前后完全一致：`fd=12 vma=0 inode_used=56 blocks_used=2434 inodes_used=56`，`violations=0` |
| 单次扫描耗时（宿主侧连续 6 次计时） | 每次约 0.08 s（含命令回显与轮询，轮询间隔 20 ms） |
| `fslimit --check`（同一会话末尾追加检查） | PASS |

新增用例的有效性单独验证过：改掉 `README` 的 `nlink` 后 `check` 精确报出该 inode；清掉超级块位图位后报出元数据块；两者都在 QEMU 中实测，随后恢复镜像。`violations` 计数与退出码由 `testrun` 换算，宿主不需要解析文本才能判断成败。

未执行：Linux CI 与 macOS 构建（本机为 WSL2）；合并前仍以 PR 的 Linux 单核/三核 CI 为准。

**限制**

- 报告是 best-effort 快照：扫描期间其他进程可以改变状态，单条违规可能是竞态而不是 bug。只在空闲/单用例环境下断言 `violations=0`。
- 同一时刻只允许一个扫描；并发调用返回 -1，不排队等待。
- inode/目录扫描是 O(inode 数 + 目录项数)，计数表按 `NINODES` 静态分配。若提高 `NINODES`，这份静态内存和扫描时间都会增长。
- 不审计页表叶子与 `page_state` 的逐页引用关系（计划中的 16.2），也不检查块泄漏；前者成本高且需要跨核处理，后者仍由 `onyxfs.py fsck` 负责。
- 读不了目录的镜像仍可能在读取阶段 panic（与计划第 5 条一致）；`check` 不承诺救回无法读取的镜像。
- 不修复任何东西：修复仍走正常文件操作或 `mkfs`。

### 批次 17 — DevLink：宿主与客户机的控制通道

目标不是“多一个命令”，而是同一个 QEMU 会话内改程序、传程序、跑程序，不重启、不重建镜像。默认启动路径保持不变：只有 `make dev` 会把第二个 virtio-mmio 槽接到宿主 socket，普通 `make qemu` 不加载任何 DevLink 设备。

**交付**

| 文件 | 内容 |
| --- | --- |
| `kernel/virtio_console.c` | 槽 1 的 virtio-serial 驱动：RX/TX 两个队列、128 KiB 接收 FIFO、同步发送、`AGENT` 字符设备 |
| `kernel/memlayout.h`、`kernel/vm.c`、`kernel/plic.c`、`kernel/trap.c` | `VIRTIO1 = 0x10002000`、IRQ 2、内核页表映射与中断分发 |
| `kernel/file.h` | `AGENT` 主设备号 2 |
| `user/agent.c` | 帧协议、`ping`/`exec`/`put`/`get`/`stat`/`klog`、分片与重同步 |
| `user/init.c` | 启动时创建 `/dev`、`/dev/agent`，`chmod 0600`，fork/exec `agent` |
| `Makefile` | `make dev`（`DEVSOCKET`，默认 `~/.onyx-dev/dev.sock`）、`make dev-reload`、`devlink-test`、`UPROGS`/`OBJS` |
| `tools/onyxctl.py` | 仅标准库的宿主客户端与 CLI，含 `watch` 热重载 |
| `test-devlink.py` | 端到端测试：自己启动 `make dev`，临时 socket，结束时杀掉整个进程组 |
| `.github/workflows/test.yml` | 单核/三核 Linux job 增加 `python3 test-devlink.py` |
| README、USER_GUIDE、help | 命令、开发接口边界与限制 |

**协议**

12 字节帧头，小端：

| 字段 | 长度 | 说明 |
| --- | --- | --- |
| magic | 4 | `"ONYX"`（0x58594e4f） |
| type | 1 | 1 ping、2 exec、3 put、4 get、5 stat、6 klog；响应置 0x80 |
| flags | 1 | bit 0 表示同一请求还有后续帧 |
| length | 2 | 本帧负载长度，单帧上限 4000 |
| id | 4 | 请求编号，响应原样带回 |

单次请求/响应上限 64 KiB，两个方向都支持分片。`exec` 的负载是 `argc` 加逐个长度前缀的参数；`put` 是路径、模式和文件内容；`get`/`stat` 只带路径；`klog` 带 seq 游标。`exec` 先按原样 `exec`，失败后再试 `/<name>`，与 Shell 的命令查找保持一致；引号、管道和变量展开由调用者自己拆分。

`exec` 的实现按计划的第一版：子进程把 stdout/stderr 重定向到 `/dev/ag-output/stdout 和 /dev/ag-output/stderr`，退出后回传并删除，单路输出上限 32 KiB。没有做流式输出。

**关键取舍与计划文字的差异**

1. **`virtserialport` 换成 `virtconsole`**。QEMU 把 virtio-serial 的 0 号端口保留给 `virtconsole`：不带 `nr` 的 `virtserialport` 会拿到 1 号端口，数据进入队列 4，而 `guest_connected` 在非 multiport 模式下只会在 0 号端口上置位——结果是宿主的数据被静默丢弃。`nr=0` 会被 QEMU 直接拒绝（"reserved for virtconsole devices"）。因此 `make dev` 用 `-device virtconsole,chardev=devlink,name=agent`，端口 0、队列 0/1，刚好是计划想要的“单端口、不启用 multiport”。
2. 不协商 `VIRTIO_CONSOLE_F_MULTIPORT`、`F_SIZE`、`F_EMERG_WRITE`，只接受 `VIRTIO_F_VERSION_1`（若设备提供）。没有控制队列。
3. **接收 FIFO 定为 128 KiB，而不是几 KiB**。virtio-serial 没有流控：QEMU 只要还有已投递的描述符就继续写，FIFO 满后只能丢字节，而丢字节会让帧失步。单次请求上限 64 KiB，128 KiB 留出一倍余量；这个大小是协议契约的一部分，不是可随手调小的参数。
4. **agent 在失步时重同步而不是退出**：宿主超时重试会重连并重发，客户机可能正停在旧帧的负载中间；按字节滑动直到帧头 magic 匹配，最多浪费一次请求，不会把整个调试通道打死。
5. **宿主侧的失败诊断**：每个请求都有超时和独立 id；只读请求可在重连后用新 id 重发，exec 和 put 只发送一次。超时或断链时错误信息包含请求 id、尝试次数、帧内已收字节数和响应体已收字节数，便于区分“agent 没起来”“帧被截断”和“传到一半断链”。`klog` 支持 `--cursor-file` 与 `--follow`，游标按返回的 seq 写回文件，跨调用增量拉取；`put -` 从 stdin 读，`get -` 写 stdout；`watch` 可监视目录或单个 `.c` 文件。
6. 没有新增系统调用：`exec`/文件操作复用既有调用，`klog` 复用 `klog()`，权限仍由 `/dev/agent` 的 0600 节点和既有文件权限检查决定。
6. `check` 这一类“检查器”不修改状态的原则同样适用：驱动只在初始化和收发路径写设备寄存器与自己的队列，不碰文件系统。

**验收结果**

本机（WSL2 / Ubuntu 26.04 / QEMU 10.2.1 / gcc 15.2.0）：

| 命令 | 结果 |
| --- | --- |
| `make dev` 启动 | 探针打印 `devlink: virtio-serial ready, rxq=8 txq=8 descriptors=8, features=0:30000006` |
| `make qemu` 普通启动 | 无 devlink 输出；`ps` 中没有 `agent`；`cat /dev/agent` 返回读错误，登录与既有流程不变 |
| `python3 test-devlink.py` | 6 项全部 PASS：ping protocol=1、exec `testrun cowtest` 退出码 0、9000 字节 put/get SHA-256 一致、`/dev/agent` 模式 0600、热重载 516 ms、失败命令保留 127 |
| `make dev-reload` / `onyxctl watch` 手工验证 | 保存 `user/echo.c` 后 532 ms 内打印新程序输出，QEMU 不重启、`fs.img` 不重建 |
| `make kernel/kernel fs.img` | 通过；镜像 2471/3200 块，余 682 块 |
| `test-harness.py` / `test-auth.py` / `test-tools.py` | 21 / 1 / 30 项通过 |
| `test-xv6.py dedicated --cpus 1` / `3` | 13 项全部 PASS，`fslimit` 50.79 s / 55.96 s，`tracetest` 23.19 s / 22.99 s |
| `test-xv6.py usertests --cpus 1` / `3` | `ALL TESTS PASSED`，206.19 s / 213.08 s |
| `test-xv6.py crash --cpus 1` / `3` | PASS，9.49 s / 8.66 s |
| `test-stage3.py`、`test-shell.py` 单核/三核 | 全部 PASS |

`test-devlink.py` 自带 `make dev`：临时 socket、独立进程组，结束时先 SIGTERM 再按需 SIGKILL，并断言 QEMU 进程确实退出；串口输出写入 `test-results/`，失败时打印最后 25 行。

未执行：Linux CI 与 macOS 构建（本机为 WSL2），合并前以 PR 的 Linux 单核/三核 CI 为准。

**限制**

- 这是本机开发接口：没有认证，不做网络监听；安全边界是“宿主本地 socket + root 客户机进程”。不要转发或暴露给不可信用户。
- `/dev/agent` 只有 root 可读写；agent 串行处理请求，一次只有一个宿主连接。同时打开第二个读端会失败。
- 单次请求/响应上限 64 KiB，`exec` 单路输出上限 32 KiB；超过上限的文件传输和输出会被拒绝或截断（`get` 拒绝，`exec` 截断）。
- 接收 FIFO 满时丢弃字节并计数；协议失步后 agent 会重同步，但这次请求会失败。
- 不使用 multiport，因此只有一个端口；`dev` 设备只在 `make dev` 下存在。
- `exec` 依赖客户机 `/dev/ag-output` 可写（agent 启动时创建）；磁盘满时 exec 会以 127 或 126 失败并返回 stderr。
- 热重载监视的是 `user/*.c` 的修改时间；`watch` 串行执行，保存后需要等当前这次运行结束才会处理下一次修改。

### 批次 20.2 — `sleep` 与不带 `/` 的命令查找

目标是让 Shell 换到别的目录后仍能直接执行命令，并提供一个不依赖 RTC 的等待手段。本批**不新增系统调用**、不改磁盘格式、不改 `exec` ABI。

**交付**

| 文件 | 内容 |
| --- | --- |
| `user/sleep.c` | 校验整数秒数并按 tick 等待。到期返回 0，参数错误返回 2，前台 Ctrl-C 触发内核默认退出码 130 |
| `user/sh.c` | `EXEC` 分支：不含 `/` 的名字先按原样 `exec`，失败后再试 `/<name>`；补 `#include "kernel/param.h"` 取 `MAXPATH` |
| `user/help.c` | `system` 组新增 `sleep` 条目 |
| `Makefile` | `UPROGS` 增加 `$U/_sleep` |
| `README.md`、`USER_GUIDE.md` | 更新程序计数、命令查找规则和 `sleep` 的用法 |

**设计取舍**

1. **复用既有的计时接口。** `pause(ticks)` 等待时钟 tick，遇到待处理信号可提前返回。作业由 `fg` 或 `bg` 恢复时，`sleep` 根据 `uptime()` 计算已过去的 tick，仅等待剩余时间。若在暂停期间已经到期，恢复后立即完成。
2. **每秒按 10 tick 换算。** QEMU 中一个 tick 约 100 ms。命令接受 1 至 86400 个整数秒。作为对[开发环境能力计划](batch14-dev-experience-plan.md) §11.3 的零秒特例，`sleep 0` 立即返回。
3. **不用 `atoi`。** `atoi("12abc")` 返回 12、`atoi("abc")` 返回 0，都会把输入错误变成一次错误时长的睡眠；解析器逐字符校验并检查溢出，失败返回 2。
4. **命令查找先原样、再 `/name`。** 含 `/` 的显式路径不回落，`./ls` 与 `/ls` 语义不变；不引入 `PATH`，避免与 20.3 的 `exec` 改动叠加。
5. **终止信号沿用内核默认行为。** 在前台按 Ctrl-C 发送 `SIGINT`，内核以状态码 130 终止进程。
6. **命令查找沿用现有的失败状态。** Shell 子进程在两次 `exec` 都失败后打印错误并执行 `exit(0)`。命令退出状态留给后续批次处理。

**初版验收结果**

以下是作业恢复修复前的记录。环境为 WSL2、QEMU 10.2.1、riscv64-linux-gnu-gcc 15.2.0、Python 3.14.4、clang-format 21.1.8。

| 命令 | 结果 |
| --- | --- |
| `make fmt && git diff --exit-code` | 通过（首次 `make fmt` 修正 `MAX_SECONDS` 的宏对齐一处） |
| `make kernel/kernel fs.img` | 通过，`nmeta 47`、`balloc: first 2565`、3200 块，镜像含 `user/_sleep` |
| `python3 test-harness.py` | 21 项通过 |
| `python3 test-auth.py` | 1 项通过 |
| `python3 test-tools.py` | 35 项通过（8.3 s） |
| `python3 test-tools.py --qemu` | 38 项通过（11.9 s，含一次客户机启动与 `put` 写回读） |
| `python3 test-stage3.py` | 2/2 通过（`test_login`、`test_invalid_images`），CPU=3 |
| `python3 test-shell.py` | 4/4 通过（syntax / memory / append / jobs），CPU=3 |
| `./test-xv6.py dedicated --cpus 3` | 14 项全部 PASS：`check` 1.4 s、`cowtest` 1.5 s、`kmemtest` 1.4 s、`waitxtest` 1.8 s、`cputest` 5.3 s、`priotest` 7.3 s、`idtest` 1.3 s、`permtest` 3.7 s、`privtest` 2.4 s、`mmaptest` 5.8 s、`mixstress` 8.2 s、`sigtest` 2.6 s、`fslimit` 64.6 s、`tracetest` 26.9 s |
| `./test-xv6.py usertests --cpus 3` | `ALL TESTS PASSED`，251.5 s |
| `./test-xv6.py crash --cpus 1` / `--cpus 3` | PASS，6.8 s / 7.2 s |
| 手工验证 | `sleep 0/1/5`、`sleep`（usage，退出码 2）、`sleep abc`/`-1`/`1.5`/`999999999`（拒绝）、`cd /etc` 后 `ls` 可用、`./ls` 失败、`/ls` 可用、`sleep 5` 中 Ctrl-C 终止 |
| `df` | 2568/3200 块已用、62/200 inode 已用；比加 `sleep` 前多 42 块、1 个 inode |

未执行：`CPUS=1` 的 `dedicated` 与 `usertests`（本地只跑三核，以 PR 的 Linux 单核 CI 为准）。

**评审修复验证**

macOS arm64 的 QEMU 单核、三核均通过全部 6 项 Shell 测试。新增的 `lookup` 用例验证当前目录优先、根目录回落和显式路径。`sleep` 用例核对参数退出码、1 秒对应 10 tick，以及等待中的作业收到 `SIGCONT` 后继续运行。将路径缓冲区移出递归栈后，RISC-V GCC 16.2 生成的 `runcmd` 栈帧由 176 字节恢复至 48 字节。

**限制**

- 只支持整数秒，不支持 `0.5` 这类小数，也没有 `sleep` 的 `--help`；
- 不引入 `PATH`：命令查找只多试一次 `/name`，仍是“根目录为主”的模型；
- 初版镜像中新增 `sleep` 后多用约 42 块，3200 块的镜像剩余 632 块；
- 命令查找优先当前目录的同名文件，这是“先按原样”的必然结果，已在 PR 说明中记录。
