# 主题三 · 代码路径：从 `open("/f1")` 到 inode

## 你已知的

`open(path, flags)` 只有两个参数，但内核要做三件事：**解析路径**、**处理最后一段**
（打开还是创建）、**把 inode 变成 file 句柄**。

## 三段式

```
openat(2)
 └─ do_filp_open()                      fs/namei.c:4169
     └─ path_openat()                   fs/namei.c:4130
         ├─ link_path_walk()            fs/namei.c:2445   ← 逐段解析（不含最后一段）
         │    └─ walk_component()       fs/namei.c:2135
         │        ├─ lookup_fast()      fs/namei.c:1743   ← 查 dentry 缓存
         │        └─ lookup_slow()      fs/namei.c:1831   ← 问文件系统
         └─ do_last() / lookup_open()   fs/namei.c:3714   ← 处理“最后一段”
             ├─ 已存在 → 直接用这个 dentry
             ├─ O_CREAT → 创建（打点 lookup_open: create）
             └─ 不存在的非 O_CREAT → -ENOENT
                 └─ vfs_open()          fs/open.c:1092    ← dentry+inode → file
```

## 为什么“最后一段”要单独处理

因为最后一段的行为取决于标志位：

| `open()` 标志 | 最后一段要做什么 |
|---|---|
| 无特殊标志 | 必须已存在，否则 `-ENOENT` |
| `O_CREAT` | 不存在就创建；`O_EXCL` 时存在则报 `-EEXIST` |
| `O_DIRECTORY` | 必须是目录 |
| `O_NOFOLLOW` | 最后一段是符号链接时不跟随 |

前面所有段（`/`、`etc`、`bin`…）都必须已经存在且是目录，所以它们走 `walk_component()`；
只有最后一段才需要“可能要创建”的逻辑。

## 两种创建入口，别搞混

| 系统调用 | 创建函数 | 说明 |
|---|---|---|
| `open(O_CREAT)` / `creat()` | `lookup_open()` 里的创建分支（打点所在） | 走路径解析 + 最后一段处理 |
| `mknod()` / `mkfifo()` | `vfs_create()` / `vfs_mknod()`（`do_mknodat()` 里） | 显式指定类型 |

本次实测用的 `touch` 走的是前者。最初把打点加在 `vfs_create()` 上，结果一次都没触发
——因为 tmpfs 的打开路径走的是 `lookup_open()` 里那条分支，`vfs_create()` 只在
`mknod` 系路径上。**打点位置错了，日志会安静得让人以为代码没跑。**

## 符号链接怎么展开

`walk_component()` 拿到一段发现是符号链接时，`pick_link()` 把目标路径插回待解析队列，
`step_into()` 进入新 dentry。层数上限 `MAXSYMLINKS = 40`（`include/linux/namei.h:13`），
超过就 `-ELOOP`。

本次 rootfs 里 `/bin/sh -> busybox` 就是这条路：解析 `/bin/sh` 时先命中 dentry，
发现是链接，再展开到 `/bin/busybox`。

## 打点与观察

| 打点 | 位置 | 本次触发 |
|---|---|---|
| `lookup_open: create %s` | `fs/namei.c:3770` | 每个 `O_CREAT` 打开一次（101 次） |
| `drop_caches: value=%d` | `fs/drop_caches.c:63` | 每次手工丢缓存一次（1 次） |

`lookup_fast()` / `lookup_slow()` 是每次路径解析都走的**最热路径**（一次 `ls` 几百次调用），
所以不在这里打点；它们的命中情况通过 `/proc/sys/fs/dentry-state` 间接观察（主题四）。

## 代码位置

| 位置 | 内容 |
|---|---|
| `fs/namei.c:4130` `path_openat()` | 打开操作的总流程 |
| `fs/namei.c:2445` `link_path_walk()` | 逐段解析 |
| `fs/namei.c:2135` `walk_component()` | 一段的解析 + 进入下一级 |
| `fs/namei.c:1743` / `1831` | 缓存命中 / 未命中 |
| `fs/namei.c:3714` `lookup_open()` | 最后一段：打开或创建（打点在 3770） |
| `fs/open.c:1092` `vfs_open()` | inode → file |
