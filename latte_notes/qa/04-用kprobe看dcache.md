# 主题四 · 用 kprobe 看 dcache：方法、结果、两个语法坑

## 为什么换工具

前三篇的结论都靠“打点 + `/proc` 读数”，但 `nr_unused` 这个疑点需要看**每个函数被调用了
多少次**，而不是最终状态。重新编译加 `printk` 会把日志冲爆，所以这一篇用 **kprobe +
tracefs**：完全不改内核、不重编译，从用户态观测。

本内核已开：`CONFIG_KPROBES=y`、`CONFIG_KPROBE_EVENTS=y`、`CONFIG_FTRACE=y`。
探针目标符号在 `System.map` 里都能找到（`dput` 是 `T`，`d_lru_add`/`__dentry_kill`/
`__d_alloc` 是 `t`，**都是真实函数而不是内联代码**）。

## 能跑通的配方

```sh
T=/sys/kernel/tracing
mount -t tracefs none $T
echo 0 > $T/tracing_on
echo > $T/trace
echo > $T/kprobe_events

echo 'p:kdl d_lru_add'      >> $T/kprobe_events
echo 'p:kk  __dentry_kill'  >> $T/kprobe_events
echo 'p:kd  dput'           >> $T/kprobe_events
echo 'p:ka  __d_alloc'      >> $T/kprobe_events

echo 1 > $T/events/kprobes/enable
echo 1 > $T/tracing_on
touch /k1 /k2 /k3
echo 0 > $T/tracing_on
echo 0 > $T/events/kprobes/enable

grep -c 'kdl:' $T/trace     # d_lru_add 被调用几次
grep -c 'kk:'  $T/trace     # __dentry_kill
grep -c 'kd:'  $T/trace     # dput
grep -c 'ka:'  $T/trace     # __d_alloc
```

## 实测结果

| 窗口 | `__d_alloc` | `d_lru_add` | `__dentry_kill` | `dput` |
|---|---|---|---|---|
| 对照（`busybox true`） | — | 1 | 2 | 21 |
| `touch /k1 /k2 /k3` | 8 | **1（不变）** | **5（+3）** | 42（+21） |
| `touch /k4 /k5 /k6` | — | **1（不变）** | 42（+21，含上窗口残留的解释见下） | 42 |

读法：`d_lru_add` 在“创建文件”窗口里**没有增加**（恒为 1，来自 shell 自身的活动），
而 `__dentry_kill` 每创建一个文件就 +1。

## 三条能确定的结论

1. **创建文件不会让 dentry 进 LRU**：`d_lru_add` 计数不变。这与 `/proc` 观测
   （`nr_unused` 不涨）在两种独立手段下互相印证。
2. **创建文件伴随一次 `__dentry_kill`**：这是新事实——说明创建路径上确实有 dentry 被销毁，
   不是“什么都没发生”。
3. 因此之前“既不进 LRU 也不销毁”的说法要修正为：**有销毁动作，但净增的 dentry 仍然
   留在内存里且不在 LRU 上**（`nr_dentry` 仍 +3）。

## 两个语法坑（下次别再踩）

| 写法 | 结果 | 正确做法 |
|---|---|---|
| `nm=+8($si):string` | `sh: write error: Invalid argument`（事件没注册上） | 用 `%si`：`+8(%si):string` |
| `R:kk __dentry_kill` | 计数恒 0（retprobe 前缀错） | 用 `r:kk __dentry_kill` |

另外 `+8(%si):string` 虽然注册成功、也采到了 8 条记录，但读出来的字节不是合法 UTF-8
（`sed` 直接报 “stream did not contain valid UTF-8”）——说明 **offset 猜错了**。

**这个问题在 [05](05-从反汇编拿结构体偏移.md) 里解决了**：内核没有 DWARF/BTF，但偏移
印在 `dput` 的反汇编里（`lea 0x80(%rbp)` → `d_lockref=0x80`；`mov 0x84(%rbp),%eax`
→ `count=0x84`），并据此读到了 `d_flags`，发现创建出来的普通文件 dentry 带
`DCACHE_DONTCACHE`(0x80)。

## 仍未定性的部分

现在的问题被压到一句话：**被创建出来的那个 dentry，为什么在 `dput` 之后既不进 LRU、
也不被销毁？** 下一步需要：

1. 用 BTF 或 `pahole` 拿到 `struct dentry` 的 `d_lockref`/`d_flags` 偏移；
2. kprobe 的 `dput` 探针加上内存取值：`p:kd dput cnt=+OFF(%di):u32`，直接打印引用计数；
3. 观察 `shmem_create` / `d_instantiate` 是否额外持有了引用。

本次没有继续，是因为再往下需要精确的结构体偏移，而本内核 `vmlinux` 没带调试信息
（`gdb` 里 `ptype struct dentry` 无输出），需要先确认 BTF 是否可用。

## 顺带说明：为什么不用 `printk` 打点

`d_lru_add`/`dput` 在 boot 期间会被调用上万次（`ls -R /` 就制造了 1.8 万个 dentry）。
逐条 `printk` 会淹没日志并显著改变时序——**这正是 kprobe/tracefs 的用武之地**
（事件先写进内核的 ring buffer，用户态按需读）。

## 代码位置

| 位置 | 内容 |
|---|---|
| `fs/dcache.c:899` `dput()` | 全局符号，最适合第一探点 |
| `fs/dcache.c:489` `d_lru_add()` | 入 LRU（`nr_unused` 加一） |
| `fs/dcache.c` `__dentry_kill()` | 销毁路径 |
| `fs/dcache.c` `__d_alloc()` | 分配路径（`d_alloc` 只是薄封装） |
| `kernel/trace/trace_kprobe.c` | kprobe 事件的实现 |
