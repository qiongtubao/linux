# 主题三 · 代码路径：link / unlink 与 inode 的生死

## 你已知的

`ln a b` 让两个名字指向同一份数据；`rm b` 只去掉一个名字。
内核里对应两个函数：`vfs_link()` 与 `vfs_unlink()`，它们都只改**计数**，不搬数据。

## 建硬链接（`ln a b`）

```
sys_linkat()
 └─ do_linkat() → vfs_link(old=/a, new=/b)      fs/namei.c:4913（打点）
     ├─ may_create()                            权限、目录可写
     ├─ 检查同一文件系统（s_max_links 上限）
     ├─ inode_lock(target); i_nlink++            ← 关键：计数加一
     ├─ d_instantiate(new_dentry, inode)         ← 新名字挂到 i_dentry alias 链
     └─ fsnotify_link()                          fanotify/inotify 通知
```

**只加了一个名字，没有复制任何数据。**

## 删名字（`rm b`）

```
sys_unlinkat()
 └─ do_unlinkat() → vfs_unlink(dir, dentry)      fs/namei.c:4669（打点）
     ├─ may_delete()                            权限、目录可写、不是挂载点
     ├─ inode_lock(target); i_nlink--            ← 关键
     ├─ 文件系统的 ->unlink()（tmpfs：shmem_unlink）
     ├─ d_delete(dentry)                          ← dentry 变 negative
     └─ dput(dentry)                              ← 没人用了就进 dcache LRU
```

inode 与数据**此时不动**：只要 `i_nlink > 0`（`/a` 还在），数据就必须留着。

## inode 什么时候真的消失

```
iput(inode)                        fs/inode.c:1930
 └─ i_count-- （到 0 才继续）
     └─ iput_final()                fs/inode.c:1874
         ├─ i_nlink > 0 → 挂到 sb->s_inodes 的 unused 链表（可回收，不释放）
         └─ i_nlink = 0 → evict()    fs/inode.c:786 → 释放 inode 与数据
```

两个条件的组合产生了四种状态：

| `i_nlink` | `i_count` | 含义 |
|---|---|---|
| >0 | >0 | 正常使用中 |
| >0 | 0 | 有名字但没人打开 → 可回收进缓存 |
| 0 | >0 | **已删除但还打开着**（数据留着，`lsof` 能查到 `deleted`） |
| 0 | 0 | 真正释放 |

## 本次打点看到的值

```
[latte] vfs_link: b (i_nlink=1)      ← 加之前是 1
[latte] vfs_unlink: b (i_nlink=2)    ← 减之前是 2
```

与 `ls -l` 的输出（`/a`、`/b` 的链接计数从 1 变 2）严丝合缝。

## 代码位置

| 位置 | 内容 |
|---|---|
| `fs/namei.c:4913` `vfs_link()` | 建硬链接（打点） |
| `fs/namei.c:4669` `vfs_unlink()` | 删名字（打点） |
| `fs/dcache.c:2465` `d_delete()` | dentry 转 negative |
| `fs/inode.c:1930` `iput()` | 引用计数减一 |
| `fs/inode.c:1874` `iput_final()` | 分流：evict 还是挂 unused |
| `fs/inode.c:786` `evict()` | 真正释放 |
| `mm/shmem.c` `shmem_link` / `shmem_unlink` | tmpfs 的具体实现 |
