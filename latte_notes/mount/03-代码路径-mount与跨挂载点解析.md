# 主题三 · 代码路径：`mount(2)` 与路径解析怎么跨挂载点

## 你已知的

`mount -t tmpfs none /mnt` 这条命令做两件事：**准备一个文件系统实例**，**把它接到
`/mnt` 上**。内核里对应两组函数，顺序不能反。

## `mount(2)` 的流程

```
sys_mount()
 └─ path_mount()                    fs/namespace.c:3953
     └─ do_new_mount()              fs/namespace.c:3675   ← 打点
         ├─ get_fs_type(fstype)     找到文件系统类型（tmpfs 的 file_system_type）
         ├─ fs_context_for_mount()  建 fs_context，解析 -o 选项
         ├─ vfs_get_tree(fc)        → 造 superblock（此时还没有根 dentry 之外的东西）
         ├─ do_new_mount_fc()       fs/namespace.c:3638
         │   └─ do_add_mount()
         │       └─ graft_tree() → attach_recursive_mnt()   fs/namespace.c:2551
         │            ← 真正接到父 mount 的树上：从这一刻起路径解析才会走到它
         └─ 失败就释放刚建的 sb，树不受影响
```

**为什么要分“建 sb”和“接树”两步**：建 sb 可能失败（选项错、设备坏），此时它还没进树，
失败就是干净的一次回滚。等接树成功，用户才看得到。

## 路径解析怎么跨挂载点

```
解析 /mnt/x
 ├─ 走到 dentry(/mnt)
 ├─ handle_mounts()                fs/namei.c:1626
 │   ├─ __lookup_mnt()             查这个 dentry 上有没有挂载（fs/namespace.c:795）
 │   └─ 有 → 换根：path->mnt 改成那个 mount，dentry 改成它的根
 └─ 继续解析 "x"（现在在新 sb 里）
```

这就是 `/mnt` 看起来“变成另一个目录”的机制——**不是替换 dentry，而是解析时换了一棵树**。

## `umount(2)` 的流程

```
sys_umount()
 └─ path_umount()                  fs/namespace.c:2025
     └─ do_umount()                fs/namespace.c:1860   ← 打点
         ├─ 还有用户（cwd、打开文件）→ -EBUSY
         ├─ MNT_DETACH → 先摘下，等最后一个用户离开再释放
         └─ umount_tree()          从树上摘下来
```

## 本次打点看到的完整序列

```
[latte] do_new_mount: devtmpfs -> devtmpfs     ← 内核自己挂 devtmpfs（1.30s）
[latte] do_new_mount: none -> proc             ← /init 里的 mount -t proc
[latte] do_new_mount: none -> sysfs
[latte] do_new_mount: none -> devtmpfs
[latte] do_new_mount: none -> tmpfs            ← 本次实验：挂到 /mnt
[latte] do_umount: flags=0x0                   ← 本次实验：umount
```

**启动过程本身就是靠 `mount` 把 `/proc`、`/sys`、`/dev` 拼进树的**，这也解释了为什么
`/dev` 里的设备节点、`/proc` 里的文件平时“像目录树的一部分”。

## 代码位置

| 位置 | 内容 |
|---|---|
| `fs/namespace.c:3953` `path_mount()` | `mount(2)` 入口 |
| `fs/namespace.c:3675` `do_new_mount()` | 主干（打点） |
| `fs/namespace.c:1218` `vfs_kern_mount()` | 建 sb（未接树） |
| `fs/namespace.c:3638` `do_new_mount_fc()` | 拿到 fc 后接树 |
| `fs/namespace.c:2551` `attach_recursive_mnt()` | 接树 |
| `fs/namespace.c:795` `__lookup_mnt()` | 查 dentry 上的挂载 |
| `fs/namei.c:1626` `handle_mounts()` | 路径解析换根 |
| `fs/namespace.c:1860` `do_umount()` | 摘树（打点） |
