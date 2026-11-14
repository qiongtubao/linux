# initramfs 解压：rootfs 里为什么会有 /init

承接 `latte_notes/init-process.md`——1 号进程最后要 execve 到 `/init`。本文按主题
组织：归档长什么样、为什么必须用状态机、解到哪里、谁在解、谁等它、内存怎么还、
哪些情况会失败。函数只在论证需要时出现，不做逐个走读。

## 全景

initramfs 就是一个 cpio 归档（cpio archive，常再压一层 gzip/xz/zstd），bootloader
用 `-initrd` 传进来（也可能编译时内建）；内核在 rootfs_initcall 段登记异步解包，
逐条目铺进 rootfs（根文件系统，即内存里的 ramfs）；之后 1 号进程才 execve `/init`。

```
do_initcalls()（rootfs_initcall 段）
 └─ populate_rootfs()               ← 只 async_schedule_domain()，不阻塞
      └─ do_populate_rootfs()       ← 异步执行体
           ├─ unpack_to_rootfs(__initramfs_start, size)   ← 内建归档（512 字节）
           ├─ unpack_to_rootfs(initrd_start, len)         ← 外部 initrd（本次 2M）
           │    └─ write_buffer() → actions[state]()      ← cpio FSM 逐条目
           │         ├─ do_header() 解析 110 字节头
           │         ├─ do_name()   建目录/文件/设备
           │         ├─ do_copy()   写正文
           │         └─ do_symlink() 建符号链接
           └─ free_initrd_mem() → free_reserved_area()     ← "Freeing initrd memory"
                （x86 走自己的 free_initrd_mem → free_init_pages）
kernel_init_freeable()
 └─ wait_for_initramfs()             ← async_synchronize_cookie_domain(等解包)
kernel_init()
 └─ run_init_process("/init")        ← 这时 /init 才存在
```

## 主题一 · 归档长什么样：cpio newc 的字节布局

- 一个条目 = 110 字节头 + 名字 + 对齐填充 + 正文 + 对齐填充，顺序固定。
- 110 = 6 字节 magic（`070701` 或 `070702`）+ 13 个 8 字节十六进制字段：ino、
  mode、uid、gid、nlink、mtime、body_len、major、minor、rdev 主/次、name_len、
  校验和。`070702` 比 `070701` 多的就是那个逐文件校验和。
- 全归档以 `TRAILER!!!` 收尾，后面还有 `\0` 填充（4 字节对齐用）。
- 所以解包器要做的只是“按固定偏移读头、按头里的长度跳到下一条”——没有索引表，
  没有递归，这是它几千行就能写完的根本原因。
- 代码位置：`parse_header()` `init/initramfs.c:197` 拆 13 个字段，magic 判断在
  `do_header()` `init/initramfs.c:300`。

## 主题二 · 数据是流式到达的：为什么必须有状态机

- 归档不是一次性给出来：压缩器输出、bootloader 搬运都是分块的，一条条目可能横跨
  两块缓冲区（头在上一块末尾，名字在下一块开头）。
- 于是不能“一次解析一条”，只能把“我现在在读什么”变成显式状态：Start →
  GotHeader →（Collect 攒 / SkipIt 跳）→ GotName →（CopyFile / GotSymlink）→
  Reset，共 8 个。
- 数据够就就地取（`eat()` 前移游标、扣减剩余量），不够就进 Collect 分多次拼。
- 引擎只有一句：`while (!actions[state]())`——返回 0 换状态继续，返回 1 表示这块
  缓冲吃完了。
- 启示：状态机是“流式输入 + 变长记录”的通用解法，跟 cpio 无关也能套用。
- 代码位置：状态枚举 `:227`、映射表 `actions[]` `:505`、引擎 `write_buffer()` `:517`。

## 主题三 · 解到哪里去：rootfs 是内存文件系统

- rootfs 不挂在任何块设备上，节点和正文全在内存（ramfs / 页缓存）里，所以“解压”
  其实是 mkdir / open / write / mknod 几个内存文件系统操作，没有写盘动作。
- 条目按类型落地：目录 mkdir、普通文件打开待写、设备与管道 mknod、符号链接
  symlink；同名但类型冲突的旧节点先删（`clean_path()`）；`nlink >= 2` 时按
  ino+dev 复用已有节点（`maybe_link()`）。
