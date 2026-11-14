# 主题二 · vfsmount 树与命名空间

## 你已知的

一台机器上不同用户看到的“根”可能不一样（容器里 `/` 是另一个文件系统）。
这靠的是**挂载命名空间**：每个命名空间有一棵独立的挂载树。

## 挂载树的结构

每个挂载点在内核里是一个 `struct mount`，它同时挂在两个地方：

```c
struct mount {
	struct vfsmount mnt;              /* 通用部分：根 dentry、super_block */
	struct mount *mnt_parent;         /* 父挂载 */
	struct dentry *mnt_mountpoint;    /* 我挂在父挂载的哪个 dentry 上 */
	struct list_head mnt_mounts;      /* 我的子挂载们 */
	struct list_head mnt_child;       /* 兄弟之间串起来 */
	struct mnt_namespace *mnt_ns;     /* 属于哪个命名空间 */
	...
};
```

于是“一棵树”在内存里就是一串 `mnt_parent` / `mnt_mountpoint` 的链子，而
**跨挂载点解析**就是沿着这张表换根。

## 命名空间

| 概念 | 作用 |
|---|---|
| `mnt_namespace` | 一组进程共享的挂载树；`fork` 默认共享，`clone(CLONE_NEWNS)` 复制一份 |
| 隔离效果 | 一个命名空间里 `mount`/`umount` 不影响别人 |
| 容器基础 | Docker 等就是用新的 mount + pid + net 命名空间做出“看起来独立的系统” |

## `/proc/self/mountinfo` 怎么读

每行是“一个挂载”，字段用空格分开（内核官方文档 `Documentation/filesystems/proc.rst` 有完整说明）：

```
36 35 98:0 /mnt1 /mnt2 rw,noatime master:1 - ext3 /dev/root rw,errors=continue
│  │  │    │     │      │                  │  │     │       │
│  │  │    │     │      │                  │  │     │       └ superblock 选项
│  │  │    │     │      │                  │  │     └──────── 源（设备/名字）
│  │  │    │     │      │                  │  └────────────── 文件系统类型
│  │  │    │     │      │                  └───────────────── 分隔符 "-"
│  │  │    │     │      └──────────────────────────────────── 挂载选项
│  │  │    │     └─────────────────────────────────────────── 挂载点在树里的路径
│  │  │    └───────────────────────────────────────────────── 在该文件系统内的根
│  │  └────────────────────────────────────────────────────── 主:次设备号
│  └───────────────────────────────────────────────────────── 父挂载的 ID
└──────────────────────────────────────────────────────────── 本挂载的 ID
```

重点看三列：**主:次设备号**（跨挂载点会变）、**挂载点**、**父 ID**（树的结构）。

## 与上一站的关系

| 现象 | 原因 |
|---|---|
| 硬链接不能跨 fs | inode 号只在同一 superblock 内唯一 |
| 符号链接可以跨 | 它存的是路径字符串，由解析器沿挂载树走 |
| `st_dev` 在挂载点两侧不同 | 两侧属于不同 superblock |

## 代码位置

| 位置 | 内容 |
|---|---|
| `include/linux/mount.h` | `struct vfsmount` |
| `fs/mount.h` | `struct mount`（内部结构） |
| `fs/namespace.c` | 挂载树的维护（`attach_recursive_mnt`、`umount_tree`） |
| `fs/pnode.c` | 挂载传播（shared/private peer group） |
| `fs/proc_namespace.c:135` `show_mountinfo()` | mountinfo 的输出 |
