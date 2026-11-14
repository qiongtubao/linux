# 1 号进程的诞生：rest_init → kernel_init → 用户态

承接 `latte_notes/mm-core-init.md`（`mm_core_init()` 让内存真正可用），
本文讲 `start_kernel()` 最后一步 `rest_init()` 怎么分出 0/1/2 号进程，
以及 1 号进程如何收尾、丢掉 `__init` 段、`execve` 出用户态 init。

## 一句话模型

`start_kernel()` 是 0 号进程（swapper / idle）在跑。跑到最后必须“分岔”：

| 进程 | 名字 | 干什么 |
|---|---|---|
| 0 号 | swapper / idle | `cpu_startup_entry()` 后永不返回，只在没事时补 CPU |
| 1 号 | `kernel_init` | 内核侧收尾（SMP、initcall）→ 丢掉 `__init` 段 → execve 用户 init |
| 2 号 | `kthreadd` | 所有内核线程的总管，`kthread_create()` 最终都落到它 |

分岔之后 1 号和 2 号并行跑，1 号先卡在 `kthreadd_done` 上等 2 号就位。

```
start_kernel()                          ← 0 号进程在跑
 └─ rest_init()
     ├─ user_mode_thread(kernel_init)  → pid 1 ┐
     ├─ kernel_thread(kthreadd)        → pid 2 │ 三者并行
     ├─ complete(&kthreadd_done) ──────────────┘ 放行 1 号
     └─ cpu_startup_entry()            0 号变 idle，不再返回

kernel_init（pid 1）
 ├─ kernel_init_freeable()
 │   ├─ smp_prepare_cpus / smp_init / sched_init_smp  ← 其它 CPU 上线
 │   ├─ page_alloc_init_late()        ← 见 mm 笔记（打印 Memory:）
 │   ├─ do_basic_setup() → do_initcalls()  ← 八段 initcall
 │   └─ wait_for_initramfs / console_on_rootfs
 ├─ async_synchronize_full()          ← 等异步初始化落地
 ├─ free_initmem() → free_reserved_area()  ← __init 段还给 buddy
 ├─ mark_readonly / pti_finalize
 └─ run_init_process("/init") → kernel_execve   ← 此后变成用户进程
```

## 启动时序表

| 阶段 | 函数 | 文件:行 | 一句话 |
|---|---|---|---|
| start_kernel | `rest_init` | `init/main.c:717` | 分岔点：造 1 号、2 号，自己变 idle |
| rest_init | `user_mode_thread(kernel_init)` | `init/main.c:731` | 建 1 号进程，抢下 pid 1 |
| rest_init | `kernel_thread(kthreadd)` | `init/main.c:747` | 建 2 号进程，内核线程总管 |
| rest_init | `complete(&kthreadd_done)` | `init/main.c:757` | 放行卡在等 2 号的 1 号进程 |
| rest_init | `cpu_startup_entry` | `init/main.c:774` | 0 号进程进 idle，永不返回 |
| pid 1 | `kernel_init` | `init/main.c:1530` | 1 号进程本体，从不返回 |
| pid 1 | `kernel_init_freeable` | `init/main.c:1640` | SMP、workqueue、initcall 全在这 |
| pid 1 | `do_basic_setup` | `init/main.c:1413` | 驱动模型 + 八段 initcall |
| pid 1 | `do_initcalls` | `init/main.c:1379` | 按段逐个调 initcall |
| pid 1 | `do_initcall_level` | `init/main.c:1357` | 跑单独一段：先过滤参数再调用 |
| pid 1 | `page_alloc_init_late` | `mm/mm_init.c:2334` | 补 deferred 页，打印 `Memory:` |
| pid 1 | `free_initmem` | `init/main.c:1520` | 弱符号，x86 用默认实现 |
| pid 1 | `free_initmem_default` | `include/linux/mm.h:3302` | 用 `__init_begin/__init_end` 划范围 |
| pid 1 | `free_reserved_area` | `mm/page_alloc.c:6244` | 逐页还给 buddy，打印 `Freeing …K` |
| pid 1 | `free_reserved_page` | `mm/page_alloc.c:6289` | 单页：清 Reserved、计数归零 |
| pid 1 | `run_init_process` | `init/main.c:1440` | `kernel_execve` 成用户态 init |

## 四个概念（小白版）