- 关键细节：用的不是用户态 `mkdir(2)`，而是 `fs/init.c` 里那批 `init_*()`。那个
  文件开头写得很直白：“mimic syscalls, but don't use the user address space or
  file descriptors”——路径是内核字符串、不走用户地址空间、也不占 fd，所以 init
  阶段可以直接调（内部实现仍是 `vfs_mkdir()` 这类正常 VFS 路径）。
- `init_*()` 期间攒下的 fput 不会立刻跑，最后由 `init_flush_fput()`
  （`flush_delayed_fput()` + `task_work_run()`）一次性放掉。
- 代码位置：`do_name()` `:392`、`do_copy()` `:454`、`do_symlink()` `:480`；
  `init_mkdir()` `fs/init.c:222`。

## 主题四 · 谁在什么时候解：initcall 段 + 异步域

- rootfs_initcall 段由 `do_initcalls()` 顺序调用；`populate_rootfs()` 被调到时只
  用 `async_schedule_domain()` 登记任务就返回，解包在异步 worker 里跑。
- 为什么异步：2M 归档解压要几十毫秒，同步做会把整段 initcall 拉长、还与其他
  initcall 串行；异步之后别的 initcall 可以先跑。
- 顺序固定：先内建归档（`__initramfs_start`，本次 512 字节 = `/dev`、
  `/dev/console`、`/root`），再外部 initrd（`initrd_start`，本次 2195968 字节）。
- 同一个函数里还 `usermodehelper_enable()`：打开“允许执行用户态程序”的开关，
  后面 `/init` 的 execve 依赖它。
- 代码位置：`populate_rootfs()` `:883`（`rootfs_initcall` 在 `:896`）、
  `do_populate_rootfs()` `:797`。

## 主题五 · 谁负责等：wait_for_initramfs 与它的三个消费者

- 异步意味着“解完了”这个事实得由消费者自己确认，于是有了同步点
  `wait_for_initramfs()`：`async_synchronize_cookie_domain(cookie + 1)`，等这次
  解包以及之后排队的任务全部跑完。
- 三个消费者：`kernel_init_freeable()`（`init/main.c:1692`，等它落地好 execve
  `/init`）、usermodehelper（`kernel/umh.c:108`，执行用户态程序前）、固件加载
  （`drivers/base/firmware_loader/main.c:513`，固件可能就在 rootfs 里）。
- cookie 还没登记就来等只 `pr_warn_once`：那是调用顺序 bug，死等会挂死机器。
- 命令行 `initramfs_async=0` 退化成同步（`populate_rootfs()` 里就地等），语义不变。
- 实测：本机日志 4 次（3 处调用者，umh 路径跑了两次）。

## 主题六 · 内存怎么还回去：保留区 vs __initramfs 段

- 外部 initrd 是 bootloader 放在 RAM 里、由 `reserve_initrd_mem()` 用
  `memblock_reserve()` 记过账的保留区（reserved region）：不显式还就一直白占。
- 归还链：`free_initrd_mem()` →（x86 自己的实现）`free_init_pages()` →
  `free_reserved_area()`，逐页还给 buddy，并打出 `Freeing initrd memory: 2148K`。
- 内建归档不用单独还：数据在 `__initramfs` 段里（属于 `__init` 段范围），后面
  `free_initmem()` 会统一回收——所以“内建归档省内存”只对一半。
- 例外：`retain_initrd`/`keepinitrd` 留着给 sysfs 导出；kexec 场景只还不与
  crashkernel 保留区重叠的那部分。
- 代码位置：`free_initrd_mem()` weak 版 `:725`、x86 版
  `arch/x86/mm/init.c:1014`、`free_reserved_area()` `mm/page_alloc.c:6244`。

## 主题七 · 失败与边界：什么该 panic，什么只报错

- 内建归档解不开 → `panic`：没有它系统起不来，降级没有意义。
- 外部 initrd 解不开 → 只 `printk`：可能本来就有别的 root 可用；
  `CONFIG_INITRAMFS_FORCE=y` 时干脆无视外部 initrd。
