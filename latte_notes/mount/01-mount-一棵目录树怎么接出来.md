# 主题一 · mount：一棵目录树是怎么被“接”出来的

## 你已知的

Windows 里每个分区一个盘符（`C:`、`D:`），插入 U 盘会多一个 `E:`。Linux 不一样：
**只有一棵目录树**，U 盘要“挂”到某个目录（比如 `/media/usb`）上，才会出现在树里。

## 三个对象

| 对象 | 是什么 | 例子 |
|---|---|---|
| **superblock**（`super_block`） | 一个文件系统实例：块大小、根 inode、操作函数表 | tmpfs 的一份实例 |
| **vfsmount**（内核里是 `struct mount`） | 一次“挂载”：把某个 sb 的根接到某个 dentry 上 | `/mnt` 上那个 tmpfs |
| **dentry / inode** | 名字与文件本体（上一站） | `/mnt/x` |

`mount(2)` 做的事就是：**建一个 superblock（如果还没有），再创建一个 vfsmount 把它接到
挂载点的 dentry 上**。目录树因此是“拼”出来的：

```
/                 (rootfs 的根)
├── bin/ ...
├── mnt           ← 挂载点：这里不是 rootfs 的目录，而是另一个 sb 的根
│   └── x         (tmpfs 里的文件)
└── proc/ ...
```

## 跨挂载点时发生了什么

路径解析走到 `/mnt` 这个 dentry 时，内核发现它是“被挂载覆盖”的点，于是把当前路径
切到那个 vfsmount 的根（`handle_mounts()`）。这就是为什么：

| 观察 | 挂载前 | 挂载后 |
|---|---|---|
| `stat -c %d /mnt` | rootfs 的设备号 | **tmpfs 的设备号（不同）** |
| `stat -c %i /mnt` | `/mnt` 目录自己的 inode 号 | tmpfs 根目录的 inode 号 |
| `/mnt` 里能看到的东西 | rootfs 里那个空目录 | tmpfs 的内容（原来那个目录被“盖住”） |

**`st_dev` 是判断“有没有跨挂载点”的最直接信号**——同一个 superblock 下的所有文件
共享一个设备号。

## 为什么硬链接不能跨文件系统

inode 号只在**同一个 superblock 内**唯一。`/a` 的 inode 号 5 和 `/mnt/a` 的 inode 号 5
是两份完全不同的东西，所以硬链接只能连同一文件系统里的名字（`ln` 跨挂载点会报
`EXDEV`）。符号链接没有这个限制，因为它存的是**字符串路径**，由解析器按当前挂载树
去走。

## 三个容易混的动作

| 动作 | 做了什么 | 谁能看见 |
|---|---|---|
| `vfs_kern_mount()` | 只建 sb 和根 dentry | 谁都看不见（还没接树） |
| `attach_recursive_mnt()` | 把 vfsmount 接到父 mount 的树上 | 本命名空间内可见 |
| `do_umount()` | 从树上摘下来 | 之后路径解析不再走到它 |

## 代码位置

| 位置 | 内容 |
|---|---|
| `fs/namespace.c:3675` `do_new_mount()` | `mount(2)` 主干（打点） |
| `fs/namespace.c:1218` `vfs_kern_mount()` | 建 superblock（未接树） |
| `fs/namespace.c:2551` `attach_recursive_mnt()` | 接树 |
| `fs/namei.c:1626` `handle_mounts()` | 路径解析跨挂载点 |
| `fs/proc_namespace.c:135` `show_mountinfo()` | `/proc/self/mountinfo` 的来源 |