**0/1/2 号进程**：0 号是启动时就存在的“内核栈上的进程”，没有用户态，
最后变成 idle；1 号是 rootfs 的 init；2 号专门派生内核线程。
`ps` 里看不到 0 号，因为它在 idle 里不睡也不退。

**`user_mode_thread` 与 `kernel_thread`**：`kernel_thread` 建的线程只在内核态跑；
`user_mode_thread` 建的进程虽然一开始也在内核态（执行 `kernel_init`），
但它的入口最终会 `kernel_execve()` 跳到用户态，所以按用户进程记账。

**`system_state`**：`BOOTING → SCHEDULING → FREEING_INITMEM → RUNNING`。
`FREEING_INITMEM` 是个警告位：从这一刻起 `__init` 段的代码和数据随时会被收回，
任何还赖在那段里跑的代码都属于 bug。

**initcall 八段与 `__init` 段**：编译时把初始化函数按
`pure→core→postcore→arch→subsys→fs→device→late` 排序放进不同段，
`do_initcalls()` 顺序扫过去。这些函数都带 `__init`，跑完就该被回收，
回收动作正是 `free_initmem()`——所以 `free_initmem()` 必须排在 initcall 之后。

## 代码走读

**`rest_init()`**：`rcu_scheduler_starting()` 打开调度，然后先建 1 号（抢 pid 1）、
再建 2 号（内核线程总管），把 1 号钉在 boot CPU 上，`complete(&kthreadd_done)`
放行 1 号，最后自己 `schedule_preempt_disabled()` + `cpu_startup_entry()` 变 idle。
它是 `__ref` 且 `noinline`：绝不能被内联进 `__init` 的 `start_kernel()`。

**`kernel_init()`**：先 `wait_for_completion(&kthreadd_done)`，再 `kernel_init_freeable()`
把内核侧收尾，`async_synchronize_full()` 等异步活儿落地，置 `SYSTEM_FREEING_INITMEM`
后 `free_initmem()` 丢掉 `__init` 段，`mark_readonly()` + `pti_finalize()` 定型保护，
最后 `run_init_process()` 依次尝试 `/init`、`init=`、`/sbin/init`…，全失败就 panic。

**`kernel_init_freeable()`**：把 `gfp_allowed_mask` 放开（从此可睡眠分配）、
`set_mems_allowed()` 允许任意 node 取页、`smp_prepare_cpus/smp_init/sched_init_smp`
把其它 CPU 拉起来，然后 `page_alloc_init_late()`（内存彻底稳定）、
`do_basic_setup()`（initcall）、`wait_for_initramfs()`、`console_on_rootfs()`。

**`do_basic_setup()`**：`cpuset_init_smp()`、`driver_init()`（建 `/sys/bus` 等）、
`init_irq_proc()`、`do_ctors()`，最后 `do_initcalls()`——设备基本都在这儿出现。

**`do_initcalls()`**：先 `kzalloc` 一份可写命令行（`parse_args` 会改动字符串），
再对八段循环：每段前 `strcpy` 恢复命令行，调 `do_initcall_level()`，最后 `kfree`。

**`do_initcall_level()`**：先用 `parse_args()` 只挑本段的内核参数
（`core.xxx=` 之类），`do_trace_initcall_level()` 给 ftrace 打点，
再顺序遍历本段区间，逐个 `do_one_initcall()`。

**`free_initmem()` / `free_initmem_default()` / `free_reserved_area()`**：
弱符号 `free_initmem()` 调 `free_initmem_default(POISON_FREE_INITMEM)`，
后者用链接脚本的 `__init_begin/__init_end` 圈出整段并能按页毒化，
`free_reserved_area()` 逐页 `free_reserved_page()` 交还 buddy，并有
`pr_info("Freeing %s memory: %ldK")`。

**`free_reserved_page()`**：清分配打点 tag、`ClearPageReserved()`、
`init_page_count()` 置 1 后 `__free_page()` 走正常释放路径，
最后 `adjust_managed_page_count()` 把页数补回 zone 与 `totalram_pages`。

**`run_init_process()`**：`argv_init[0] = init_filename`，打印
`Run %s as init process`，然后 `kernel_execve()`。成功即不再返回，
失败返回负错误码让调用者试下一个候选。

## 三个关键问答

**为什么 `rest_init()` 必须是非 `__init` 函数？**
因为 `start_kernel()` 在 `__init` 段里，1 号进程可能很快跑到 `free_initmem()`
把那段回收；而 0 号进程此时还在 `start_kernel()` 的栈上继续执行，代码被收回就崩。
所以分岔逻辑放在 `__ref`（既不回收、又不算 `__init`）的 `rest_init()` 里。