- 缺 `TRAILER!!!` → 压缩分支按 `state != Reset` 报 junk；未压缩分支由
  `flush_buffer()` 遇非 `'0'`/非 `\0` 报 junk；残留硬链接表由 `free_hash()` 兜底。
- 名字不以 `\0` 结尾 → 直接判坏包（`malformed archive`）。
- 启示：错误策略与“这步失败还能不能开机”一一对应，不是随手选的严重级别。

## 实测证据（QEMU）

`make -j16 bzImage` → `bzImage is ready (#12)`，`-initrd bb_initramfs.cpio` 为
2195968 字节。9 个打点全部命中，`grep -c` 计数：

| 打点 | 次数 | 说明 |
|---|---|---|
| `populate_rootfs: schedule async` | 1 | 登记点只跑一次 |
| `do_populate_rootfs: builtin cpio` | 1 | 内建归档解一次 |
| `do_populate_rootfs: external initrd` | 1 | 2195968 字节 |
| `unpack_to_rootfs: N bytes` | 2 | 512（内建）+ 2195968（外部） |
| `unpack_to_rootfs: done` | 2 | 两次都 `message=(none)` |
| `cpio node` | 12 | 内建 3 + 外部 9 |
| `cpio symlink` | 8 | 全部来自外部归档 |
| `free_initrd_mem -> free_reserved_area` | 1 | 由 x86 的实现打出 |
| `wait_for_initramfs -> async_synchronize` | 4 | 3 处调用者，umh 路径跑两次 |

原有两行原文（都落在解包窗口里）：

```
[    2.182758] Unpacking initramfs...
[    2.233585] Freeing initrd memory: 2148K
```

顺序核对（`grep -n` 行号严格递增）：`do_initcalls`(431) < `populate_rootfs`(548) <
`builtin cpio`(549) < `Unpacking initramfs...`(555) < `external initrd`(556) <
`2195968 bytes`(557) < 12 条 `cpio node` + 8 条 `cpio symlink`(558–574) <
`done`(575) < `free_initrd_mem`(576) < `Freeing initrd memory`(577) <
`wait_for_initramfs`(653–662) < `free_initmem`(665) < `Run /init`(673)。

字节与页数：2195968 ÷ 4K = 536.1 页，`2148K` = 537 页，差 3584 字节（不足 1 页）
——x86 的 `PAGE_ALIGN(end)` 把尾巴那半页一起还了。

两处与预期不同、已查清：① 内建归档并不空——`CONFIG_INITRAMFS_SOURCE=""` 只是
不用自定义目录，内核仍编进 `usr/default_cpio_list`（`/dev`、`/dev/console`、
`/root`），于是多 3 条 `cpio node` 和一次 512 字节解包；② `free_initrd_mem` 的
weak 版在 x86 上不被调用，打点因此同时加到了 `arch/x86/mm/init.c`。

## 自查 5 问

1. **newc 头多长？** 110 字节 = 6 字节 magic + 13 × 8 字节字段。
2. **FSM 有几个状态？** 8 个：Start / GotHeader / Collect / SkipIt / GotName /
   CopyFile / GotSymlink / Reset。
3. **`cpio node:` 为什么是 12 条？** 内建归档 3 条（`/dev`、`/dev/console`、
   `/root`）+ 外部归档 9 条（目录 7 + 普通文件 2）；外部那 8 个符号链接走
   `do_symlink()`，打在 `cpio symlink:` 上；`TRAILER!!!` 在打点之前就
   `return 0` 了。所以外部一轮合计 9 + 8 = 17，与 `cpio -it | wc -l` 相等。
4. **内建归档为什么只有 512 字节？** 它就是 `usr/default_cpio_list` 编出来的
   cpio，只有 `/dev`、`/dev/console`、`/root` 三条；`CONFIG_INITRAMFS_SOURCE=""`
   只是表示不加自定义目录，不代表归档为空。
5. **`Freeing initrd memory` 的页数怎么对得上？** 外部 initrd 2195968 字节 =
   536.1 页，`PAGE_ALIGN(end)` 尾巴多半页，回收 537 页 = 2148K。

## 下一阶段

本次 rootfs 自带 `/init`，走不到 `prepare_namespace()`；下一步看它，或回 mm 看回收。
