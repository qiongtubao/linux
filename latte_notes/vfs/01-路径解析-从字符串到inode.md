# 主题一 · 路径解析：一个字符串怎么变成一个文件

## 你已知的

`cat /etc/passwd` 里那个 `/etc/passwd` 只是一串字符。内核要把它变成“能读的东西”，
中间要做**逐段解析**：先找 `/`，再在 `/` 里找 `etc`，再在 `etc` 里找 `passwd`。

## 三件套：dentry、inode、file

| 对象 | 代表什么 | 生命周期 |
|---|---|---|
| **dentry**（目录项） | 路径上的**一个名字**，例如 `/etc` 或 `/etc/passwd` | 被路径引用时存在；没人用时进 LRU，可被回收 |
| **inode** | 文件**本体**的元数据：大小、权限、时间、数据块位置 | 在文件系统里持久存在（内存里有副本） |
| **file** | 一次 `open()` 之后的**句柄**：偏移、标志、指向 dentry | 每次 open 一个，close 时释放 |

三者的关系：`dentry` 的 `d_inode` 指向 `inode`；`file` 的 `f_path.dentry` 指向 dentry。
**同一个 inode 可以有多个 dentry**（硬链接就是多个名字指向同一个 inode）。

## 目录是什么

在文件系统里，目录就是一张“名字 → inode 号”的表：

```
/           (inode 1, 目录)
├── etc     (inode 2, 目录)
│   └── passwd (inode 7, 普通文件)
└── bin     (inode 3, 目录)
```

所以 `cat /etc/passwd` 的解析过程是：

```
从根 dentry 开始
  ├─ 在 "/" 里查名字 "etc"  → 命中 inode 2（目录）
  └─ 在 inode 2 里查 "passwd" → 命中 inode 7（普通文件）
```

## 每一段要做的两件事

| 步骤 | 做什么 | 代价 |
|---|---|---|
| 查缓存 | 看 dentry 缓存里有没有“父目录 + 这个名字”的现成结果 | 纳秒级（内存哈希） |
| 问文件系统 | 缓存没有，调 `->lookup()` 读目录块（磁盘/内存） | 微秒到毫秒级 |

第二次打开同一个路径通常快得多，就是因为第一段查出结果后**被缓存下来了**（主题二）。

## 挂载点怎么处理

`/mnt` 看起来是个普通目录，但某个 dentry 上可能“盖着”另一个文件系统（挂载点）。
解析到那一段时，内核要把路径切到被挂载文件系统的根（`follow_managed()`），
否则 `/mnt/x` 会找到内存里那个空目录而不是挂上来的文件系统。

## 符号链接

`/bin/sh -> busybox` 这类链接，解析到 `sh` 时要把目标路径**插回**待解析队列继续走
（`pick_link()` / `step_into()`），并且有层数上限（40 层，防环）。

## 代码位置

| 位置 | 内容 |
|---|---|
| `fs/namei.c:2445` `link_path_walk()` | 主循环：逐段解析 |
| `fs/namei.c:2135` `walk_component()` | 处理一段 |
| `fs/namei.c:1743` `lookup_fast()` | 查 dentry 缓存 |
| `fs/namei.c:1831` `lookup_slow()` | 缓存未命中，问文件系统 |
| `include/linux/dcache.h` | `struct dentry` / `struct qstr` |
| `fs/inode.c` | `struct inode` 的分配与哈希 |