**为什么先建 1 号再建 2 号，1 号却又要等 2 号？**
先建 1 号是为了让它拿到固定的 pid 1（这是给 rootfs 的 init 预留的坑）。
但 1 号后面要建内核线程，而所有内核线程都归 2 号管，所以 1 号必须先阻塞在
`kthreadd_done` 上，等 0 号把 2 号建好并 `complete()` 之后才继续。

**为什么 `free_initmem()` 要排在 `async_synchronize_full()` 之后？**
异步探测（async probe）会把 `__init` 函数指针存起来延后执行。
若不等异步任务全部落地就回收 `__init` 段，那些函数指针就指向被复用的内存。

## QEMU 实测

构建 `make -j16 bzImage` → `bzImage is ready (#10)`；日志 `/tmp/latte-init.log`。
过滤掉 245 行重复的 `idt_setup_from_table 1` 后，`[latte]` 打点原文：

```
414:[    1.082815] [latte] start_kernel -> rest_init
415:[    1.083092] [latte] rest_init -> kernel_init(pid 1)
416:[    1.095586] [latte] rest_init -> kthreadd(pid 2)
417:[    1.101073] [latte] kernel_init -> kernel_init_freeable
426:[    1.247051] [latte] kernel_init_freeable -> page_alloc_init_late
428:[    1.250196] [latte] kernel_init_freeable -> do_basic_setup
430:[    1.271306] [latte] do_basic_setup -> do_initcalls
587:[    3.299471] [latte] rest_init -> cpu_startup_entry(idle)
632:[    3.844206] [latte] kernel_init -> free_initmem
639:[    4.216620] [latte] kernel_init -> run_init_process
```

`free_initmem` 与 `run_init_process` 之间的三行（本节的主角）：

```
633:[    3.913431] Freeing unused kernel image (initmem) memory: 2856K
635:[    3.917014] Freeing unused kernel image (text/rodata gap) memory: 4K
636:[    3.918854] Freeing unused kernel image (rodata/data gap) memory: 864K
640:[    4.216826] Run /init as init process
```

两个值得留意的细节：

- `cpu_startup_entry(idle)` 落在 3.29s，排在 `do_initcalls`(1.27s) 之后。
  原因是 0 号进程 `complete(&kthreadd_done)` 后马上 `schedule_preempt_disabled()`
  让出 CPU，1 号进程一路跑到 3s 多（initcall 里有熵池初始化），0 号才被调度回来
  执行到 `cpu_startup_entry()` 那行打点。这正好证明“分岔是并行的”。
- `Freeing … (initmem) memory: 2856K` 与 `Memory:` 里的 `init` 项
  （`2856K init`）完全一致，说明 `free_initmem()` 回收的正是链接脚本
  `__init_begin..__init_end` 那 2856K；两条 gap 是段对齐留下的零头。

内存总量与上一阶段一致（`2015128K/2096632K`），说明本次只加注释与打点，
没有改变行为。


## 自查 5 问

1. `rest_init()` 为什么带 `noinline`？—— 老 gcc（gcc-3.4）会意外内联它，
   内联进 `__init` 段后分岔逻辑就会连同代码一起被回收。
2. `SYSTEM_FREEING_INITMEM` 之后还能用 `__init` 段吗？—— 不能，
   该状态就是“随时可能被回收”的警告位。
3. 1 号进程失败时试哪些候选？—— `ramdisk_execute_command`（默认 `/init`）→
   `init=` → `CONFIG_DEFAULT_INIT` → `/sbin/init`、`/etc/init`、`/bin/init`、`/bin/sh`。
4. initcall 一共几段、什么顺序？—— 8 段：
   `pure→core→postcore→arch→subsys→fs→device→late`。
5. `free_reserved_page()` 与 `__free_pages_core()` 区别？—— 前者回收零散
   保留页（refcount 置 1 后走 `__free_page`），后者是启动期整批页入 buddy。

## 下一阶段

用户态 init 跑起来后，`kernel_init` 已在 `execve` 里“变成”了 init 进程，
内核侧只剩 kthreadd 派生线程与中断/调度在跑。
下一步可深入 initramfs 的切换（`prepare_namespace`）或回到 mm 看内存回收（kswapd / shrinker）。
