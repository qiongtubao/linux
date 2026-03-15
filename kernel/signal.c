// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/kernel/signal.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  1997-11-02  Modified for POSIX.1b signals by Richard Henderson
 *
 *  2003-06-02  Jim Houston - Concurrent Computer Corp.
 *		Changes to use preallocated sigqueue structures
 *		to allow signals to be sent reliably.
 */

/*
 * Linux内核信号处理子系统
 *
 * 本文件实现了Linux信号机制的核心功能：
 * - 信号的发送（kill/tkill/tgkill系统调用）
 * - 信号队列管理（实时信号与普通信号）
 * - 信号的传递与处理（do_signal）
 * - 信号阻塞与挂起（sigprocmask/sigsuspend）
 * - 进程组/会话信号广播
 * - ptrace相关信号处理
 */

#include <linux/slab.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/sched/mm.h>
#include <linux/sched/user.h>
#include <linux/sched/debug.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/sched/cputime.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/tty.h>
#include <linux/binfmts.h>
#include <linux/coredump.h>
#include <linux/security.h>
#include <linux/syscalls.h>
#include <linux/ptrace.h>
#include <linux/signal.h>
#include <linux/signalfd.h>
#include <linux/ratelimit.h>
#include <linux/tracehook.h>
#include <linux/capability.h>
#include <linux/freezer.h>
#include <linux/pid_namespace.h>
#include <linux/nsproxy.h>
#include <linux/user_namespace.h>
#include <linux/uprobes.h>
#include <linux/compat.h>
#include <linux/cn_proc.h>
#include <linux/compiler.h>
#include <linux/posix-timers.h>
#include <linux/livepatch.h>
#include <linux/cgroup.h>
#include <linux/audit.h>

#define CREATE_TRACE_POINTS
#include <trace/events/signal.h>

#include <asm/param.h>
#include <linux/uaccess.h>
#include <asm/unistd.h>
#include <asm/siginfo.h>
#include <asm/cacheflush.h>

/*
 * SLAB caches for signal bits.
 */

static struct kmem_cache *sigqueue_cachep;

int print_fatal_signals __read_mostly;

/**
 * sig_handler - 获取任务的信号处理函数
 * @t: 目标任务结构
 * @sig: 信号编号
 * 返回值：信号处理函数指针
 */
static void __user *sig_handler(struct task_struct *t, int sig)
{
	return t->sighand->action[sig - 1].sa.sa_handler;
}

/**
 * sig_handler_ignored - 检查信号处理函数是否被忽略
 * @handler: 信号处理函数指针
 * @sig: 信号编号
 * 返回值：如果信号被忽略返回true，否则返回false
 */
static inline bool sig_handler_ignored(void __user *handler, int sig)
{
	/* Is it explicitly or implicitly ignored? */
	return handler == SIG_IGN ||
	       (handler == SIG_DFL && sig_kernel_ignore(sig));
}

/**
 * sig_task_ignored - 检查任务是否应该忽略特定信号
 * @t: 目标任务结构
 * @sig: 信号编号
 * @force: 是否强制发送信号
 * 返回值：如果任务应该忽略信号返回true，否则返回false
 */
static bool sig_task_ignored(struct task_struct *t, int sig, bool force)
{
	void __user *handler;

	handler = sig_handler(t, sig);

	/* SIGKILL and SIGSTOP may not be sent to the global init */
	if (unlikely(is_global_init(t) && sig_kernel_only(sig)))
		return true;

	if (unlikely(t->signal->flags & SIGNAL_UNKILLABLE) &&
	    handler == SIG_DFL && !(force && sig_kernel_only(sig)))
		return true;

	/* Only allow kernel generated signals to this kthread */
	if (unlikely((t->flags & PF_KTHREAD) &&
		     (handler == SIG_KTHREAD_KERNEL) && !force))
		return true;

	return sig_handler_ignored(handler, sig);
}

/**
 * sig_ignored - 检查信号是否被忽略（考虑阻塞和ptrace）
 * @t: 目标任务结构
 * @sig: 信号编号
 * @force: 是否强制发送信号
 * 返回值：如果信号被忽略返回true，否则返回false
 */
static bool sig_ignored(struct task_struct *t, int sig, bool force)
{
	/*
	 * Blocked signals are never ignored, since the
	 * signal handler may change by the time it is
	 * unblocked.
	 */
	if (sigismember(&t->blocked, sig) || sigismember(&t->real_blocked, sig))
		return false;

	/*
	 * Tracers may want to know about even ignored signal unless it
	 * is SIGKILL which can't be reported anyway but can be ignored
	 * by SIGNAL_UNKILLABLE task.
	 */
	if (t->ptrace && sig != SIGKILL)
		return false;

	return sig_task_ignored(t, sig, force);
}

/**
 * has_pending_signals - 检查是否有待处理的信号
 * @signal: 待处理信号集合
 * @blocked: 阻塞信号集合
 * 返回值：如果有未阻塞的待处理信号返回true，否则返回false
 *
 * 重新计算待处理状态，从本地待处理信号集合、全局待处理信号和阻塞信号中计算。
 */
static inline bool has_pending_signals(sigset_t *signal, sigset_t *blocked)
{
	unsigned long ready;
	long i;

	switch (_NSIG_WORDS) {
	default:
		for (i = _NSIG_WORDS, ready = 0; --i >= 0 ;)
			ready |= signal->sig[i] &~ blocked->sig[i];
		break;

	case 4: ready  = signal->sig[3] &~ blocked->sig[3];
		ready |= signal->sig[2] &~ blocked->sig[2];
		ready |= signal->sig[1] &~ blocked->sig[1];
		ready |= signal->sig[0] &~ blocked->sig[0];
		break;

	case 2: ready  = signal->sig[1] &~ blocked->sig[1];
		ready |= signal->sig[0] &~ blocked->sig[0];
		break;

	case 1: ready  = signal->sig[0] &~ blocked->sig[0];
	}
	return ready !=	0;
}

#define PENDING(p,b) has_pending_signals(&(p)->signal, (b))

/**
 * recalc_sigpending_tsk - 重新计算任务的待处理信号状态
 * @t: 目标任务结构
 * 返回值：如果有待处理信号返回true，否则返回false
 */
static bool recalc_sigpending_tsk(struct task_struct *t)
{
	if ((t->jobctl & (JOBCTL_PENDING_MASK | JOBCTL_TRAP_FREEZE)) ||
	    PENDING(&t->pending, &t->blocked) ||
	    PENDING(&t->signal->shared_pending, &t->blocked) ||
	    cgroup_task_frozen(t)) {
		set_tsk_thread_flag(t, TIF_SIGPENDING);
		return true;
	}

	/*
	 * We must never clear the flag in another thread, or in current
	 * when it's possible the current syscall is returning -ERESTART*.
	 * So we don't clear it here, and only callers who know they should do.
	 */
	return false;
}

/*
 * After recalculating TIF_SIGPENDING, we need to make sure the task wakes up.
 * This is superfluous when called on current, the wakeup is a harmless no-op.
 */
/**
 * recalc_sigpending_and_wake - 重新计算信号挂起状态并唤醒进程
 * @t: 目标进程
 *
 * 重新计算进程的信号挂起标志，如果有挂起的信号则唤醒进程
 */
void recalc_sigpending_and_wake(struct task_struct *t)
{
	if (recalc_sigpending_tsk(t))
		signal_wake_up(t, 0);
}

/**
 * recalc_sigpending - 重新计算当前进程的信号挂起状态
 *
 * 检查当前进程是否有挂起的信号需要处理
 */
void recalc_sigpending(void)
{
	if (!recalc_sigpending_tsk(current) && !freezing(current) &&
	    !klp_patch_pending(current))
		clear_thread_flag(TIF_SIGPENDING);

}
EXPORT_SYMBOL(recalc_sigpending);

/**
 * calculate_sigpending - 计算进程的信号挂起状态
 *
 * 检查当前进程的信号队列，设置TIF_SIGPENDING标志
 */
void calculate_sigpending(void)
{
	/* Have any signals or users of TIF_SIGPENDING been delayed
	 * until after fork?
	 */
	spin_lock_irq(&current->sighand->siglock);
	set_tsk_thread_flag(current, TIF_SIGPENDING);
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);
}

/* Given the mask, find the first available signal that should be serviced. */

#define SYNCHRONOUS_MASK \
	(sigmask(SIGSEGV) | sigmask(SIGBUS) | sigmask(SIGILL) | \
	 sigmask(SIGTRAP) | sigmask(SIGFPE) | sigmask(SIGSYS))

/**
 * next_signal - 获取下一个需要处理的信号
 * @pending: 信号挂起队列
 * @mask: 信号掩码
 *
 * 从信号队列中找到下一个未被阻塞的信号
 * 返回值: 信号编号，如果没有信号则返回0
 */
int next_signal(struct sigpending *pending, sigset_t *mask)
{
	unsigned long i, *s, *m, x;
	int sig = 0;

	s = pending->signal.sig;
	m = mask->sig;

	/*
	 * Handle the first word specially: it contains the
	 * synchronous signals that need to be dequeued first.
	 */
	x = *s &~ *m;
	if (x) {
		if (x & SYNCHRONOUS_MASK)
			x &= SYNCHRONOUS_MASK;
		sig = ffz(~x) + 1;
		return sig;
	}

	switch (_NSIG_WORDS) {
	default:
		for (i = 1; i < _NSIG_WORDS; ++i) {
			x = *++s &~ *++m;
			if (!x)
				continue;
			sig = ffz(~x) + i*_NSIG_BPW + 1;
			break;
		}
		break;

	case 2:
		x = s[1] &~ m[1];
		if (!x)
			break;
		sig = ffz(~x) + _NSIG_BPW + 1;
		break;

	case 1:
		/* Nothing to do */
		break;
	}

	return sig;
}

static inline void print_dropped_signal(int sig)
{
	static DEFINE_RATELIMIT_STATE(ratelimit_state, 5 * HZ, 10);

	if (!print_fatal_signals)
		return;

	if (!__ratelimit(&ratelimit_state))
		return;

	pr_info("%s/%d: reached RLIMIT_SIGPENDING, dropped signal %d\n",
				current->comm, current->pid, sig);
}

/**
 * task_set_jobctl_pending - set jobctl pending bits
 * @task: target task
 * @mask: pending bits to set
 *
 * Clear @mask from @task->jobctl.  @mask must be subset of
 * %JOBCTL_PENDING_MASK | %JOBCTL_STOP_CONSUME | %JOBCTL_STOP_SIGMASK |
 * %JOBCTL_TRAPPING.  If stop signo is being set, the existing signo is
 * cleared.  If @task is already being killed or exiting, this function
 * becomes noop.
 *
 * CONTEXT:
 * Must be called with @task->sighand->siglock held.
 *
 * RETURNS:
 * %true if @mask is set, %false if made noop because @task was dying.
 */
/**
 * task_set_jobctl_pending - 设置作业控制挂起标志
 * @task: 目标进程
 * @mask: 要设置的作业控制标志掩码
 *
 * 设置进程的作业控制相关挂起标志(如SIGSTOP、SIGTSTP等)
 * 返回值: 成功返回true，失败返回false
 */
bool task_set_jobctl_pending(struct task_struct *task, unsigned long mask)
{
	BUG_ON(mask & ~(JOBCTL_PENDING_MASK | JOBCTL_STOP_CONSUME |
			JOBCTL_STOP_SIGMASK | JOBCTL_TRAPPING));
	BUG_ON((mask & JOBCTL_TRAPPING) && !(mask & JOBCTL_PENDING_MASK));

	if (unlikely(fatal_signal_pending(task) || (task->flags & PF_EXITING)))
		return false;

	if (mask & JOBCTL_STOP_SIGMASK)
		task->jobctl &= ~JOBCTL_STOP_SIGMASK;

	task->jobctl |= mask;
	return true;
}

/**
 * task_clear_jobctl_trapping - clear jobctl trapping bit
 * @task: target task
 *
 * If JOBCTL_TRAPPING is set, a ptracer is waiting for us to enter TRACED.
 * Clear it and wake up the ptracer.  Note that we don't need any further
 * locking.  @task->siglock guarantees that @task->parent points to the
 * ptracer.
 *
 * CONTEXT:
 * Must be called with @task->sighand->siglock held.
 */
/**
 * task_clear_jobctl_trapping - 清除作业控制陷阱标志
 * @task: 目标进程
 *
 * 清除进程的JOBCTL_TRAPPING标志，表示陷阱处理完成
 */
void task_clear_jobctl_trapping(struct task_struct *task)
{
	if (unlikely(task->jobctl & JOBCTL_TRAPPING)) {
		task->jobctl &= ~JOBCTL_TRAPPING;
		smp_mb();	/* advised by wake_up_bit() */
		wake_up_bit(&task->jobctl, JOBCTL_TRAPPING_BIT);
	}
}

/**
 * task_clear_jobctl_pending - clear jobctl pending bits
 * @task: target task
 * @mask: pending bits to clear
 *
 * Clear @mask from @task->jobctl.  @mask must be subset of
 * %JOBCTL_PENDING_MASK.  If %JOBCTL_STOP_PENDING is being cleared, other
 * STOP bits are cleared together.
 *
 * If clearing of @mask leaves no stop or trap pending, this function calls
 * task_clear_jobctl_trapping().
 *
 * CONTEXT:
 * Must be called with @task->sighand->siglock held.
 */
/**
 * task_clear_jobctl_pending - 清除作业控制挂起标志
 * @task: 目标进程
 * @mask: 要清除的作业控制标志掩码
 *
 * 清除进程指定的作业控制挂起标志
 */
void task_clear_jobctl_pending(struct task_struct *task, unsigned long mask)
{
	BUG_ON(mask & ~JOBCTL_PENDING_MASK);

	if (mask & JOBCTL_STOP_PENDING)
		mask |= JOBCTL_STOP_CONSUME | JOBCTL_STOP_DEQUEUED;

	task->jobctl &= ~mask;

	if (!(task->jobctl & JOBCTL_PENDING_MASK))
		task_clear_jobctl_trapping(task);
}

/**
 * task_participate_group_stop - participate in a group stop
 * @task: task participating in a group stop
 *
 * @task has %JOBCTL_STOP_PENDING set and is participating in a group stop.
 * Group stop states are cleared and the group stop count is consumed if
 * %JOBCTL_STOP_CONSUME was set.  If the consumption completes the group
 * stop, the appropriate `SIGNAL_*` flags are set.
 *
 * CONTEXT:
 * Must be called with @task->sighand->siglock held.
 *
 * RETURNS:
 * %true if group stop completion should be notified to the parent, %false
 * otherwise.
 */
static bool task_participate_group_stop(struct task_struct *task)
{
	struct signal_struct *sig = task->signal;
	bool consume = task->jobctl & JOBCTL_STOP_CONSUME;

	WARN_ON_ONCE(!(task->jobctl & JOBCTL_STOP_PENDING));

	task_clear_jobctl_pending(task, JOBCTL_STOP_PENDING);

	if (!consume)
		return false;

	if (!WARN_ON_ONCE(sig->group_stop_count == 0))
		sig->group_stop_count--;

	/*
	 * Tell the caller to notify completion iff we are entering into a
	 * fresh group stop.  Read comment in do_signal_stop() for details.
	 */
	if (!sig->group_stop_count && !(sig->flags & SIGNAL_STOP_STOPPED)) {
		signal_set_stop_flags(sig, SIGNAL_STOP_STOPPED);
		return true;
	}
	return false;
}

/**
 * task_join_group_stop - 将进程加入组停止状态
 * @task: 目标进程
 *
 * 将进程加入到进程组的停止状态中，用于作业控制
 */
void task_join_group_stop(struct task_struct *task)
{
	unsigned long mask = current->jobctl & JOBCTL_STOP_SIGMASK;
	struct signal_struct *sig = current->signal;

	if (sig->group_stop_count) {
		sig->group_stop_count++;
		mask |= JOBCTL_STOP_CONSUME;
	} else if (!(sig->flags & SIGNAL_STOP_STOPPED))
		return;

	/* Have the new thread join an on-going signal group stop */
	task_set_jobctl_pending(task, mask | JOBCTL_STOP_PENDING);
}

/*
 * allocate a new signal queue record
 * - this may be called without locks if and only if t == current, otherwise an
 *   appropriate lock must be held to stop the target task from exiting
 */
/**
 * __sigqueue_alloc - 分配信号队列节点
 * @sig: 信号编号
 * @t: 目标进程
 * @flags: 内存分配标志
 * @override_rlimit: 是否忽略资源限制
 *
 * 为实时信号分配队列节点，检查用户信号队列限制
 * 返回值: 成功返回信号队列节点指针，失败返回NULL
 */
static struct sigqueue *
__sigqueue_alloc(int sig, struct task_struct *t, gfp_t flags, int override_rlimit)
{
	struct sigqueue *q = NULL;
	struct user_struct *user;
	int sigpending;

	/*
	 * Protect access to @t credentials. This can go away when all
	 * callers hold rcu read lock.
	 *
	 * NOTE! A pending signal will hold on to the user refcount,
	 * and we get/put the refcount only when the sigpending count
	 * changes from/to zero.
	 */
	rcu_read_lock();
	user = __task_cred(t)->user;
	sigpending = atomic_inc_return(&user->sigpending);
	if (sigpending == 1)
		get_uid(user);
	rcu_read_unlock();

	if (override_rlimit || likely(sigpending <= task_rlimit(t, RLIMIT_SIGPENDING))) {
		q = kmem_cache_alloc(sigqueue_cachep, flags);
	} else {
		print_dropped_signal(sig);
	}

	if (unlikely(q == NULL)) {
		if (atomic_dec_and_test(&user->sigpending))
			free_uid(user);
	} else {
		INIT_LIST_HEAD(&q->list);
		q->flags = 0;
		q->user = user;
	}

	return q;
}

static void __sigqueue_free(struct sigqueue *q)
{
	if (q->flags & SIGQUEUE_PREALLOC)
		return;
	if (atomic_dec_and_test(&q->user->sigpending))
		free_uid(q->user);
	kmem_cache_free(sigqueue_cachep, q);
}

/**
 * flush_sigqueue - 清空信号队列
 * @queue: 要清空的信号队列
 *
 * 释放信号队列中的所有挂起信号，清空队列
 */
void flush_sigqueue(struct sigpending *queue)
{
	struct sigqueue *q;

	sigemptyset(&queue->signal);
	while (!list_empty(&queue->list)) {
		q = list_entry(queue->list.next, struct sigqueue , list);
		list_del_init(&q->list);
		__sigqueue_free(q);
	}
}

/*
 * Flush all pending signals for this kthread.
 */
/**
 * flush_signals - 清空进程的所有信号
 * @t: 目标进程
 *
 * 清空进程的私有信号队列和共享信号队列，重置信号掩码
 */
void flush_signals(struct task_struct *t)
{
	unsigned long flags;

	spin_lock_irqsave(&t->sighand->siglock, flags);
	clear_tsk_thread_flag(t, TIF_SIGPENDING);
	flush_sigqueue(&t->pending);
	flush_sigqueue(&t->signal->shared_pending);
	spin_unlock_irqrestore(&t->sighand->siglock, flags);
}
EXPORT_SYMBOL(flush_signals);

#ifdef CONFIG_POSIX_TIMERS
static void __flush_itimer_signals(struct sigpending *pending)
{
	sigset_t signal, retain;
	struct sigqueue *q, *n;

	signal = pending->signal;
	sigemptyset(&retain);

	list_for_each_entry_safe(q, n, &pending->list, list) {
		int sig = q->info.si_signo;

		if (likely(q->info.si_code != SI_TIMER)) {
			sigaddset(&retain, sig);
		} else {
			sigdelset(&signal, sig);
			list_del_init(&q->list);
			__sigqueue_free(q);
		}
	}

	sigorsets(&pending->signal, &signal, &retain);
}

/**
 * flush_itimer_signals - 清空定时器相关信号
 *
 * 清空当前进程由间隔定时器(ITIMER_REAL/ITIMER_VIRTUAL/ITIMER_PROF)
 * 产生的SIGALRM、SIGVTALRM、SIGPROF信号
 */
void flush_itimer_signals(void)
{
	struct task_struct *tsk = current;
	unsigned long flags;

	spin_lock_irqsave(&tsk->sighand->siglock, flags);
	__flush_itimer_signals(&tsk->pending);
	__flush_itimer_signals(&tsk->signal->shared_pending);
	spin_unlock_irqrestore(&tsk->sighand->siglock, flags);
}
#endif

/**
 * ignore_signals - 忽略进程的所有信号
 * @t: 目标进程
 *
 * 将进程的所有信号处理器设置为SIG_IGN，用于内核线程等不需要处理信号的进程
 */
void ignore_signals(struct task_struct *t)
{
	int i;

	for (i = 0; i < _NSIG; ++i)
		t->sighand->action[i].sa.sa_handler = SIG_IGN;

	flush_signals(t);
}

/*
 * Flush all handlers for a task.
 */

/**
 * flush_signal_handlers - 重置信号处理器为默认状态
 * @t: 目标进程
 * @force_default: 是否强制设置为默认处理器
 *
 * 将进程的信号处理器重置为默认状态，用于exec等场景
 * 清除自定义信号处理器，恢复默认信号行为
 */
void
flush_signal_handlers(struct task_struct *t, int force_default)
{
	int i;
	struct k_sigaction *ka = &t->sighand->action[0];
	for (i = _NSIG ; i != 0 ; i--) {
		if (force_default || ka->sa.sa_handler != SIG_IGN)
			ka->sa.sa_handler = SIG_DFL;
		ka->sa.sa_flags = 0;
#ifdef __ARCH_HAS_SA_RESTORER
		ka->sa.sa_restorer = NULL;
#endif
		sigemptyset(&ka->sa.sa_mask);
		ka++;
	}
}

/**
 * unhandled_signal - 检查信号是否未被处理
 * @tsk: 目标进程
 * @sig: 信号编号
 *
 * 检查指定信号是否未被进程处理(默认处理或忽略)
 * 返回值: 如果信号未被处理返回true，否则返回false
 */
bool unhandled_signal(struct task_struct *tsk, int sig)
{
	void __user *handler = tsk->sighand->action[sig-1].sa.sa_handler;
	if (is_global_init(tsk))
		return true;

	if (handler != SIG_IGN && handler != SIG_DFL)
		return false;

	/* if ptraced, let the tracer determine */
	return !tsk->ptrace;
}

static void collect_signal(int sig, struct sigpending *list, kernel_siginfo_t *info,
			   bool *resched_timer)
{
	struct sigqueue *q, *first = NULL;

	/*
	 * Collect the siginfo appropriate to this signal.  Check if
	 * there is another siginfo for the same signal.
	*/
	list_for_each_entry(q, &list->list, list) {
		if (q->info.si_signo == sig) {
			if (first)
				goto still_pending;
			first = q;
		}
	}

	sigdelset(&list->signal, sig);

	if (first) {
still_pending:
		list_del_init(&first->list);
		copy_siginfo(info, &first->info);

		*resched_timer =
			(first->flags & SIGQUEUE_PREALLOC) &&
			(info->si_code == SI_TIMER) &&
			(info->si_sys_private);

		__sigqueue_free(first);
	} else {
		/*
		 * Ok, it wasn't in the queue.  This must be
		 * a fast-pathed signal or we must have been
		 * out of queue space.  So zero out the info.
		 */
		clear_siginfo(info);
		info->si_signo = sig;
		info->si_errno = 0;
		info->si_code = SI_USER;
		info->si_pid = 0;
		info->si_uid = 0;
	}
}

static int __dequeue_signal(struct sigpending *pending, sigset_t *mask,
			kernel_siginfo_t *info, bool *resched_timer)
{
	int sig = next_signal(pending, mask);

	if (sig)
		collect_signal(sig, pending, info, resched_timer);
	return sig;
}

/*
 * Dequeue a signal and return the element to the caller, which is
 * expected to free it.
 *
 * All callers have to hold the siglock.
 */
/**
 * dequeue_signal - 从信号队列中取出一个信号
 * @tsk: 目标进程
 * @mask: 信号掩码
 * @info: 存储信号信息的结构体
 *
 * 从进程的信号队列中取出下一个未被阻塞的信号进行处理
 * 返回值: 信号编号，如果没有信号则返回0
 */
int dequeue_signal(struct task_struct *tsk, sigset_t *mask, kernel_siginfo_t *info)
{
	bool resched_timer = false;
	int signr;

	/* We only dequeue private signals from ourselves, we don't let
	 * signalfd steal them
	 */
	signr = __dequeue_signal(&tsk->pending, mask, info, &resched_timer);
	if (!signr) {
		signr = __dequeue_signal(&tsk->signal->shared_pending,
					 mask, info, &resched_timer);
#ifdef CONFIG_POSIX_TIMERS
		/*
		 * itimer signal ?
		 *
		 * itimers are process shared and we restart periodic
		 * itimers in the signal delivery path to prevent DoS
		 * attacks in the high resolution timer case. This is
		 * compliant with the old way of self-restarting
		 * itimers, as the SIGALRM is a legacy signal and only
		 * queued once. Changing the restart behaviour to
		 * restart the timer in the signal dequeue path is
		 * reducing the timer noise on heavy loaded !highres
		 * systems too.
		 */
		if (unlikely(signr == SIGALRM)) {
			struct hrtimer *tmr = &tsk->signal->real_timer;

			if (!hrtimer_is_queued(tmr) &&
			    tsk->signal->it_real_incr != 0) {
				hrtimer_forward(tmr, tmr->base->get_time(),
						tsk->signal->it_real_incr);
				hrtimer_restart(tmr);
			}
		}
#endif
	}

	recalc_sigpending();
	if (!signr)
		return 0;

	if (unlikely(sig_kernel_stop(signr))) {
		/*
		 * Set a marker that we have dequeued a stop signal.  Our
		 * caller might release the siglock and then the pending
		 * stop signal it is about to process is no longer in the
		 * pending bitmasks, but must still be cleared by a SIGCONT
		 * (and overruled by a SIGKILL).  So those cases clear this
		 * shared flag after we've set it.  Note that this flag may
		 * remain set after the signal we return is ignored or
		 * handled.  That doesn't matter because its only purpose
		 * is to alert stop-signal processing code when another
		 * processor has come along and cleared the flag.
		 */
		current->jobctl |= JOBCTL_STOP_DEQUEUED;
	}
#ifdef CONFIG_POSIX_TIMERS
	if (resched_timer) {
		/*
		 * Release the siglock to ensure proper locking order
		 * of timer locks outside of siglocks.  Note, we leave
		 * irqs disabled here, since the posix-timers code is
		 * about to disable them again anyway.
		 */
		spin_unlock(&tsk->sighand->siglock);
		posixtimer_rearm(info);
		spin_lock(&tsk->sighand->siglock);

		/* Don't expose the si_sys_private value to userspace */
		info->si_sys_private = 0;
	}
#endif
	return signr;
}
EXPORT_SYMBOL_GPL(dequeue_signal);

static int dequeue_synchronous_signal(kernel_siginfo_t *info)
{
	struct task_struct *tsk = current;
	struct sigpending *pending = &tsk->pending;
	struct sigqueue *q, *sync = NULL;

	/*
	 * Might a synchronous signal be in the queue?
	 */
	if (!((pending->signal.sig[0] & ~tsk->blocked.sig[0]) & SYNCHRONOUS_MASK))
		return 0;

	/*
	 * Return the first synchronous signal in the queue.
	 */
	list_for_each_entry(q, &pending->list, list) {
		/* Synchronous signals have a positive si_code */
		if ((q->info.si_code > SI_USER) &&
		    (sigmask(q->info.si_signo) & SYNCHRONOUS_MASK)) {
			sync = q;
			goto next;
		}
	}
	return 0;
next:
	/*
	 * Check if there is another siginfo for the same signal.
	 */
	list_for_each_entry_continue(q, &pending->list, list) {
		if (q->info.si_signo == sync->info.si_signo)
			goto still_pending;
	}

	sigdelset(&pending->signal, sync->info.si_signo);
	recalc_sigpending();
still_pending:
	list_del_init(&sync->list);
	copy_siginfo(info, &sync->info);
	__sigqueue_free(sync);
	return info->si_signo;
}

/*
 * Tell a process that it has a new active signal..
 *
 * NOTE! we rely on the previous spin_lock to
 * lock interrupts for us! We can only be called with
 * "siglock" held, and the local interrupt must
 * have been disabled when that got acquired!
 *
 * No need to set need_resched since signal event passing
 * goes through ->blocked
 */
/**
 * signal_wake_up_state - 唤醒进程以处理信号
 * @t: 目标进程
 * @state: 要唤醒的进程状态掩码
 *
 * 唤醒处于指定状态的进程，使其能够处理挂起的信号
 */
void signal_wake_up_state(struct task_struct *t, unsigned int state)
{
	set_tsk_thread_flag(t, TIF_SIGPENDING);
	/*
	 * TASK_WAKEKILL also means wake it up in the stopped/traced/killable
	 * case. We don't check t->state here because there is a race with it
	 * executing another processor and just now entering stopped state.
	 * By using wake_up_state, we ensure the process will wake up and
	 * handle its death signal.
	 */
	if (!wake_up_state(t, state | TASK_INTERRUPTIBLE))
		kick_process(t);
}

/*
 * Remove signals in mask from the pending set and queue.
 * Returns 1 if any signals were found.
 *
 * All callers must be holding the siglock.
 */
static void flush_sigqueue_mask(sigset_t *mask, struct sigpending *s)
{
	struct sigqueue *q, *n;
	sigset_t m;

	sigandsets(&m, mask, &s->signal);
	if (sigisemptyset(&m))
		return;

	sigandnsets(&s->signal, &s->signal, mask);
	list_for_each_entry_safe(q, n, &s->list, list) {
		if (sigismember(mask, q->info.si_signo)) {
			list_del_init(&q->list);
			__sigqueue_free(q);
		}
	}
}

static inline int is_si_special(const struct kernel_siginfo *info)
{
	return info <= SEND_SIG_PRIV;
}

static inline bool si_fromuser(const struct kernel_siginfo *info)
{
	return info == SEND_SIG_NOINFO ||
		(!is_si_special(info) && SI_FROMUSER(info));
}

/*
 * called with RCU read lock from check_kill_permission()
 */
static bool kill_ok_by_cred(struct task_struct *t)
{
	const struct cred *cred = current_cred();
	const struct cred *tcred = __task_cred(t);

	return uid_eq(cred->euid, tcred->suid) ||
	       uid_eq(cred->euid, tcred->uid) ||
	       uid_eq(cred->uid, tcred->suid) ||
	       uid_eq(cred->uid, tcred->uid) ||
	       ns_capable(tcred->user_ns, CAP_KILL);
}

/*
 * Bad permissions for sending the signal
 * - the caller must hold the RCU read lock
 */
static int check_kill_permission(int sig, struct kernel_siginfo *info,
				 struct task_struct *t)
{
	struct pid *sid;
	int error;

	if (!valid_signal(sig))
		return -EINVAL;

	if (!si_fromuser(info))
		return 0;

	error = audit_signal_info(sig, t); /* Let audit system see the signal */
	if (error)
		return error;

	if (!same_thread_group(current, t) &&
	    !kill_ok_by_cred(t)) {
		switch (sig) {
		case SIGCONT:
			sid = task_session(t);
			/*
			 * We don't return the error if sid == NULL. The
			 * task was unhashed, the caller must notice this.
			 */
			if (!sid || sid == task_session(current))
				break;
			fallthrough;
		default:
			return -EPERM;
		}
	}

	return security_task_kill(t, info, sig, NULL);
}

/**
 * ptrace_trap_notify - schedule trap to notify ptracer
 * @t: tracee wanting to notify tracer
 *
 * This function schedules sticky ptrace trap which is cleared on the next
 * TRAP_STOP to notify ptracer of an event.  @t must have been seized by
 * ptracer.
 *
 * If @t is running, STOP trap will be taken.  If trapped for STOP and
 * ptracer is listening for events, tracee is woken up so that it can
 * re-trap for the new event.  If trapped otherwise, STOP trap will be
 * eventually taken without returning to userland after the existing traps
 * are finished by PTRACE_CONT.
 *
 * CONTEXT:
 * Must be called with @task->sighand->siglock held.
 */
static void ptrace_trap_notify(struct task_struct *t)
{
	WARN_ON_ONCE(!(t->ptrace & PT_SEIZED));
	assert_spin_locked(&t->sighand->siglock);

	task_set_jobctl_pending(t, JOBCTL_TRAP_NOTIFY);
	ptrace_signal_wake_up(t, t->jobctl & JOBCTL_LISTENING);
}

/*
 * Handle magic process-wide effects of stop/continue signals. Unlike
 * the signal actions, these happen immediately at signal-generation
 * time regardless of blocking, ignoring, or handling.  This does the
 * actual continuing for SIGCONT, but not the actual stopping for stop
 * signals. The process stop is done as a signal action for SIG_DFL.
 *
 * Returns true if the signal should be actually delivered, otherwise
 * it should be dropped.
 */
/**
 * prepare_signal - 为信号发送做准备工作
 * @sig: 信号编号
 * @p: 目标任务结构
 * @force: 是否强制发送信号
 * 返回值：如果信号可以发送返回true，否则返回false
 */
static bool prepare_signal(int sig, struct task_struct *p, bool force)
{
	struct signal_struct *signal = p->signal;
	struct task_struct *t;
	sigset_t flush;

	if (signal->flags & (SIGNAL_GROUP_EXIT | SIGNAL_GROUP_COREDUMP)) {
		if (!(signal->flags & SIGNAL_GROUP_EXIT))
			return sig == SIGKILL;
		/*
		 * The process is in the middle of dying, nothing to do.
		 */
	} else if (sig_kernel_stop(sig)) {
		/*
		 * This is a stop signal.  Remove SIGCONT from all queues.
		 */
		siginitset(&flush, sigmask(SIGCONT));
		flush_sigqueue_mask(&flush, &signal->shared_pending);
		for_each_thread(p, t)
			flush_sigqueue_mask(&flush, &t->pending);
	} else if (sig == SIGCONT) {
		unsigned int why;
		/*
		 * Remove all stop signals from all queues, wake all threads.
		 */
		siginitset(&flush, SIG_KERNEL_STOP_MASK);
		flush_sigqueue_mask(&flush, &signal->shared_pending);
		for_each_thread(p, t) {
			flush_sigqueue_mask(&flush, &t->pending);
			task_clear_jobctl_pending(t, JOBCTL_STOP_PENDING);
			if (likely(!(t->ptrace & PT_SEIZED)))
				wake_up_state(t, __TASK_STOPPED);
			else
				ptrace_trap_notify(t);
		}

		/*
		 * Notify the parent with CLD_CONTINUED if we were stopped.
		 *
		 * If we were in the middle of a group stop, we pretend it
		 * was already finished, and then continued. Since SIGCHLD
		 * doesn't queue we report only CLD_STOPPED, as if the next
		 * CLD_CONTINUED was dropped.
		 */
		why = 0;
		if (signal->flags & SIGNAL_STOP_STOPPED)
			why |= SIGNAL_CLD_CONTINUED;
		else if (signal->group_stop_count)
			why |= SIGNAL_CLD_STOPPED;

		if (why) {
			/*
			 * The first thread which returns from do_signal_stop()
			 * will take ->siglock, notice SIGNAL_CLD_MASK, and
			 * notify its parent. See get_signal().
			 */
			signal_set_stop_flags(signal, why | SIGNAL_STOP_CONTINUED);
			signal->group_stop_count = 0;
			signal->group_exit_code = 0;
		}
	}

	return !sig_ignored(p, sig, force);
}

/*
 * Test if P wants to take SIG.  After we've checked all threads with this,
 * it's equivalent to finding no threads not blocking SIG.  Any threads not
 * blocking SIG were ruled out because they are not running and already
 * have pending signals.  Such threads will dequeue from the shared queue
 * as soon as they're available, so putting the signal on the shared queue
 * will be equivalent to sending it to one such thread.
 */
static inline bool wants_signal(int sig, struct task_struct *p)
{
	if (sigismember(&p->blocked, sig))
		return false;

	if (p->flags & PF_EXITING)
		return false;

	if (sig == SIGKILL)
		return true;

	if (task_is_stopped_or_traced(p))
		return false;

	return task_curr(p) || !signal_pending(p);
}

/**
 * complete_signal - 完成信号传递处理
 * @sig: 信号编号
 * @p: 目标任务结构
 * @type: PID类型（进程、进程组、会话）
 *
 * 选择适当的线程来接收信号，并唤醒相应的进程
 */
static void complete_signal(int sig, struct task_struct *p, enum pid_type type)
{
	struct signal_struct *signal = p->signal;
	struct task_struct *t;

	/*
	 * Now find a thread we can wake up to take the signal off the queue.
	 *
	 * If the main thread wants the signal, it gets first crack.
	 * Probably the least surprising to the average bear.
	 */
	if (wants_signal(sig, p))
		t = p;
	else if ((type == PIDTYPE_PID) || thread_group_empty(p))
		/*
		 * There is just one thread and it does not need to be woken.
		 * It will dequeue unblocked signals before it runs again.
		 */
		return;
	else {
		/*
		 * Otherwise try to find a suitable thread.
		 */
		t = signal->curr_target;
		while (!wants_signal(sig, t)) {
			t = next_thread(t);
			if (t == signal->curr_target)
				/*
				 * No thread needs to be woken.
				 * Any eligible threads will see
				 * the signal in the queue soon.
				 */
				return;
		}
		signal->curr_target = t;
	}

	/*
	 * Found a killable thread.  If the signal will be fatal,
	 * then start taking the whole group down immediately.
	 */
	if (sig_fatal(p, sig) &&
	    !(signal->flags & SIGNAL_GROUP_EXIT) &&
	    !sigismember(&t->real_blocked, sig) &&
	    (sig == SIGKILL || !p->ptrace)) {
		/*
		 * This signal will be fatal to the whole group.
		 */
		if (!sig_kernel_coredump(sig)) {
			/*
			 * Start a group exit and wake everybody up.
			 * This way we don't have other threads
			 * running and doing things after a slower
			 * thread has the fatal signal pending.
			 */
			signal->flags = SIGNAL_GROUP_EXIT;
			signal->group_exit_code = sig;
			signal->group_stop_count = 0;
			t = p;
			do {
				task_clear_jobctl_pending(t, JOBCTL_PENDING_MASK);
				sigaddset(&t->pending.signal, SIGKILL);
				signal_wake_up(t, 1);
			} while_each_thread(p, t);
			return;
		}
	}

	/*
	 * The signal is already in the shared-pending queue.
	 * Tell the chosen thread to wake up and dequeue it.
	 */
	signal_wake_up(t, sig == SIGKILL);
	return;
}

static inline bool legacy_queue(struct sigpending *signals, int sig)
{
	return (sig < SIGRTMIN) && sigismember(&signals->signal, sig);
}

/**
 * __send_signal - 发送信号的核心实现函数
 * @sig: 信号编号
 * @info: 信号信息结构
 * @t: 目标任务结构
 * @type: PID类型（进程、进程组、会话）
 * @force: 是否强制发送信号
 * 返回值：成功返回0，失败返回负错误码
 */
static int __send_signal(int sig, struct kernel_siginfo *info, struct task_struct *t,
			enum pid_type type, bool force)
{
	struct sigpending *pending;
	struct sigqueue *q;
	int override_rlimit;
	int ret = 0, result;

	assert_spin_locked(&t->sighand->siglock);

	result = TRACE_SIGNAL_IGNORED;
	if (!prepare_signal(sig, t, force))
		goto ret;

	pending = (type != PIDTYPE_PID) ? &t->signal->shared_pending : &t->pending;
	/*
	 * Short-circuit ignored signals and support queuing
	 * exactly one non-rt signal, so that we can get more
	 * detailed information about the cause of the signal.
	 */
	result = TRACE_SIGNAL_ALREADY_PENDING;
	if (legacy_queue(pending, sig))
		goto ret;

	result = TRACE_SIGNAL_DELIVERED;
	/*
	 * Skip useless siginfo allocation for SIGKILL and kernel threads.
	 */
	if ((sig == SIGKILL) || (t->flags & PF_KTHREAD))
		goto out_set;

	/*
	 * Real-time signals must be queued if sent by sigqueue, or
	 * some other real-time mechanism.  It is implementation
	 * defined whether kill() does so.  We attempt to do so, on
	 * the principle of least surprise, but since kill is not
	 * allowed to fail with EAGAIN when low on memory we just
	 * make sure at least one signal gets delivered and don't
	 * pass on the info struct.
	 */
	if (sig < SIGRTMIN)
		override_rlimit = (is_si_special(info) || info->si_code >= 0);
	else
		override_rlimit = 0;

	q = __sigqueue_alloc(sig, t, GFP_ATOMIC, override_rlimit);
	if (q) {
		list_add_tail(&q->list, &pending->list);
		switch ((unsigned long) info) {
		case (unsigned long) SEND_SIG_NOINFO:
			clear_siginfo(&q->info);
			q->info.si_signo = sig;
			q->info.si_errno = 0;
			q->info.si_code = SI_USER;
			q->info.si_pid = task_tgid_nr_ns(current,
							task_active_pid_ns(t));
			rcu_read_lock();
			q->info.si_uid =
				from_kuid_munged(task_cred_xxx(t, user_ns),
						 current_uid());
			rcu_read_unlock();
			break;
		case (unsigned long) SEND_SIG_PRIV:
			clear_siginfo(&q->info);
			q->info.si_signo = sig;
			q->info.si_errno = 0;
			q->info.si_code = SI_KERNEL;
			q->info.si_pid = 0;
			q->info.si_uid = 0;
			break;
		default:
			copy_siginfo(&q->info, info);
			break;
		}
	} else if (!is_si_special(info) &&
		   sig >= SIGRTMIN && info->si_code != SI_USER) {
		/*
		 * Queue overflow, abort.  We may abort if the
		 * signal was rt and sent by user using something
		 * other than kill().
		 */
		result = TRACE_SIGNAL_OVERFLOW_FAIL;
		ret = -EAGAIN;
		goto ret;
	} else {
		/*
		 * This is a silent loss of information.  We still
		 * send the signal, but the *info bits are lost.
		 */
		result = TRACE_SIGNAL_LOSE_INFO;
	}

out_set:
	signalfd_notify(t, sig);
	sigaddset(&pending->signal, sig);

	/* Let multiprocess signals appear after on-going forks */
	if (type > PIDTYPE_TGID) {
		struct multiprocess_signals *delayed;
		hlist_for_each_entry(delayed, &t->signal->multiprocess, node) {
			sigset_t *signal = &delayed->signal;
			/* Can't queue both a stop and a continue signal */
			if (sig == SIGCONT)
				sigdelsetmask(signal, SIG_KERNEL_STOP_MASK);
			else if (sig_kernel_stop(sig))
				sigdelset(signal, SIGCONT);
			sigaddset(signal, sig);
		}
	}

	complete_signal(sig, t, type);
ret:
	trace_signal_generate(sig, info, t, type != PIDTYPE_PID, result);
	return ret;
}

static inline bool has_si_pid_and_uid(struct kernel_siginfo *info)
{
	bool ret = false;
	switch (siginfo_layout(info->si_signo, info->si_code)) {
	case SIL_KILL:
	case SIL_CHLD:
	case SIL_RT:
		ret = true;
		break;
	case SIL_TIMER:
	case SIL_POLL:
	case SIL_FAULT:
	case SIL_FAULT_MCEERR:
	case SIL_FAULT_BNDERR:
	case SIL_FAULT_PKUERR:
	case SIL_SYS:
		ret = false;
		break;
	}
	return ret;
}

/**
 * send_signal - 发送信号的高级封装函数
 * @sig: 信号编号
 * @info: 信号信息结构
 * @t: 目标任务结构
 * @type: PID类型（进程、进程组、会话）
 * 返回值：成功返回0，失败返回负错误码
 */
static int send_signal(int sig, struct kernel_siginfo *info, struct task_struct *t,
			enum pid_type type)
{
	/* Should SIGKILL or SIGSTOP be received by a pid namespace init? */
	bool force = false;

	if (info == SEND_SIG_NOINFO) {
		/* Force if sent from an ancestor pid namespace */
		force = !task_pid_nr_ns(current, task_active_pid_ns(t));
	} else if (info == SEND_SIG_PRIV) {
		/* Don't ignore kernel generated signals */
		force = true;
	} else if (has_si_pid_and_uid(info)) {
		/* SIGKILL and SIGSTOP is special or has ids */
		struct user_namespace *t_user_ns;

		rcu_read_lock();
		t_user_ns = task_cred_xxx(t, user_ns);
		if (current_user_ns() != t_user_ns) {
			kuid_t uid = make_kuid(current_user_ns(), info->si_uid);
			info->si_uid = from_kuid_munged(t_user_ns, uid);
		}
		rcu_read_unlock();

		/* A kernel generated signal? */
		force = (info->si_code == SI_KERNEL);

		/* From an ancestor pid namespace? */
		if (!task_pid_nr_ns(current, task_active_pid_ns(t))) {
			info->si_pid = 0;
			force = true;
		}
	}
	return __send_signal(sig, info, t, type, force);
}

static void print_fatal_signal(int signr)
{
	struct pt_regs *regs = signal_pt_regs();
	pr_info("potentially unexpected fatal signal %d.\n", signr);

#if defined(__i386__) && !defined(__arch_um__)
	pr_info("code at %08lx: ", regs->ip);
	{
		int i;
		for (i = 0; i < 16; i++) {
			unsigned char insn;

			if (get_user(insn, (unsigned char *)(regs->ip + i)))
				break;
			pr_cont("%02x ", insn);
		}
	}
	pr_cont("\n");
#endif
	preempt_disable();
	show_regs(regs);
	preempt_enable();
}

static int __init setup_print_fatal_signals(char *str)
{
	get_option (&str, &print_fatal_signals);

	return 1;
}

__setup("print-fatal-signals=", setup_print_fatal_signals);

/**
 * __group_send_sig_info - 向进程组发送信号的内部实现
 * @sig: 信号编号
 * @info: 信号信息结构体
 * @p: 进程组中的一个进程
 *
 * 向整个进程组(线程组)发送信号的底层实现函数
 * 返回值: 成功返回0，失败返回负值错误码
 */
int
__group_send_sig_info(int sig, struct kernel_siginfo *info, struct task_struct *p)
{
	return send_signal(sig, info, p, PIDTYPE_TGID);
}

/**
 * do_send_sig_info - 向进程发送信号信息
 * @sig: 信号编号
 * @info: 信号信息结构
 * @p: 目标进程
 * @type: PID类型（进程、进程组、会话）
 * 返回值：成功返回0，失败返回负错误码
 */
/**
 * do_send_sig_info - 向进程发送信号的核心函数
 * @sig: 信号编号
 * @info: 信号信息结构体
 * @p: 目标进程
 * @type: 信号类型(PIDTYPE_PID/PIDTYPE_TGID等)
 *
 * 这是信号发送的核心实现函数，负责将信号添加到目标进程的信号队列
 * 返回值: 成功返回0，失败返回负值错误码
 */
int do_send_sig_info(int sig, struct kernel_siginfo *info, struct task_struct *p,
			enum pid_type type)
{
	unsigned long flags;
	int ret = -ESRCH;

	if (lock_task_sighand(p, &flags)) {  // 获取目标进程的信号处理锁
		ret = send_signal(sig, info, p, type);  // 在锁保护下发送信号到进程队列
		unlock_task_sighand(p, &flags);  // 释放信号处理锁
	}

	return ret;
}

/*
 * Force a signal that the process can't ignore: if necessary
 * we unblock the signal and change any SIG_IGN to SIG_DFL.
 *
 * Note: If we unblock the signal, we always reset it to SIG_DFL,
 * since we do not want to have a signal handler that was blocked
 * be invoked when user space had explicitly blocked it.
 *
 * We don't want to have recursive SIGSEGV's etc, for example,
 * that is why we also clear SIGNAL_UNKILLABLE.
 */
/**
 * force_sig_info_to_task - 强制向指定进程发送信号
 * @info: 信号信息结构体
 * @t: 目标进程
 *
 * 强制向指定进程发送信号，忽略信号掩码和处理器设置
 * 主要用于内核检测到严重错误需要强制终止特定进程的场景
 * 返回值: 发送结果状态码
 */
static int
force_sig_info_to_task(struct kernel_siginfo *info, struct task_struct *t)
{
	unsigned long int flags;
	int ret, blocked, ignored;
	struct k_sigaction *action;
	int sig = info->si_signo;

	spin_lock_irqsave(&t->sighand->siglock, flags);
	action = &t->sighand->action[sig-1];
	ignored = action->sa.sa_handler == SIG_IGN;
	blocked = sigismember(&t->blocked, sig);
	if (blocked || ignored) {
		action->sa.sa_handler = SIG_DFL;
		if (blocked) {
			sigdelset(&t->blocked, sig);
			recalc_sigpending_and_wake(t);
		}
	}
	/*
	 * Don't clear SIGNAL_UNKILLABLE for traced tasks, users won't expect
	 * debugging to leave init killable.
	 */
	if (action->sa.sa_handler == SIG_DFL && !t->ptrace)
		t->signal->flags &= ~SIGNAL_UNKILLABLE;
	ret = send_signal(sig, info, t, PIDTYPE_PID);
	spin_unlock_irqrestore(&t->sighand->siglock, flags);

	return ret;
}

/**
 * force_sig_info - 强制发送信号给当前进程
 * @info: 信号信息结构
 * 返回值：成功返回0，失败返回负错误码
 */
/**
 * force_sig_info - 强制发送信号给当前进程
 * @info: 信号信息结构体
 *
 * 强制向当前进程发送信号，无视信号掩码和处理器设置
 * 主要用于内核需要强制终止进程的场景
 * 返回值: 发送结果
 */
int force_sig_info(struct kernel_siginfo *info)
{
	return force_sig_info_to_task(info, current);
}

/*
 * Nuke all other threads in the group.
 */
/**
 * zap_other_threads - 终止进程组中的其他线程
 * @p: 目标进程
 *
 * 向进程组中除当前进程外的所有线程发送SIGKILL信号
 * 用于exec系统调用和进程终止时清理其他线程
 * 返回值: 被终止的线程数量
 */
int zap_other_threads(struct task_struct *p)
{
	struct task_struct *t = p;
	int count = 0;

	p->signal->group_stop_count = 0;

	while_each_thread(p, t) {
		task_clear_jobctl_pending(t, JOBCTL_PENDING_MASK);
		count++;

		/* Don't bother with already dead threads */
		if (t->exit_state)
			continue;
		sigaddset(&t->pending.signal, SIGKILL);
		signal_wake_up(t, 1);
	}

	return count;
}

struct sighand_struct *__lock_task_sighand(struct task_struct *tsk,
					   unsigned long *flags)
{
	struct sighand_struct *sighand;

	rcu_read_lock();
	for (;;) {
		sighand = rcu_dereference(tsk->sighand);
		if (unlikely(sighand == NULL))
			break;

		/*
		 * This sighand can be already freed and even reused, but
		 * we rely on SLAB_TYPESAFE_BY_RCU and sighand_ctor() which
		 * initializes ->siglock: this slab can't go away, it has
		 * the same object type, ->siglock can't be reinitialized.
		 *
		 * We need to ensure that tsk->sighand is still the same
		 * after we take the lock, we can race with de_thread() or
		 * __exit_signal(). In the latter case the next iteration
		 * must see ->sighand == NULL.
		 */
		spin_lock_irqsave(&sighand->siglock, *flags);
		if (likely(sighand == rcu_access_pointer(tsk->sighand)))
			break;
		spin_unlock_irqrestore(&sighand->siglock, *flags);
	}
	rcu_read_unlock();

	return sighand;
}

/*
 * send signal info to all the members of a group
 */
/**
 * group_send_sig_info - 向进程组发送信号
 * @sig: 信号编号
 * @info: 信号信息结构体
 * @p: 进程组中的一个进程
 * @type: 信号发送类型
 *
 * 向整个进程组(线程组)发送信号，所有线程都会收到该信号
 * 返回值: 成功返回0，失败返回负值错误码
 */
int group_send_sig_info(int sig, struct kernel_siginfo *info,
			struct task_struct *p, enum pid_type type)
{
	int ret;

	rcu_read_lock();
	ret = check_kill_permission(sig, info, p);
	rcu_read_unlock();

	if (!ret && sig)
		ret = do_send_sig_info(sig, info, p, type);

	return ret;
}

/*
 * __kill_pgrp_info() sends a signal to a process group: this is what the tty
 * control characters do (^C, ^Z etc)
 * - the caller must hold at least a readlock on tasklist_lock
 */
/**
 * __kill_pgrp_info - 向进程组发送信号的内部函数
 * @sig: 信号编号
 * @info: 信号信息结构体
 * @pgrp: 目标进程组PID
 *
 * 向指定进程组的所有进程发送信号，这是kill(-pid)系统调用的实现
 * 返回值: 成功返回0，失败返回负值错误码
 */
int __kill_pgrp_info(int sig, struct kernel_siginfo *info, struct pid *pgrp)
{
	struct task_struct *p = NULL;
	int retval, success;

	success = 0;
	retval = -ESRCH;
	do_each_pid_task(pgrp, PIDTYPE_PGID, p) {
		int err = group_send_sig_info(sig, info, p, PIDTYPE_PGID);
		success |= !err;
		retval = err;
	} while_each_pid_task(pgrp, PIDTYPE_PGID, p);
	return success ? 0 : retval;
}

/**
 * kill_pid_info - 向指定PID发送信号
 * @sig: 信号编号
 * @info: 信号信息结构体
 * @pid: 目标进程的PID结构
 *
 * 向指定PID的进程发送信号，这是kill()系统调用的核心实现
 * 返回值: 成功返回0，失败返回负值错误码
 */
int kill_pid_info(int sig, struct kernel_siginfo *info, struct pid *pid)
{
	int error = -ESRCH;
	struct task_struct *p;

	for (;;) {
		rcu_read_lock();
		p = pid_task(pid, PIDTYPE_PID);
		if (p)
			error = group_send_sig_info(sig, info, p, PIDTYPE_TGID);
		rcu_read_unlock();
		if (likely(!p || error != -ESRCH))
			return error;

		/*
		 * The task was unhashed in between, try again.  If it
		 * is dead, pid_task() will return NULL, if we race with
		 * de_thread() it will find the new leader.
		 */
	}
}

static int kill_proc_info(int sig, struct kernel_siginfo *info, pid_t pid)
{
	int error;
	rcu_read_lock();
	error = kill_pid_info(sig, info, find_vpid(pid));
	rcu_read_unlock();
	return error;
}

static inline bool kill_as_cred_perm(const struct cred *cred,
				     struct task_struct *target)
{
	const struct cred *pcred = __task_cred(target);

	return uid_eq(cred->euid, pcred->suid) ||
	       uid_eq(cred->euid, pcred->uid) ||
	       uid_eq(cred->uid, pcred->suid) ||
	       uid_eq(cred->uid, pcred->uid);
}

/*
 * The usb asyncio usage of siginfo is wrong.  The glibc support
 * for asyncio which uses SI_ASYNCIO assumes the layout is SIL_RT.
 * AKA after the generic fields:
 *	kernel_pid_t	si_pid;
 *	kernel_uid32_t	si_uid;
 *	sigval_t	si_value;
 *
 * Unfortunately when usb generates SI_ASYNCIO it assumes the layout
 * after the generic fields is:
 *	void __user 	*si_addr;
 *
 * This is a practical problem when there is a 64bit big endian kernel
 * and a 32bit userspace.  As the 32bit address will encoded in the low
 * 32bits of the pointer.  Those low 32bits will be stored at higher
 * address than appear in a 32 bit pointer.  So userspace will not
 * see the address it was expecting for it's completions.
 *
 * There is nothing in the encoding that can allow
 * copy_siginfo_to_user32 to detect this confusion of formats, so
 * handle this by requiring the caller of kill_pid_usb_asyncio to
 * notice when this situration takes place and to store the 32bit
 * pointer in sival_int, instead of sival_addr of the sigval_t addr
 * parameter.
 */
/**
 * kill_pid_usb_asyncio - USB异步I/O专用的信号发送函数
 * @sig: 信号编号
 * @errno: 错误码
 * @addr: 信号值地址
 * @pid: 目标进程PID
 *
 * 用于USB子系统向用户进程发送异步I/O完成信号
 * 返回值: 发送结果状态码
 */
int kill_pid_usb_asyncio(int sig, int errno, sigval_t addr,
			 struct pid *pid, const struct cred *cred)
{
	struct kernel_siginfo info;
	struct task_struct *p;
	unsigned long flags;
	int ret = -EINVAL;

	if (!valid_signal(sig))
		return ret;

	clear_siginfo(&info);
	info.si_signo = sig;
	info.si_errno = errno;
	info.si_code = SI_ASYNCIO;
	*((sigval_t *)&info.si_pid) = addr;

	rcu_read_lock();
	p = pid_task(pid, PIDTYPE_PID);
	if (!p) {
		ret = -ESRCH;
		goto out_unlock;
	}
	if (!kill_as_cred_perm(cred, p)) {
		ret = -EPERM;
		goto out_unlock;
	}
	ret = security_task_kill(p, &info, sig, cred);
	if (ret)
		goto out_unlock;

	if (sig) {
		if (lock_task_sighand(p, &flags)) {
			ret = __send_signal(sig, &info, p, PIDTYPE_TGID, false);
			unlock_task_sighand(p, &flags);
		} else
			ret = -ESRCH;
	}
out_unlock:
	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(kill_pid_usb_asyncio);

/*
 * kill_something_info() interprets pid in interesting ways just like kill(2).
 *
 * POSIX specifies that kill(-1,sig) is unspecified, but what we have
 * is probably wrong.  Should make it like BSD or SYSV.
 */

static int kill_something_info(int sig, struct kernel_siginfo *info, pid_t pid)
{
	int ret;

	if (pid > 0)
		return kill_proc_info(sig, info, pid);

	/* -INT_MIN is undefined.  Exclude this case to avoid a UBSAN warning */
	if (pid == INT_MIN)
		return -ESRCH;

	read_lock(&tasklist_lock);
	if (pid != -1) {
		ret = __kill_pgrp_info(sig, info,
				pid ? find_vpid(-pid) : task_pgrp(current));
	} else {
		int retval = 0, count = 0;
		struct task_struct * p;

		for_each_process(p) {
			if (task_pid_vnr(p) > 1 &&
					!same_thread_group(p, current)) {
				int err = group_send_sig_info(sig, info, p,
							      PIDTYPE_MAX);
				++count;
				if (err != -EPERM)
					retval = err;
			}
		}
		ret = count ? retval : -ESRCH;
	}
	read_unlock(&tasklist_lock);

	return ret;
}

/*
 * These are for backward compatibility with the rest of the kernel source.
 */

/**
 * send_sig_info - 向指定进程发送信号
 * @sig: 信号编号
 * @info: 信号信息结构体
 * @p: 目标进程
 *
 * 向指定进程发送信号，会进行权限检查
 * 返回值: 成功返回0，失败返回负值错误码
 */
int send_sig_info(int sig, struct kernel_siginfo *info, struct task_struct *p)
{
	/*
	 * Make sure legacy kernel users don't send in bad values
	 * (normal paths check this in check_kill_permission).
	 */
	if (!valid_signal(sig))
		return -EINVAL;

	return do_send_sig_info(sig, info, p, PIDTYPE_PID);
}
EXPORT_SYMBOL(send_sig_info);

#define __si_special(priv) \
	((priv) ? SEND_SIG_PRIV : SEND_SIG_NOINFO)

/**
 * send_sig - 发送信号给指定进程
 * @sig: 信号编号
 * @p: 目标进程
 * @priv: 权限标志(1表示特权操作)
 *
 * 向指定进程发送信号，这是一个便捷的包装函数
 * 返回值: 成功返回0，失败返回负值错误码
 */
int
send_sig(int sig, struct task_struct *p, int priv)
{
	return send_sig_info(sig, __si_special(priv), p);
}
EXPORT_SYMBOL(send_sig);

/**
 * force_sig - 强制发送信号给当前进程
 * @sig: 信号编号
 *
 * 强制向当前进程发送指定信号，忽略信号掩码和处理器设置
 * 主要用于内核检测到严重错误需要终止进程的场景
 */
void force_sig(int sig)
{
	struct kernel_siginfo info;

	clear_siginfo(&info);
	info.si_signo = sig;
	info.si_errno = 0;
	info.si_code = SI_KERNEL;
	info.si_pid = 0;
	info.si_uid = 0;
	force_sig_info(&info);
}
EXPORT_SYMBOL(force_sig);

/*
 * When things go south during signal handling, we
 * will force a SIGSEGV. And if the signal that caused
 * the problem was already a SIGSEGV, we'll want to
 * make sure we don't even try to deliver the signal..
 */
/**
 * force_sigsegv - 强制发送SIGSEGV信号
 * @sig: 原始信号编号(用于记录)
 *
 * 当信号处理器本身出现段错误时，强制发送SIGSEGV信号
 * 这是处理信号处理过程中错误的最后手段
 */
void force_sigsegv(int sig)
{
	struct task_struct *p = current;

	if (sig == SIGSEGV) {
		unsigned long flags;
		spin_lock_irqsave(&p->sighand->siglock, flags);
		p->sighand->action[sig - 1].sa.sa_handler = SIG_DFL;
		spin_unlock_irqrestore(&p->sighand->siglock, flags);
	}
	force_sig(SIGSEGV);
}

/**
 * force_sig_fault_to_task - 强制向指定进程发送错误信号
 * @sig: 信号编号(SIGSEGV/SIGBUS等)
 * @code: 信号代码(如SEGV_MAPERR/SEGV_ACCERR等)
 * @addr: 错误地址
 * @t: 目标进程
 *
 * 向指定进程发送内存访问错误等故障信号
 * 返回值: 发送结果状态码
 */
int force_sig_fault_to_task(int sig, int code, void __user *addr
	___ARCH_SI_TRAPNO(int trapno)
	___ARCH_SI_IA64(int imm, unsigned int flags, unsigned long isr)
	, struct task_struct *t)
{
	struct kernel_siginfo info;

	clear_siginfo(&info);
	info.si_signo = sig;
	info.si_errno = 0;
	info.si_code  = code;
	info.si_addr  = addr;
#ifdef __ARCH_SI_TRAPNO
	info.si_trapno = trapno;
#endif
#ifdef __ia64__
	info.si_imm = imm;
	info.si_flags = flags;
	info.si_isr = isr;
#endif
	return force_sig_info_to_task(&info, t);
}

/**
 * force_sig_fault - 强制发送错误信号给当前进程
 * @sig: 信号编号(SIGSEGV/SIGBUS等)
 * @code: 信号代码
 * @addr: 错误地址
 *
 * 向当前进程发送内存访问错误信号
 * 返回值: 发送结果状态码
 */
int force_sig_fault(int sig, int code, void __user *addr
	___ARCH_SI_TRAPNO(int trapno)
	___ARCH_SI_IA64(int imm, unsigned int flags, unsigned long isr))
{
	return force_sig_fault_to_task(sig, code, addr
				       ___ARCH_SI_TRAPNO(trapno)
				       ___ARCH_SI_IA64(imm, flags, isr), current);
}

/**
 * send_sig_fault - 发送错误信号给指定进程
 * @sig: 信号编号(SIGSEGV/SIGBUS等)
 * @code: 信号代码
 * @addr: 错误地址
 * @t: 目标进程
 *
 * 向指定进程发送内存访问错误信号，会进行权限检查
 * 返回值: 发送结果状态码
 */
int send_sig_fault(int sig, int code, void __user *addr
	___ARCH_SI_TRAPNO(int trapno)
	___ARCH_SI_IA64(int imm, unsigned int flags, unsigned long isr)
	, struct task_struct *t)
{
	struct kernel_siginfo info;

	clear_siginfo(&info);
	info.si_signo = sig;
	info.si_errno = 0;
	info.si_code  = code;
	info.si_addr  = addr;
#ifdef __ARCH_SI_TRAPNO
	info.si_trapno = trapno;
#endif
#ifdef __ia64__
	info.si_imm = imm;
	info.si_flags = flags;
	info.si_isr = isr;
#endif
	return send_sig_info(info.si_signo, &info, t);
}

/**
 * force_sig_mceerr - 强制发送内存校正错误信号
 * @code: 错误代码(BUS_MCEERR_AR/BUS_MCEERR_AO)
 * @addr: 错误内存地址
 * @lsb: 最低有效位，表示错误粒度
 *
 * 当检测到内存ECC错误时发送SIGBUS信号给当前进程
 * 返回值: 发送结果状态码
 */
int force_sig_mceerr(int code, void __user *addr, short lsb)
{
	struct kernel_siginfo info;

	WARN_ON((code != BUS_MCEERR_AO) && (code != BUS_MCEERR_AR));
	clear_siginfo(&info);
	info.si_signo = SIGBUS;
	info.si_errno = 0;
	info.si_code = code;
	info.si_addr = addr;
	info.si_addr_lsb = lsb;
	return force_sig_info(&info);
}

/**
 * send_sig_mceerr - 发送内存校正错误信号给指定进程
 * @code: 错误代码(BUS_MCEERR_AR/BUS_MCEERR_AO)
 * @addr: 错误内存地址
 * @lsb: 最低有效位，表示错误粒度
 * @t: 目标进程
 *
 * 向指定进程发送内存ECC错误的SIGBUS信号
 * 返回值: 发送结果状态码
 */
int send_sig_mceerr(int code, void __user *addr, short lsb, struct task_struct *t)
{
	struct kernel_siginfo info;

	WARN_ON((code != BUS_MCEERR_AO) && (code != BUS_MCEERR_AR));
	clear_siginfo(&info);
	info.si_signo = SIGBUS;
	info.si_errno = 0;
	info.si_code = code;
	info.si_addr = addr;
	info.si_addr_lsb = lsb;
	return send_sig_info(info.si_signo, &info, t);
}
EXPORT_SYMBOL(send_sig_mceerr);

/**
 * force_sig_bnderr - 强制发送内存边界错误信号
 * @addr: 错误访问的地址
 * @lower: 有效内存区域的下界
 * @upper: 有效内存区域的上界
 *
 * 当发生内存边界检查错误时发送SIGSEGV信号给当前进程
 * 主要用于Intel MPX(Memory Protection Extensions)功能
 * 返回值: 发送结果状态码
 */
int force_sig_bnderr(void __user *addr, void __user *lower, void __user *upper)
{
	struct kernel_siginfo info;

	clear_siginfo(&info);
	info.si_signo = SIGSEGV;
	info.si_errno = 0;
	info.si_code  = SEGV_BNDERR;
	info.si_addr  = addr;
	info.si_lower = lower;
	info.si_upper = upper;
	return force_sig_info(&info);
}

#ifdef SEGV_PKUERR
/**
 * force_sig_pkuerr - 强制发送保护密钥错误信号
 * @addr: 错误访问的地址
 * @pkey: 保护密钥值
 *
 * 当违反内存保护密钥权限时发送SIGSEGV信号给当前进程
 * 用于Intel PKU(Protection Keys for Userspace)功能
 * 返回值: 发送结果状态码
 */
int force_sig_pkuerr(void __user *addr, u32 pkey)
{
	struct kernel_siginfo info;

	clear_siginfo(&info);
	info.si_signo = SIGSEGV;
	info.si_errno = 0;
	info.si_code  = SEGV_PKUERR;
	info.si_addr  = addr;
	info.si_pkey  = pkey;
	return force_sig_info(&info);
}
#endif

/* For the crazy architectures that include trap information in
 * the errno field, instead of an actual errno value.
 */
/**
 * force_sig_ptrace_errno_trap - 强制发送ptrace错误陷阱信号
 * @errno: 错误码
 * @addr: 陷阱地址
 *
 * 在ptrace过程中发生错误时发送SIGTRAP信号给当前进程
 * 用于调试器与被调试进程的错误处理
 * 返回值: 发送结果状态码
 */
int force_sig_ptrace_errno_trap(int errno, void __user *addr)
{
	struct kernel_siginfo info;

	clear_siginfo(&info);
	info.si_signo = SIGTRAP;
	info.si_errno = errno;
	info.si_code  = TRAP_HWBKPT;
	info.si_addr  = addr;
	return force_sig_info(&info);
}

/**
 * kill_pgrp - 向进程组发送信号
 * @pid: 进程组PID
 * @sig: 信号编号
 * @priv: 权限级别（0=用户信号，1=特权信号）
 * 返回值：成功返回0，失败返回负错误码
 */
/**
 * kill_pgrp - 向进程组发送信号
 * @pid: 进程组的PID结构
 * @sig: 信号编号
 * @priv: 权限标志(1表示特权操作)
 *
 * 向指定进程组的所有进程发送信号，实现kill(-pgid, sig)功能
 * 返回值: 成功返回0，失败返回负值错误码
 */
int kill_pgrp(struct pid *pid, int sig, int priv)
{
	int ret;

	read_lock(&tasklist_lock);
	ret = __kill_pgrp_info(sig, __si_special(priv), pid);
	read_unlock(&tasklist_lock);

	return ret;
}
EXPORT_SYMBOL(kill_pgrp);

/**
 * kill_pid - 向指定PID的进程发送信号
 * @pid: 目标进程PID
 * @sig: 信号编号
 * @priv: 权限级别（0=用户信号，1=特权信号）
 * 返回值：成功返回0，失败返回负错误码
 */
/**
 * kill_pid - 向指定PID发送信号
 * @pid: 目标进程的PID结构
 * @sig: 信号编号
 * @priv: 权限标志(1表示特权操作)
 *
 * 向指定PID的进程发送信号，实现kill(pid, sig)系统调用
 * 返回值: 成功返回0，失败返回负值错误码
 */
int kill_pid(struct pid *pid, int sig, int priv)
{
	return kill_pid_info(sig, __si_special(priv), pid);
}
EXPORT_SYMBOL(kill_pid);

/*
 * These functions support sending signals using preallocated sigqueue
 * structures.  This is needed "because realtime applications cannot
 * afford to lose notifications of asynchronous events, like timer
 * expirations or I/O completions".  In the case of POSIX Timers
 * we allocate the sigqueue structure from the timer_create.  If this
 * allocation fails we are able to report the failure to the application
 * with an EAGAIN error.
 */
struct sigqueue *sigqueue_alloc(void)
{
	struct sigqueue *q = __sigqueue_alloc(-1, current, GFP_KERNEL, 0);

	if (q)
		q->flags |= SIGQUEUE_PREALLOC;

	return q;
}

/**
 * sigqueue_free - 释放信号队列节点
 * @q: 要释放的信号队列节点
 *
 * 释放由__sigqueue_alloc分配的信号队列节点内存，
 * 同时更新用户的信号队列使用计数
 */
void sigqueue_free(struct sigqueue *q)
{
	unsigned long flags;
	spinlock_t *lock = &current->sighand->siglock;

	BUG_ON(!(q->flags & SIGQUEUE_PREALLOC));
	/*
	 * We must hold ->siglock while testing q->list
	 * to serialize with collect_signal() or with
	 * __exit_signal()->flush_sigqueue().
	 */
	spin_lock_irqsave(lock, flags);
	q->flags &= ~SIGQUEUE_PREALLOC;
	/*
	 * If it is queued it will be freed when dequeued,
	 * like the "regular" sigqueue.
	 */
	if (!list_empty(&q->list))
		q = NULL;
	spin_unlock_irqrestore(lock, flags);

	if (q)
		__sigqueue_free(q);
}

/**
 * send_sigqueue - 发送队列化的实时信号
 * @q: 预分配的信号队列节点
 * @pid: 目标进程PID
 * @type: PID类型(PIDTYPE_PID/PIDTYPE_TGID等)
 *
 * 发送实时信号到目标进程的信号队列，用于定时器和异步I/O
 * 返回值: 成功返回0，失败返回负值错误码
 */
int send_sigqueue(struct sigqueue *q, struct pid *pid, enum pid_type type)
{
	int sig = q->info.si_signo;
	struct sigpending *pending;
	struct task_struct *t;
	unsigned long flags;
	int ret, result;

	BUG_ON(!(q->flags & SIGQUEUE_PREALLOC));

	ret = -1;
	rcu_read_lock();
	t = pid_task(pid, type);
	if (!t || !likely(lock_task_sighand(t, &flags)))
		goto ret;

	ret = 1; /* the signal is ignored */
	result = TRACE_SIGNAL_IGNORED;
	if (!prepare_signal(sig, t, false))
		goto out;

	ret = 0;
	if (unlikely(!list_empty(&q->list))) {
		/*
		 * If an SI_TIMER entry is already queue just increment
		 * the overrun count.
		 */
		BUG_ON(q->info.si_code != SI_TIMER);
		q->info.si_overrun++;
		result = TRACE_SIGNAL_ALREADY_PENDING;
		goto out;
	}
	q->info.si_overrun = 0;

	signalfd_notify(t, sig);
	pending = (type != PIDTYPE_PID) ? &t->signal->shared_pending : &t->pending;
	list_add_tail(&q->list, &pending->list);
	sigaddset(&pending->signal, sig);
	complete_signal(sig, t, type);
	result = TRACE_SIGNAL_DELIVERED;
out:
	trace_signal_generate(sig, &q->info, t, type != PIDTYPE_PID, result);
	unlock_task_sighand(t, &flags);
ret:
	rcu_read_unlock();
	return ret;
}

static void do_notify_pidfd(struct task_struct *task)
{
	struct pid *pid;

	WARN_ON(task->exit_state == 0);
	pid = task_pid(task);
	wake_up_all(&pid->wait_pidfd);
}

/*
 * Let a parent know about the death of a child.
 * For a stopped/continued status change, use do_notify_parent_cldstop instead.
 *
 * Returns true if our parent ignored us and so we've switched to
 * self-reaping.
 */
/**
 * do_notify_parent - 通知父进程子进程状态变化
 * @tsk: 状态发生变化的子进程
 * @sig: 要发送给父进程的信号
 *
 * 当子进程退出或状态改变时，向父进程发送SIGCHLD等信号
 * 用于实现wait()系统调用的进程状态通知机制
 * 返回值: 如果父进程忽略了信号，返回true(进程自动回收)，否则返回false
 */
bool do_notify_parent(struct task_struct *tsk, int sig)
{
	struct kernel_siginfo info;
	unsigned long flags;
	struct sighand_struct *psig;
	bool autoreap = false;
	u64 utime, stime;

	BUG_ON(sig == -1);

 	/* do_notify_parent_cldstop should have been called instead.  */
 	BUG_ON(task_is_stopped_or_traced(tsk));

	BUG_ON(!tsk->ptrace &&
	       (tsk->group_leader != tsk || !thread_group_empty(tsk)));

	/* Wake up all pidfd waiters */
	do_notify_pidfd(tsk);

	if (sig != SIGCHLD) {
		/*
		 * This is only possible if parent == real_parent.
		 * Check if it has changed security domain.
		 */
		if (tsk->parent_exec_id != READ_ONCE(tsk->parent->self_exec_id))
			sig = SIGCHLD;
	}

	clear_siginfo(&info);
	info.si_signo = sig;
	info.si_errno = 0;
	/*
	 * We are under tasklist_lock here so our parent is tied to
	 * us and cannot change.
	 *
	 * task_active_pid_ns will always return the same pid namespace
	 * until a task passes through release_task.
	 *
	 * write_lock() currently calls preempt_disable() which is the
	 * same as rcu_read_lock(), but according to Oleg, this is not
	 * correct to rely on this
	 */
	rcu_read_lock();
	info.si_pid = task_pid_nr_ns(tsk, task_active_pid_ns(tsk->parent));
	info.si_uid = from_kuid_munged(task_cred_xxx(tsk->parent, user_ns),
				       task_uid(tsk));
	rcu_read_unlock();

	task_cputime(tsk, &utime, &stime);
	info.si_utime = nsec_to_clock_t(utime + tsk->signal->utime);
	info.si_stime = nsec_to_clock_t(stime + tsk->signal->stime);

	info.si_status = tsk->exit_code & 0x7f;
	if (tsk->exit_code & 0x80)
		info.si_code = CLD_DUMPED;
	else if (tsk->exit_code & 0x7f)
		info.si_code = CLD_KILLED;
	else {
		info.si_code = CLD_EXITED;
		info.si_status = tsk->exit_code >> 8;
	}

	psig = tsk->parent->sighand;
	spin_lock_irqsave(&psig->siglock, flags);
	if (!tsk->ptrace && sig == SIGCHLD &&
	    (psig->action[SIGCHLD-1].sa.sa_handler == SIG_IGN ||
	     (psig->action[SIGCHLD-1].sa.sa_flags & SA_NOCLDWAIT))) {
		/*
		 * We are exiting and our parent doesn't care.  POSIX.1
		 * defines special semantics for setting SIGCHLD to SIG_IGN
		 * or setting the SA_NOCLDWAIT flag: we should be reaped
		 * automatically and not left for our parent's wait4 call.
		 * Rather than having the parent do it as a magic kind of
		 * signal handler, we just set this to tell do_exit that we
		 * can be cleaned up without becoming a zombie.  Note that
		 * we still call __wake_up_parent in this case, because a
		 * blocked sys_wait4 might now return -ECHILD.
		 *
		 * Whether we send SIGCHLD or not for SA_NOCLDWAIT
		 * is implementation-defined: we do (if you don't want
		 * it, just use SIG_IGN instead).
		 */
		autoreap = true;
		if (psig->action[SIGCHLD-1].sa.sa_handler == SIG_IGN)
			sig = 0;
	}
	/*
	 * Send with __send_signal as si_pid and si_uid are in the
	 * parent's namespaces.
	 */
	if (valid_signal(sig) && sig)
		__send_signal(sig, &info, tsk->parent, PIDTYPE_TGID, false);
	__wake_up_parent(tsk, tsk->parent);
	spin_unlock_irqrestore(&psig->siglock, flags);

	return autoreap;
}

/**
 * do_notify_parent_cldstop - notify parent of stopped/continued state change
 * @tsk: task reporting the state change
 * @for_ptracer: the notification is for ptracer
 * @why: CLD_{CONTINUED|STOPPED|TRAPPED} to report
 *
 * Notify @tsk's parent that the stopped/continued state has changed.  If
 * @for_ptracer is %false, @tsk's group leader notifies to its real parent.
 * If %true, @tsk reports to @tsk->parent which should be the ptracer.
 *
 * CONTEXT:
 * Must be called with tasklist_lock at least read locked.
 */
static void do_notify_parent_cldstop(struct task_struct *tsk,
				     bool for_ptracer, int why)
{
	struct kernel_siginfo info;
	unsigned long flags;
	struct task_struct *parent;
	struct sighand_struct *sighand;
	u64 utime, stime;

	if (for_ptracer) {
		parent = tsk->parent;
	} else {
		tsk = tsk->group_leader;
		parent = tsk->real_parent;
	}

	clear_siginfo(&info);
	info.si_signo = SIGCHLD;
	info.si_errno = 0;
	/*
	 * see comment in do_notify_parent() about the following 4 lines
	 */
	rcu_read_lock();
	info.si_pid = task_pid_nr_ns(tsk, task_active_pid_ns(parent));
	info.si_uid = from_kuid_munged(task_cred_xxx(parent, user_ns), task_uid(tsk));
	rcu_read_unlock();

	task_cputime(tsk, &utime, &stime);
	info.si_utime = nsec_to_clock_t(utime);
	info.si_stime = nsec_to_clock_t(stime);

 	info.si_code = why;
 	switch (why) {
 	case CLD_CONTINUED:
 		info.si_status = SIGCONT;
 		break;
 	case CLD_STOPPED:
 		info.si_status = tsk->signal->group_exit_code & 0x7f;
 		break;
 	case CLD_TRAPPED:
 		info.si_status = tsk->exit_code & 0x7f;
 		break;
 	default:
 		BUG();
 	}

	sighand = parent->sighand;
	spin_lock_irqsave(&sighand->siglock, flags);
	if (sighand->action[SIGCHLD-1].sa.sa_handler != SIG_IGN &&
	    !(sighand->action[SIGCHLD-1].sa.sa_flags & SA_NOCLDSTOP))
		__group_send_sig_info(SIGCHLD, &info, parent);
	/*
	 * Even if SIGCHLD is not generated, we must wake up wait4 calls.
	 */
	__wake_up_parent(tsk, parent);
	spin_unlock_irqrestore(&sighand->siglock, flags);
}

static inline bool may_ptrace_stop(void)
{
	if (!likely(current->ptrace))
		return false;
	/*
	 * Are we in the middle of do_coredump?
	 * If so and our tracer is also part of the coredump stopping
	 * is a deadlock situation, and pointless because our tracer
	 * is dead so don't allow us to stop.
	 * If SIGKILL was already sent before the caller unlocked
	 * ->siglock we must see ->core_state != NULL. Otherwise it
	 * is safe to enter schedule().
	 *
	 * This is almost outdated, a task with the pending SIGKILL can't
	 * block in TASK_TRACED. But PTRACE_EVENT_EXIT can be reported
	 * after SIGKILL was already dequeued.
	 */
	if (unlikely(current->mm->core_state) &&
	    unlikely(current->mm == current->parent->mm))
		return false;

	return true;
}

/*
 * Return non-zero if there is a SIGKILL that should be waking us up.
 * Called with the siglock held.
 */
static bool sigkill_pending(struct task_struct *tsk)
{
	return sigismember(&tsk->pending.signal, SIGKILL) ||
	       sigismember(&tsk->signal->shared_pending.signal, SIGKILL);
}

/*
 * This must be called with current->sighand->siglock held.
 *
 * This should be the path for all ptrace stops.
 * We always set current->last_siginfo while stopped here.
 * That makes it a way to test a stopped process for
 * being ptrace-stopped vs being job-control-stopped.
 *
 * If we actually decide not to stop at all because the tracer
 * is gone, we keep current->exit_code unless clear_code.
 */
static void ptrace_stop(int exit_code, int why, int clear_code, kernel_siginfo_t *info)
	__releases(&current->sighand->siglock)
	__acquires(&current->sighand->siglock)
{
	bool gstop_done = false;

	if (arch_ptrace_stop_needed(exit_code, info)) {
		/*
		 * The arch code has something special to do before a
		 * ptrace stop.  This is allowed to block, e.g. for faults
		 * on user stack pages.  We can't keep the siglock while
		 * calling arch_ptrace_stop, so we must release it now.
		 * To preserve proper semantics, we must do this before
		 * any signal bookkeeping like checking group_stop_count.
		 * Meanwhile, a SIGKILL could come in before we retake the
		 * siglock.  That must prevent us from sleeping in TASK_TRACED.
		 * So after regaining the lock, we must check for SIGKILL.
		 */
		spin_unlock_irq(&current->sighand->siglock);
		arch_ptrace_stop(exit_code, info);
		spin_lock_irq(&current->sighand->siglock);
		if (sigkill_pending(current))
			return;
	}

	set_special_state(TASK_TRACED);

	/*
	 * We're committing to trapping.  TRACED should be visible before
	 * TRAPPING is cleared; otherwise, the tracer might fail do_wait().
	 * Also, transition to TRACED and updates to ->jobctl should be
	 * atomic with respect to siglock and should be done after the arch
	 * hook as siglock is released and regrabbed across it.
	 *
	 *     TRACER				    TRACEE
	 *
	 *     ptrace_attach()
	 * [L]   wait_on_bit(JOBCTL_TRAPPING)	[S] set_special_state(TRACED)
	 *     do_wait()
	 *       set_current_state()                smp_wmb();
	 *       ptrace_do_wait()
	 *         wait_task_stopped()
	 *           task_stopped_code()
	 * [L]         task_is_traced()		[S] task_clear_jobctl_trapping();
	 */
	smp_wmb();

	current->last_siginfo = info;
	current->exit_code = exit_code;

	/*
	 * If @why is CLD_STOPPED, we're trapping to participate in a group
	 * stop.  Do the bookkeeping.  Note that if SIGCONT was delievered
	 * across siglock relocks since INTERRUPT was scheduled, PENDING
	 * could be clear now.  We act as if SIGCONT is received after
	 * TASK_TRACED is entered - ignore it.
	 */
	if (why == CLD_STOPPED && (current->jobctl & JOBCTL_STOP_PENDING))
		gstop_done = task_participate_group_stop(current);

	/* any trap clears pending STOP trap, STOP trap clears NOTIFY */
	task_clear_jobctl_pending(current, JOBCTL_TRAP_STOP);
	if (info && info->si_code >> 8 == PTRACE_EVENT_STOP)
		task_clear_jobctl_pending(current, JOBCTL_TRAP_NOTIFY);

	/* entering a trap, clear TRAPPING */
	task_clear_jobctl_trapping(current);

	spin_unlock_irq(&current->sighand->siglock);
	read_lock(&tasklist_lock);
	if (may_ptrace_stop()) {
		/*
		 * Notify parents of the stop.
		 *
		 * While ptraced, there are two parents - the ptracer and
		 * the real_parent of the group_leader.  The ptracer should
		 * know about every stop while the real parent is only
		 * interested in the completion of group stop.  The states
		 * for the two don't interact with each other.  Notify
		 * separately unless they're gonna be duplicates.
		 */
		do_notify_parent_cldstop(current, true, why);
		if (gstop_done && ptrace_reparented(current))
			do_notify_parent_cldstop(current, false, why);

		/*
		 * Don't want to allow preemption here, because
		 * sys_ptrace() needs this task to be inactive.
		 *
		 * XXX: implement read_unlock_no_resched().
		 */
		preempt_disable();
		read_unlock(&tasklist_lock);
		cgroup_enter_frozen();
		preempt_enable_no_resched();
		freezable_schedule();
		cgroup_leave_frozen(true);
	} else {
		/*
		 * By the time we got the lock, our tracer went away.
		 * Don't drop the lock yet, another tracer may come.
		 *
		 * If @gstop_done, the ptracer went away between group stop
		 * completion and here.  During detach, it would have set
		 * JOBCTL_STOP_PENDING on us and we'll re-enter
		 * TASK_STOPPED in do_signal_stop() on return, so notifying
		 * the real parent of the group stop completion is enough.
		 */
		if (gstop_done)
			do_notify_parent_cldstop(current, false, why);

		/* tasklist protects us from ptrace_freeze_traced() */
		__set_current_state(TASK_RUNNING);
		if (clear_code)
			current->exit_code = 0;
		read_unlock(&tasklist_lock);
	}

	/*
	 * We are back.  Now reacquire the siglock before touching
	 * last_siginfo, so that we are sure to have synchronized with
	 * any signal-sending on another CPU that wants to examine it.
	 */
	spin_lock_irq(&current->sighand->siglock);
	current->last_siginfo = NULL;

	/* LISTENING can be set only during STOP traps, clear it */
	current->jobctl &= ~JOBCTL_LISTENING;

	/*
	 * Queued signals ignored us while we were stopped for tracing.
	 * So check for any that we should take before resuming user mode.
	 * This sets TIF_SIGPENDING, but never clears it.
	 */
	recalc_sigpending_tsk(current);
}

static void ptrace_do_notify(int signr, int exit_code, int why)
{
	kernel_siginfo_t info;

	clear_siginfo(&info);
	info.si_signo = signr;
	info.si_code = exit_code;
	info.si_pid = task_pid_vnr(current);
	info.si_uid = from_kuid_munged(current_user_ns(), current_uid());

	/* Let the debugger run.  */
	ptrace_stop(exit_code, why, 1, &info);
}

/**
 * ptrace_notify - 向调试器发送ptrace通知
 * @exit_code: 退出码或停止原因
 *
 * 当被调试进程遇到断点、单步执行等调试事件时，
 * 向调试器进程发送SIGTRAP信号进行通知
 */
void ptrace_notify(int exit_code)
{
	BUG_ON((exit_code & (0x7f | ~0xffff)) != SIGTRAP);
	if (unlikely(current->task_works))
		task_work_run();

	spin_lock_irq(&current->sighand->siglock);
	ptrace_do_notify(SIGTRAP, exit_code, CLD_TRAPPED);
	spin_unlock_irq(&current->sighand->siglock);
}

/**
 * do_signal_stop - handle group stop for SIGSTOP and other stop signals
 * @signr: signr causing group stop if initiating
 *
 * If %JOBCTL_STOP_PENDING is not set yet, initiate group stop with @signr
 * and participate in it.  If already set, participate in the existing
 * group stop.  If participated in a group stop (and thus slept), %true is
 * returned with siglock released.
 *
 * If ptraced, this function doesn't handle stop itself.  Instead,
 * %JOBCTL_TRAP_STOP is scheduled and %false is returned with siglock
 * untouched.  The caller must ensure that INTERRUPT trap handling takes
 * places afterwards.
 *
 * CONTEXT:
 * Must be called with @current->sighand->siglock held, which is released
 * on %true return.
 *
 * RETURNS:
 * %false if group stop is already cancelled or ptrace trap is scheduled.
 * %true if participated in group stop.
 */
/**
 * do_signal_stop - 处理SIGSTOP信号
 * @signr: 信号编号
 * 返回值：如果进程应该停止返回true，否则返回false
 */
static bool do_signal_stop(int signr)
	__releases(&current->sighand->siglock)
{
	struct signal_struct *sig = current->signal;

	if (!(current->jobctl & JOBCTL_STOP_PENDING)) {
		unsigned long gstop = JOBCTL_STOP_PENDING | JOBCTL_STOP_CONSUME;
		struct task_struct *t;

		/* signr will be recorded in task->jobctl for retries */
		WARN_ON_ONCE(signr & ~JOBCTL_STOP_SIGMASK);

		if (!likely(current->jobctl & JOBCTL_STOP_DEQUEUED) ||
		    unlikely(signal_group_exit(sig)))
			return false;
		/*
		 * There is no group stop already in progress.  We must
		 * initiate one now.
		 *
		 * While ptraced, a task may be resumed while group stop is
		 * still in effect and then receive a stop signal and
		 * initiate another group stop.  This deviates from the
		 * usual behavior as two consecutive stop signals can't
		 * cause two group stops when !ptraced.  That is why we
		 * also check !task_is_stopped(t) below.
		 *
		 * The condition can be distinguished by testing whether
		 * SIGNAL_STOP_STOPPED is already set.  Don't generate
		 * group_exit_code in such case.
		 *
		 * This is not necessary for SIGNAL_STOP_CONTINUED because
		 * an intervening stop signal is required to cause two
		 * continued events regardless of ptrace.
		 */
		if (!(sig->flags & SIGNAL_STOP_STOPPED))
			sig->group_exit_code = signr;

		sig->group_stop_count = 0;

		if (task_set_jobctl_pending(current, signr | gstop))
			sig->group_stop_count++;

		t = current;
		while_each_thread(current, t) {
			/*
			 * Setting state to TASK_STOPPED for a group
			 * stop is always done with the siglock held,
			 * so this check has no races.
			 */
			if (!task_is_stopped(t) &&
			    task_set_jobctl_pending(t, signr | gstop)) {
				sig->group_stop_count++;
				if (likely(!(t->ptrace & PT_SEIZED)))
					signal_wake_up(t, 0);
				else
					ptrace_trap_notify(t);
			}
		}
	}

	if (likely(!current->ptrace)) {
		int notify = 0;

		/*
		 * If there are no other threads in the group, or if there
		 * is a group stop in progress and we are the last to stop,
		 * report to the parent.
		 */
		if (task_participate_group_stop(current))
			notify = CLD_STOPPED;

		set_special_state(TASK_STOPPED);
		spin_unlock_irq(&current->sighand->siglock);

		/*
		 * Notify the parent of the group stop completion.  Because
		 * we're not holding either the siglock or tasklist_lock
		 * here, ptracer may attach inbetween; however, this is for
		 * group stop and should always be delivered to the real
		 * parent of the group leader.  The new ptracer will get
		 * its notification when this task transitions into
		 * TASK_TRACED.
		 */
		if (notify) {
			read_lock(&tasklist_lock);
			do_notify_parent_cldstop(current, false, notify);
			read_unlock(&tasklist_lock);
		}

		/* Now we don't run again until woken by SIGCONT or SIGKILL */
		cgroup_enter_frozen();
		freezable_schedule();
		return true;
	} else {
		/*
		 * While ptraced, group stop is handled by STOP trap.
		 * Schedule it and let the caller deal with it.
		 */
		task_set_jobctl_pending(current, JOBCTL_TRAP_STOP);
		return false;
	}
}

/**
 * do_jobctl_trap - take care of ptrace jobctl traps
 *
 * When PT_SEIZED, it's used for both group stop and explicit
 * SEIZE/INTERRUPT traps.  Both generate PTRACE_EVENT_STOP trap with
 * accompanying siginfo.  If stopped, lower eight bits of exit_code contain
 * the stop signal; otherwise, %SIGTRAP.
 *
 * When !PT_SEIZED, it's used only for group stop trap with stop signal
 * number as exit_code and no siginfo.
 *
 * CONTEXT:
 * Must be called with @current->sighand->siglock held, which may be
 * released and re-acquired before returning with intervening sleep.
 */
static void do_jobctl_trap(void)
{
	struct signal_struct *signal = current->signal;
	int signr = current->jobctl & JOBCTL_STOP_SIGMASK;

	if (current->ptrace & PT_SEIZED) {
		if (!signal->group_stop_count &&
		    !(signal->flags & SIGNAL_STOP_STOPPED))
			signr = SIGTRAP;
		WARN_ON_ONCE(!signr);
		ptrace_do_notify(signr, signr | (PTRACE_EVENT_STOP << 8),
				 CLD_STOPPED);
	} else {
		WARN_ON_ONCE(!signr);
		ptrace_stop(signr, CLD_STOPPED, 0, NULL);
		current->exit_code = 0;
	}
}

/**
 * do_freezer_trap - handle the freezer jobctl trap
 *
 * Puts the task into frozen state, if only the task is not about to quit.
 * In this case it drops JOBCTL_TRAP_FREEZE.
 *
 * CONTEXT:
 * Must be called with @current->sighand->siglock held,
 * which is always released before returning.
 */
static void do_freezer_trap(void)
	__releases(&current->sighand->siglock)
{
	/*
	 * If there are other trap bits pending except JOBCTL_TRAP_FREEZE,
	 * let's make another loop to give it a chance to be handled.
	 * In any case, we'll return back.
	 */
	if ((current->jobctl & (JOBCTL_PENDING_MASK | JOBCTL_TRAP_FREEZE)) !=
	     JOBCTL_TRAP_FREEZE) {
		spin_unlock_irq(&current->sighand->siglock);
		return;
	}

	/*
	 * Now we're sure that there is no pending fatal signal and no
	 * pending traps. Clear TIF_SIGPENDING to not get out of schedule()
	 * immediately (if there is a non-fatal signal pending), and
	 * put the task into sleep.
	 */
	__set_current_state(TASK_INTERRUPTIBLE);
	clear_thread_flag(TIF_SIGPENDING);
	spin_unlock_irq(&current->sighand->siglock);
	cgroup_enter_frozen();
	freezable_schedule();
}

static int ptrace_signal(int signr, kernel_siginfo_t *info)
{
	/*
	 * We do not check sig_kernel_stop(signr) but set this marker
	 * unconditionally because we do not know whether debugger will
	 * change signr. This flag has no meaning unless we are going
	 * to stop after return from ptrace_stop(). In this case it will
	 * be checked in do_signal_stop(), we should only stop if it was
	 * not cleared by SIGCONT while we were sleeping. See also the
	 * comment in dequeue_signal().
	 */
	current->jobctl |= JOBCTL_STOP_DEQUEUED;
	ptrace_stop(signr, CLD_TRAPPED, 0, info);

	/* We're back.  Did the debugger cancel the sig?  */
	signr = current->exit_code;
	if (signr == 0)
		return signr;

	current->exit_code = 0;

	/*
	 * Update the siginfo structure if the signal has
	 * changed.  If the debugger wanted something
	 * specific in the siginfo structure then it should
	 * have updated *info via PTRACE_SETSIGINFO.
	 */
	if (signr != info->si_signo) {
		clear_siginfo(info);
		info->si_signo = signr;
		info->si_errno = 0;
		info->si_code = SI_USER;
		rcu_read_lock();
		info->si_pid = task_pid_vnr(current->parent);
		info->si_uid = from_kuid_munged(current_user_ns(),
						task_uid(current->parent));
		rcu_read_unlock();
	}

	/* If the (new) signal is now blocked, requeue it.  */
	if (sigismember(&current->blocked, signr)) {
		send_signal(signr, info, current, PIDTYPE_PID);
		signr = 0;
	}

	return signr;
}

/**
 * get_signal - 获取待处理信号
 * @ksig: 内核信号结构，用于存储获取的信号信息
 * 返回值：如果获取到信号返回true，否则返回false
 */
/**
 * get_signal - 获取并处理挂起的信号
 * @ksig: 存储信号信息的结构体
 *
 * 这是信号处理的核心函数，从信号队列中取出信号并决定如何处理：
 * - 执行信号处理器
 * - 执行默认动作(终止、停止、忽略等)
 * - 处理作业控制信号
 *
 * 返回值: 如果需要处理信号返回true，否则返回false
 */
bool get_signal(struct ksignal *ksig)
{
	struct sighand_struct *sighand = current->sighand;
	struct signal_struct *signal = current->signal;
	int signr;

	if (unlikely(uprobe_deny_signal()))
		return false;

	/*
	 * Do this once, we can't return to user-mode if freezing() == T.
	 * do_signal_stop() and ptrace_stop() do freezable_schedule() and
	 * thus do not need another check after return.
	 */
	try_to_freeze();

relock:
	spin_lock_irq(&sighand->siglock);
	/*
	 * Make sure we can safely read ->jobctl() in task_work add. As Oleg
	 * states:
	 *
	 * It pairs with mb (implied by cmpxchg) before READ_ONCE. So we
	 * roughly have
	 *
	 *	task_work_add:				get_signal:
	 *	STORE(task->task_works, new_work);	STORE(task->jobctl);
	 *	mb();					mb();
	 *	LOAD(task->jobctl);			LOAD(task->task_works);
	 *
	 * and we can rely on STORE-MB-LOAD [ in task_work_add].
	 */
	smp_store_mb(current->jobctl, current->jobctl & ~JOBCTL_TASK_WORK);
	if (unlikely(current->task_works)) {
		spin_unlock_irq(&sighand->siglock);
		task_work_run();
		goto relock;
	}

	/*
	 * Every stopped thread goes here after wakeup. Check to see if
	 * we should notify the parent, prepare_signal(SIGCONT) encodes
	 * the CLD_ si_code into SIGNAL_CLD_MASK bits.
	 */
	if (unlikely(signal->flags & SIGNAL_CLD_MASK)) {
		int why;

		if (signal->flags & SIGNAL_CLD_CONTINUED)
			why = CLD_CONTINUED;
		else
			why = CLD_STOPPED;

		signal->flags &= ~SIGNAL_CLD_MASK;

		spin_unlock_irq(&sighand->siglock);

		/*
		 * Notify the parent that we're continuing.  This event is
		 * always per-process and doesn't make whole lot of sense
		 * for ptracers, who shouldn't consume the state via
		 * wait(2) either, but, for backward compatibility, notify
		 * the ptracer of the group leader too unless it's gonna be
		 * a duplicate.
		 */
		read_lock(&tasklist_lock);
		do_notify_parent_cldstop(current, false, why);

		if (ptrace_reparented(current->group_leader))
			do_notify_parent_cldstop(current->group_leader,
						true, why);
		read_unlock(&tasklist_lock);

		goto relock;
	}

	/* Has this task already been marked for death? */
	if (signal_group_exit(signal)) {
		ksig->info.si_signo = signr = SIGKILL;
		sigdelset(&current->pending.signal, SIGKILL);
		trace_signal_deliver(SIGKILL, SEND_SIG_NOINFO,
				&sighand->action[SIGKILL - 1]);
		recalc_sigpending();
		goto fatal;
	}

	for (;;) {
		struct k_sigaction *ka;

		if (unlikely(current->jobctl & JOBCTL_STOP_PENDING) &&
		    do_signal_stop(0))
			goto relock;

		if (unlikely(current->jobctl &
			     (JOBCTL_TRAP_MASK | JOBCTL_TRAP_FREEZE))) {
			if (current->jobctl & JOBCTL_TRAP_MASK) {
				do_jobctl_trap();
				spin_unlock_irq(&sighand->siglock);
			} else if (current->jobctl & JOBCTL_TRAP_FREEZE)
				do_freezer_trap();

			goto relock;
		}

		/*
		 * If the task is leaving the frozen state, let's update
		 * cgroup counters and reset the frozen bit.
		 */
		if (unlikely(cgroup_task_frozen(current))) {
			spin_unlock_irq(&sighand->siglock);
			cgroup_leave_frozen(false);
			goto relock;
		}

		/*
		 * Signals generated by the execution of an instruction
		 * need to be delivered before any other pending signals
		 * so that the instruction pointer in the signal stack
		 * frame points to the faulting instruction.
		 */
		signr = dequeue_synchronous_signal(&ksig->info);  // 优先处理同步信号(如SIGSEGV、SIGFPE等)
		if (!signr)
			signr = dequeue_signal(current, &current->blocked, &ksig->info);  // 从信号队列取出下一个未被阻塞的信号

		if (!signr)
			break; /* will return 0 */  // 没有信号需要处理，退出循环

		if (unlikely(current->ptrace) && signr != SIGKILL) {  // 被调试进程的信号需要特殊处理
			signr = ptrace_signal(signr, &ksig->info);  // 通知调试器，可能修改或阻止信号
			if (!signr)
				continue;  // 调试器阻止了信号，继续处理下一个
		}

		ka = &sighand->action[signr-1];  // 获取该信号的处理动作结构体

		/* Trace actually delivered signals. */
		trace_signal_deliver(signr, &ksig->info, ka);  // 追踪信号传递事件

		if (ka->sa.sa_handler == SIG_IGN) /* Do nothing.  */  // 信号被设置为忽略
			continue;
		if (ka->sa.sa_handler != SIG_DFL) {  // 有自定义信号处理器
			/* Run the handler.  */
			ksig->ka = *ka;  // 保存信号动作，准备执行用户处理器

			if (ka->sa.sa_flags & SA_ONESHOT)  // SA_ONESHOT标志：执行一次后恢复默认
				ka->sa.sa_handler = SIG_DFL;

			break; /* will return non-zero "signr" value */  // 跳出循环，返回给用户态执行处理器
		}

		/*
		 * Now we are doing the default action for this signal.
		 */
		if (sig_kernel_ignore(signr)) /* Default is nothing. */
			continue;

		/*
		 * Global init gets no signals it doesn't want.
		 * Container-init gets no signals it doesn't want from same
		 * container.
		 *
		 * Note that if global/container-init sees a sig_kernel_only()
		 * signal here, the signal must have been generated internally
		 * or must have come from an ancestor namespace. In either
		 * case, the signal cannot be dropped.
		 */
		if (unlikely(signal->flags & SIGNAL_UNKILLABLE) &&
				!sig_kernel_only(signr))
			continue;

		if (sig_kernel_stop(signr)) {
			/*
			 * The default action is to stop all threads in
			 * the thread group.  The job control signals
			 * do nothing in an orphaned pgrp, but SIGSTOP
			 * always works.  Note that siglock needs to be
			 * dropped during the call to is_orphaned_pgrp()
			 * because of lock ordering with tasklist_lock.
			 * This allows an intervening SIGCONT to be posted.
			 * We need to check for that and bail out if necessary.
			 */
			if (signr != SIGSTOP) {
				spin_unlock_irq(&sighand->siglock);

				/* signals can be posted during this window */

				if (is_current_pgrp_orphaned())
					goto relock;

				spin_lock_irq(&sighand->siglock);
			}

			if (likely(do_signal_stop(ksig->info.si_signo))) {
				/* It released the siglock.  */
				goto relock;
			}

			/*
			 * We didn't actually stop, due to a race
			 * with SIGCONT or something like that.
			 */
			continue;
		}

	fatal:
		spin_unlock_irq(&sighand->siglock);
		if (unlikely(cgroup_task_frozen(current)))
			cgroup_leave_frozen(true);

		/*
		 * Anything else is fatal, maybe with a core dump.
		 */
		current->flags |= PF_SIGNALED;

		if (sig_kernel_coredump(signr)) {
			if (print_fatal_signals)
				print_fatal_signal(ksig->info.si_signo);
			proc_coredump_connector(current);
			/*
			 * If it was able to dump core, this kills all
			 * other threads in the group and synchronizes with
			 * their demise.  If we lost the race with another
			 * thread getting here, it set group_exit_code
			 * first and our do_group_exit call below will use
			 * that value and ignore the one we pass it.
			 */
			do_coredump(&ksig->info);
		}

		/*
		 * Death signals, no core dump.
		 */
		do_group_exit(ksig->info.si_signo);
		/* NOTREACHED */
	}
	spin_unlock_irq(&sighand->siglock);

	ksig->sig = signr;
	return ksig->sig > 0;
}

/**
 * signal_delivered - 
 * @ksig:		kernel signal struct
 * @stepping:		nonzero if debugger single-step or block-step in use
 *
 * This function should be called when a signal has successfully been
 * delivered. It updates the blocked signals accordingly (@ksig->ka.sa.sa_mask
 * is always blocked, and the signal itself is blocked unless %SA_NODEFER
 * is set in @ksig->ka.sa.sa_flags.  Tracing is notified.
 */
static void signal_delivered(struct ksignal *ksig, int stepping)
{
	sigset_t blocked;

	/* A signal was successfully delivered, and the
	   saved sigmask was stored on the signal frame,
	   and will be restored by sigreturn.  So we can
	   simply clear the restore sigmask flag.  */
	clear_restore_sigmask();

	sigorsets(&blocked, &current->blocked, &ksig->ka.sa.sa_mask);
	if (!(ksig->ka.sa.sa_flags & SA_NODEFER))
		sigaddset(&blocked, ksig->sig);
	set_current_blocked(&blocked);
	tracehook_signal_handler(stepping);
}

/**
 * signal_setup_done - 信号处理器设置完成
 * @failed: 设置是否失败
 * @ksig: 信号信息结构体
 * @stepping: 是否在单步调试模式
 *
 * 在信号处理器设置完成后进行清理工作，
 * 如果设置失败则发送SIGSEGV信号
 */
void signal_setup_done(int failed, struct ksignal *ksig, int stepping)
{
	if (failed)
		force_sigsegv(ksig->sig);
	else
		signal_delivered(ksig, stepping);
}

/*
 * It could be that complete_signal() picked us to notify about the
 * group-wide signal. Other threads should be notified now to take
 * the shared signals in @which since we will not.
 */
static void retarget_shared_pending(struct task_struct *tsk, sigset_t *which)
{
	sigset_t retarget;
	struct task_struct *t;

	sigandsets(&retarget, &tsk->signal->shared_pending.signal, which);
	if (sigisemptyset(&retarget))
		return;

	t = tsk;
	while_each_thread(tsk, t) {
		if (t->flags & PF_EXITING)
			continue;

		if (!has_pending_signals(&retarget, &t->blocked))
			continue;
		/* Remove the signals this thread can handle. */
		sigandsets(&retarget, &retarget, &t->blocked);

		if (!signal_pending(t))
			signal_wake_up(t, 0);

		if (sigisemptyset(&retarget))
			break;
	}
}

/**
 * exit_signals - 进程退出时的信号处理
 * @tsk: 正在退出的进程
 *
 * 进程退出时进行信号相关的清理工作：
 * - 清空信号队列
 * - 处理组停止状态
 * - 通知其他进程此进程即将退出
 */
void exit_signals(struct task_struct *tsk)
{
	int group_stop = 0;
	sigset_t unblocked;

	/*
	 * @tsk is about to have PF_EXITING set - lock out users which
	 * expect stable threadgroup.
	 */
	cgroup_threadgroup_change_begin(tsk);

	if (thread_group_empty(tsk) || signal_group_exit(tsk->signal)) {
		tsk->flags |= PF_EXITING;
		cgroup_threadgroup_change_end(tsk);
		return;
	}

	spin_lock_irq(&tsk->sighand->siglock);
	/*
	 * From now this task is not visible for group-wide signals,
	 * see wants_signal(), do_signal_stop().
	 */
	tsk->flags |= PF_EXITING;

	cgroup_threadgroup_change_end(tsk);

	if (!signal_pending(tsk))
		goto out;

	unblocked = tsk->blocked;
	signotset(&unblocked);
	retarget_shared_pending(tsk, &unblocked);

	if (unlikely(tsk->jobctl & JOBCTL_STOP_PENDING) &&
	    task_participate_group_stop(tsk))
		group_stop = CLD_STOPPED;
out:
	spin_unlock_irq(&tsk->sighand->siglock);

	/*
	 * If group stop has completed, deliver the notification.  This
	 * should always go to the real parent of the group leader.
	 */
	if (unlikely(group_stop)) {
		read_lock(&tasklist_lock);
		do_notify_parent_cldstop(tsk, false, group_stop);
		read_unlock(&tasklist_lock);
	}
}

/*
 * System call entry points.
 */

/**
 *  sys_restart_syscall - restart a system call
 */
SYSCALL_DEFINE0(restart_syscall)
{
	struct restart_block *restart = &current->restart_block;
	return restart->fn(restart);
}

/**
 * do_no_restart_syscall - 不重启系统调用的处理函数
 * @param: 重启块参数
 *
 * 当系统调用被信号中断且不应该重启时调用此函数，
 * 直接返回-EINTR错误码
 * 返回值: -EINTR表示系统调用被中断
 */
long do_no_restart_syscall(struct restart_block *param)
{
	return -EINTR;
}

static void __set_task_blocked(struct task_struct *tsk, const sigset_t *newset)
{
	if (signal_pending(tsk) && !thread_group_empty(tsk)) {
		sigset_t newblocked;
		/* A set of now blocked but previously unblocked signals. */
		sigandnsets(&newblocked, newset, &current->blocked);
		retarget_shared_pending(tsk, &newblocked);
	}
	tsk->blocked = *newset;
	recalc_sigpending();
}

/**
 * set_current_blocked - change current->blocked mask
 * @newset: new mask
 *
 * It is wrong to change ->blocked directly, this helper should be used
 * to ensure the process can't miss a shared signal we are going to block.
 */
/**
 * set_current_blocked - 设置当前进程的信号阻塞掩码
 * @newset: 新的信号阻塞掩码
 *
 * 设置当前进程的信号阻塞掩码，阻塞指定的信号
 * 同时重新计算信号挂起状态
 */
void set_current_blocked(sigset_t *newset)
{
	sigdelsetmask(newset, sigmask(SIGKILL) | sigmask(SIGSTOP));
	__set_current_blocked(newset);
}

/**
 * __set_current_blocked - 内部函数：设置当前进程的信号阻塞掩码
 * @newset: 新的信号阻塞掩码
 *
 * 设置当前进程的信号阻塞掩码的内部实现，
 * 不进行额外的检查和处理
 */
void __set_current_blocked(const sigset_t *newset)
{
	struct task_struct *tsk = current;

	/*
	 * In case the signal mask hasn't changed, there is nothing we need
	 * to do. The current->blocked shouldn't be modified by other task.
	 */
	if (sigequalsets(&tsk->blocked, newset))
		return;

	spin_lock_irq(&tsk->sighand->siglock);
	__set_task_blocked(tsk, newset);
	spin_unlock_irq(&tsk->sighand->siglock);
}

/*
 * This is also useful for kernel threads that want to temporarily
 * (or permanently) block certain signals.
 *
 * NOTE! Unlike the user-mode sys_sigprocmask(), the kernel
 * interface happily blocks "unblockable" signals like SIGKILL
 * and friends.
 */
/**
 * sigprocmask - 改变进程的信号掩码
 * @how: 操作方式(SIG_BLOCK/SIG_UNBLOCK/SIG_SETMASK)
 * @set: 新的信号集合
 * @oldset: 保存旧的信号掩码
 *
 * 实现sigprocmask()系统调用，修改进程的信号阻塞掩码
 * 返回值: 成功返回0，失败返回负值错误码
 */
int sigprocmask(int how, sigset_t *set, sigset_t *oldset)
{
	struct task_struct *tsk = current;
	sigset_t newset;

	/* Lockless, only current can change ->blocked, never from irq */
	if (oldset)
		*oldset = tsk->blocked;

	switch (how) {
	case SIG_BLOCK:
		sigorsets(&newset, &tsk->blocked, set);  // 阻塞新信号：newset = blocked ∪ set
		break;
	case SIG_UNBLOCK:
		sigandnsets(&newset, &tsk->blocked, set);  // 解除阻塞：newset = blocked ∩ ~set
		break;
	case SIG_SETMASK:
		newset = *set;  // 直接设置新的信号掩码
		break;
	default:
		return -EINVAL;
	}

	__set_current_blocked(&newset);
	return 0;
}
EXPORT_SYMBOL(sigprocmask);

/*
 * The api helps set app-provided sigmasks.
 *
 * This is useful for syscalls such as ppoll, pselect, io_pgetevents and
 * epoll_pwait where a new sigmask is passed from userland for the syscalls.
 *
 * Note that it does set_restore_sigmask() in advance, so it must be always
 * paired with restore_saved_sigmask_unless() before return from syscall.
 */
/**
 * set_user_sigmask - 设置用户空间的信号掩码
 * @umask: 用户空间信号掩码指针
 * @sigsetsize: 信号集合大小
 *
 * 从用户空间复制信号掩码并设置为当前进程的阻塞掩码
 * 用于ppoll、pselect等系统调用
 * 返回值: 成功返回0，失败返回负值错误码
 */
int set_user_sigmask(const sigset_t __user *umask, size_t sigsetsize)
{
	sigset_t kmask;

	if (!umask)
		return 0;
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;
	if (copy_from_user(&kmask, umask, sizeof(sigset_t)))
		return -EFAULT;

	set_restore_sigmask();
	current->saved_sigmask = current->blocked;
	set_current_blocked(&kmask);

	return 0;
}

#ifdef CONFIG_COMPAT
/**
 * set_compat_user_sigmask - 设置兼容模式的用户信号掩码
 * @umask: 用户空间兼容信号掩码指针
 * @sigsetsize: 信号集合大小
 *
 * 32位程序在64位内核上运行时，从用户空间复制兼容格式的信号掩码
 * 用于compat_ppoll、compat_pselect等兼容系统调用
 * 返回值: 成功返回0，失败返回负值错误码
 */
int set_compat_user_sigmask(const compat_sigset_t __user *umask,
			    size_t sigsetsize)
{
	sigset_t kmask;

	if (!umask)
		return 0;
	if (sigsetsize != sizeof(compat_sigset_t))
		return -EINVAL;
	if (get_compat_sigset(&kmask, umask))
		return -EFAULT;

	set_restore_sigmask();
	current->saved_sigmask = current->blocked;
	set_current_blocked(&kmask);

	return 0;
}
#endif

/**
 *  sys_rt_sigprocmask - change the list of currently blocked signals
 *  @how: whether to add, remove, or set signals
 *  @nset: stores pending signals
 *  @oset: previous value of signal mask if non-null
 *  @sigsetsize: size of sigset_t type
 */
SYSCALL_DEFINE4(rt_sigprocmask, int, how, sigset_t __user *, nset,
		sigset_t __user *, oset, size_t, sigsetsize)
{
	sigset_t old_set, new_set;
	int error;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	old_set = current->blocked;

	if (nset) {
		if (copy_from_user(&new_set, nset, sizeof(sigset_t)))
			return -EFAULT;
		sigdelsetmask(&new_set, sigmask(SIGKILL)|sigmask(SIGSTOP));

		error = sigprocmask(how, &new_set, NULL);
		if (error)
			return error;
	}

	if (oset) {
		if (copy_to_user(oset, &old_set, sizeof(sigset_t)))
			return -EFAULT;
	}

	return 0;
}

#ifdef CONFIG_COMPAT
COMPAT_SYSCALL_DEFINE4(rt_sigprocmask, int, how, compat_sigset_t __user *, nset,
		compat_sigset_t __user *, oset, compat_size_t, sigsetsize)
{
	sigset_t old_set = current->blocked;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if (nset) {
		sigset_t new_set;
		int error;
		if (get_compat_sigset(&new_set, nset))
			return -EFAULT;
		sigdelsetmask(&new_set, sigmask(SIGKILL)|sigmask(SIGSTOP));

		error = sigprocmask(how, &new_set, NULL);
		if (error)
			return error;
	}
	return oset ? put_compat_sigset(oset, &old_set, sizeof(*oset)) : 0;
}
#endif

static void do_sigpending(sigset_t *set)
{
	spin_lock_irq(&current->sighand->siglock);
	sigorsets(set, &current->pending.signal,
		  &current->signal->shared_pending.signal);
	spin_unlock_irq(&current->sighand->siglock);

	/* Outside the lock because only this thread touches it.  */
	sigandsets(set, &current->blocked, set);
}

/**
 *  sys_rt_sigpending - examine a pending signal that has been raised
 *			while blocked
 *  @uset: stores pending signals
 *  @sigsetsize: size of sigset_t type or larger
 */
SYSCALL_DEFINE2(rt_sigpending, sigset_t __user *, uset, size_t, sigsetsize)
{
	sigset_t set;

	if (sigsetsize > sizeof(*uset))
		return -EINVAL;

	do_sigpending(&set);

	if (copy_to_user(uset, &set, sigsetsize))
		return -EFAULT;

	return 0;
}

#ifdef CONFIG_COMPAT
COMPAT_SYSCALL_DEFINE2(rt_sigpending, compat_sigset_t __user *, uset,
		compat_size_t, sigsetsize)
{
	sigset_t set;

	if (sigsetsize > sizeof(*uset))
		return -EINVAL;

	do_sigpending(&set);

	return put_compat_sigset(uset, &set, sigsetsize);
}
#endif

static const struct {
	unsigned char limit, layout;
} sig_sicodes[] = {
	[SIGILL]  = { NSIGILL,  SIL_FAULT },
	[SIGFPE]  = { NSIGFPE,  SIL_FAULT },
	[SIGSEGV] = { NSIGSEGV, SIL_FAULT },
	[SIGBUS]  = { NSIGBUS,  SIL_FAULT },
	[SIGTRAP] = { NSIGTRAP, SIL_FAULT },
#if defined(SIGEMT)
	[SIGEMT]  = { NSIGEMT,  SIL_FAULT },
#endif
	[SIGCHLD] = { NSIGCHLD, SIL_CHLD },
	[SIGPOLL] = { NSIGPOLL, SIL_POLL },
	[SIGSYS]  = { NSIGSYS,  SIL_SYS },
};

static bool known_siginfo_layout(unsigned sig, int si_code)
{
	if (si_code == SI_KERNEL)
		return true;
	else if ((si_code > SI_USER)) {
		if (sig_specific_sicodes(sig)) {
			if (si_code <= sig_sicodes[sig].limit)
				return true;
		}
		else if (si_code <= NSIGPOLL)
			return true;
	}
	else if (si_code >= SI_DETHREAD)
		return true;
	else if (si_code == SI_ASYNCNL)
		return true;
	return false;
}

enum siginfo_layout siginfo_layout(unsigned sig, int si_code)
{
	enum siginfo_layout layout = SIL_KILL;
	if ((si_code > SI_USER) && (si_code < SI_KERNEL)) {
		if ((sig < ARRAY_SIZE(sig_sicodes)) &&
		    (si_code <= sig_sicodes[sig].limit)) {
			layout = sig_sicodes[sig].layout;
			/* Handle the exceptions */
			if ((sig == SIGBUS) &&
			    (si_code >= BUS_MCEERR_AR) && (si_code <= BUS_MCEERR_AO))
				layout = SIL_FAULT_MCEERR;
			else if ((sig == SIGSEGV) && (si_code == SEGV_BNDERR))
				layout = SIL_FAULT_BNDERR;
#ifdef SEGV_PKUERR
			else if ((sig == SIGSEGV) && (si_code == SEGV_PKUERR))
				layout = SIL_FAULT_PKUERR;
#endif
		}
		else if (si_code <= NSIGPOLL)
			layout = SIL_POLL;
	} else {
		if (si_code == SI_TIMER)
			layout = SIL_TIMER;
		else if (si_code == SI_SIGIO)
			layout = SIL_POLL;
		else if (si_code < 0)
			layout = SIL_RT;
	}
	return layout;
}

static inline char __user *si_expansion(const siginfo_t __user *info)
{
	return ((char __user *)info) + sizeof(struct kernel_siginfo);
}

/**
 * copy_siginfo_to_user - 将内核信号信息复制到用户空间
 * @to: 用户空间目标地址
 * @from: 内核信号信息结构体
 *
 * 将内核中的信号信息安全地复制到用户空间，处理不同架构的差异
 * 返回值: 成功返回0，失败返回负值错误码
 */
int copy_siginfo_to_user(siginfo_t __user *to, const kernel_siginfo_t *from)
{
	char __user *expansion = si_expansion(to);
	if (copy_to_user(to, from , sizeof(struct kernel_siginfo)))
		return -EFAULT;
	if (clear_user(expansion, SI_EXPANSION_SIZE))
		return -EFAULT;
	return 0;
}

static int post_copy_siginfo_from_user(kernel_siginfo_t *info,
				       const siginfo_t __user *from)
{
	if (unlikely(!known_siginfo_layout(info->si_signo, info->si_code))) {
		char __user *expansion = si_expansion(from);
		char buf[SI_EXPANSION_SIZE];
		int i;
		/*
		 * An unknown si_code might need more than
		 * sizeof(struct kernel_siginfo) bytes.  Verify all of the
		 * extra bytes are 0.  This guarantees copy_siginfo_to_user
		 * will return this data to userspace exactly.
		 */
		if (copy_from_user(&buf, expansion, SI_EXPANSION_SIZE))
			return -EFAULT;
		for (i = 0; i < SI_EXPANSION_SIZE; i++) {
			if (buf[i] != 0)
				return -E2BIG;
		}
	}
	return 0;
}

static int __copy_siginfo_from_user(int signo, kernel_siginfo_t *to,
				    const siginfo_t __user *from)
{
	if (copy_from_user(to, from, sizeof(struct kernel_siginfo)))
		return -EFAULT;
	to->si_signo = signo;
	return post_copy_siginfo_from_user(to, from);
}

/**
 * copy_siginfo_from_user - 从用户空间复制信号信息到内核
 * @to: 内核信号信息结构体
 * @from: 用户空间信号信息地址
 *
 * 从用户空间安全地复制信号信息到内核，验证数据有效性
 * 返回值: 成功返回0，失败返回负值错误码
 */
int copy_siginfo_from_user(kernel_siginfo_t *to, const siginfo_t __user *from)
{
	if (copy_from_user(to, from, sizeof(struct kernel_siginfo)))
		return -EFAULT;
	return post_copy_siginfo_from_user(to, from);
}

#ifdef CONFIG_COMPAT
/**
 * copy_siginfo_to_external32 - copy a kernel siginfo into a compat user siginfo
 * @to: compat siginfo destination
 * @from: kernel siginfo source
 *
 * Note: This function does not work properly for the SIGCHLD on x32, but
 * fortunately it doesn't have to.  The only valid callers for this function are
 * copy_siginfo_to_user32, which is overriden for x32 and the coredump code.
 * The latter does not care because SIGCHLD will never cause a coredump.
 */
/**
 * copy_siginfo_to_external32 - 将内核信号信息转换为32位兼容格式
 * @to: 32位兼容信号信息结构体
 * @from: 内核信号信息结构体
 *
 * 将64位内核的信号信息转换为32位程序可理解的格式
 * 用于64位内核上运行32位程序的兼容性支持
 */
void copy_siginfo_to_external32(struct compat_siginfo *to,
		const struct kernel_siginfo *from)
{
	memset(to, 0, sizeof(*to));

	to->si_signo = from->si_signo;
	to->si_errno = from->si_errno;
	to->si_code  = from->si_code;
	switch(siginfo_layout(from->si_signo, from->si_code)) {
	case SIL_KILL:
		to->si_pid = from->si_pid;
		to->si_uid = from->si_uid;
		break;
	case SIL_TIMER:
		to->si_tid     = from->si_tid;
		to->si_overrun = from->si_overrun;
		to->si_int     = from->si_int;
		break;
	case SIL_POLL:
		to->si_band = from->si_band;
		to->si_fd   = from->si_fd;
		break;
	case SIL_FAULT:
		to->si_addr = ptr_to_compat(from->si_addr);
#ifdef __ARCH_SI_TRAPNO
		to->si_trapno = from->si_trapno;
#endif
		break;
	case SIL_FAULT_MCEERR:
		to->si_addr = ptr_to_compat(from->si_addr);
#ifdef __ARCH_SI_TRAPNO
		to->si_trapno = from->si_trapno;
#endif
		to->si_addr_lsb = from->si_addr_lsb;
		break;
	case SIL_FAULT_BNDERR:
		to->si_addr = ptr_to_compat(from->si_addr);
#ifdef __ARCH_SI_TRAPNO
		to->si_trapno = from->si_trapno;
#endif
		to->si_lower = ptr_to_compat(from->si_lower);
		to->si_upper = ptr_to_compat(from->si_upper);
		break;
	case SIL_FAULT_PKUERR:
		to->si_addr = ptr_to_compat(from->si_addr);
#ifdef __ARCH_SI_TRAPNO
		to->si_trapno = from->si_trapno;
#endif
		to->si_pkey = from->si_pkey;
		break;
	case SIL_CHLD:
		to->si_pid = from->si_pid;
		to->si_uid = from->si_uid;
		to->si_status = from->si_status;
		to->si_utime = from->si_utime;
		to->si_stime = from->si_stime;
		break;
	case SIL_RT:
		to->si_pid = from->si_pid;
		to->si_uid = from->si_uid;
		to->si_int = from->si_int;
		break;
	case SIL_SYS:
		to->si_call_addr = ptr_to_compat(from->si_call_addr);
		to->si_syscall   = from->si_syscall;
		to->si_arch      = from->si_arch;
		break;
	}
}

/**
 * __copy_siginfo_to_user32 - 将内核信号信息复制到32位用户空间
 * @to: 32位用户空间目标地址
 * @from: 内核信号信息结构体
 *
 * 将内核信号信息复制到32位用户空间，处理字段大小和对齐差异
 * 返回值: 成功返回0，失败返回负值错误码
 */
int __copy_siginfo_to_user32(struct compat_siginfo __user *to,
			   const struct kernel_siginfo *from)
{
	struct compat_siginfo new;

	copy_siginfo_to_external32(&new, from);
	if (copy_to_user(to, &new, sizeof(struct compat_siginfo)))
		return -EFAULT;
	return 0;
}

static int post_copy_siginfo_from_user32(kernel_siginfo_t *to,
					 const struct compat_siginfo *from)
{
	clear_siginfo(to);
	to->si_signo = from->si_signo;
	to->si_errno = from->si_errno;
	to->si_code  = from->si_code;
	switch(siginfo_layout(from->si_signo, from->si_code)) {
	case SIL_KILL:
		to->si_pid = from->si_pid;
		to->si_uid = from->si_uid;
		break;
	case SIL_TIMER:
		to->si_tid     = from->si_tid;
		to->si_overrun = from->si_overrun;
		to->si_int     = from->si_int;
		break;
	case SIL_POLL:
		to->si_band = from->si_band;
		to->si_fd   = from->si_fd;
		break;
	case SIL_FAULT:
		to->si_addr = compat_ptr(from->si_addr);
#ifdef __ARCH_SI_TRAPNO
		to->si_trapno = from->si_trapno;
#endif
		break;
	case SIL_FAULT_MCEERR:
		to->si_addr = compat_ptr(from->si_addr);
#ifdef __ARCH_SI_TRAPNO
		to->si_trapno = from->si_trapno;
#endif
		to->si_addr_lsb = from->si_addr_lsb;
		break;
	case SIL_FAULT_BNDERR:
		to->si_addr = compat_ptr(from->si_addr);
#ifdef __ARCH_SI_TRAPNO
		to->si_trapno = from->si_trapno;
#endif
		to->si_lower = compat_ptr(from->si_lower);
		to->si_upper = compat_ptr(from->si_upper);
		break;
	case SIL_FAULT_PKUERR:
		to->si_addr = compat_ptr(from->si_addr);
#ifdef __ARCH_SI_TRAPNO
		to->si_trapno = from->si_trapno;
#endif
		to->si_pkey = from->si_pkey;
		break;
	case SIL_CHLD:
		to->si_pid    = from->si_pid;
		to->si_uid    = from->si_uid;
		to->si_status = from->si_status;
#ifdef CONFIG_X86_X32_ABI
		if (in_x32_syscall()) {
			to->si_utime = from->_sifields._sigchld_x32._utime;
			to->si_stime = from->_sifields._sigchld_x32._stime;
		} else
#endif
		{
			to->si_utime = from->si_utime;
			to->si_stime = from->si_stime;
		}
		break;
	case SIL_RT:
		to->si_pid = from->si_pid;
		to->si_uid = from->si_uid;
		to->si_int = from->si_int;
		break;
	case SIL_SYS:
		to->si_call_addr = compat_ptr(from->si_call_addr);
		to->si_syscall   = from->si_syscall;
		to->si_arch      = from->si_arch;
		break;
	}
	return 0;
}

static int __copy_siginfo_from_user32(int signo, struct kernel_siginfo *to,
				      const struct compat_siginfo __user *ufrom)
{
	struct compat_siginfo from;

	if (copy_from_user(&from, ufrom, sizeof(struct compat_siginfo)))
		return -EFAULT;

	from.si_signo = signo;
	return post_copy_siginfo_from_user32(to, &from);
}

/**
 * copy_siginfo_from_user32 - 从32位用户空间复制信号信息到内核
 * @to: 内核信号信息结构体
 * @from: 32位用户空间信号信息地址
 *
 * 从32位用户空间复制信号信息到内核，处理格式转换和验证
 * 返回值: 成功返回0，失败返回负值错误码
 */
int copy_siginfo_from_user32(struct kernel_siginfo *to,
			     const struct compat_siginfo __user *ufrom)
{
	struct compat_siginfo from;

	if (copy_from_user(&from, ufrom, sizeof(struct compat_siginfo)))
		return -EFAULT;

	return post_copy_siginfo_from_user32(to, &from);
}
#endif /* CONFIG_COMPAT */

/**
 *  do_sigtimedwait - wait for queued signals specified in @which
 *  @which: queued signals to wait for
 *  @info: if non-null, the signal's siginfo is returned here
 *  @ts: upper bound on process time suspension
 */
static int do_sigtimedwait(const sigset_t *which, kernel_siginfo_t *info,
		    const struct timespec64 *ts)
{
	ktime_t *to = NULL, timeout = KTIME_MAX;
	struct task_struct *tsk = current;
	sigset_t mask = *which;
	int sig, ret = 0;

	if (ts) {
		if (!timespec64_valid(ts))
			return -EINVAL;
		timeout = timespec64_to_ktime(*ts);
		to = &timeout;
	}

	/*
	 * Invert the set of allowed signals to get those we want to block.
	 */
	sigdelsetmask(&mask, sigmask(SIGKILL) | sigmask(SIGSTOP));
	signotset(&mask);

	spin_lock_irq(&tsk->sighand->siglock);
	sig = dequeue_signal(tsk, &mask, info);
	if (!sig && timeout) {
		/*
		 * None ready, temporarily unblock those we're interested
		 * while we are sleeping in so that we'll be awakened when
		 * they arrive. Unblocking is always fine, we can avoid
		 * set_current_blocked().
		 */
		tsk->real_blocked = tsk->blocked;
		sigandsets(&tsk->blocked, &tsk->blocked, &mask);
		recalc_sigpending();
		spin_unlock_irq(&tsk->sighand->siglock);

		__set_current_state(TASK_INTERRUPTIBLE);
		ret = freezable_schedule_hrtimeout_range(to, tsk->timer_slack_ns,
							 HRTIMER_MODE_REL);
		spin_lock_irq(&tsk->sighand->siglock);
		__set_task_blocked(tsk, &tsk->real_blocked);
		sigemptyset(&tsk->real_blocked);
		sig = dequeue_signal(tsk, &mask, info);
	}
	spin_unlock_irq(&tsk->sighand->siglock);

	if (sig)
		return sig;
	return ret ? -EINTR : -EAGAIN;
}

/**
 *  sys_rt_sigtimedwait - synchronously wait for queued signals specified
 *			in @uthese
 *  @uthese: queued signals to wait for
 *  @uinfo: if non-null, the signal's siginfo is returned here
 *  @uts: upper bound on process time suspension
 *  @sigsetsize: size of sigset_t type
 */
SYSCALL_DEFINE4(rt_sigtimedwait, const sigset_t __user *, uthese,
		siginfo_t __user *, uinfo,
		const struct __kernel_timespec __user *, uts,
		size_t, sigsetsize)
{
	sigset_t these;
	struct timespec64 ts;
	kernel_siginfo_t info;
	int ret;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if (copy_from_user(&these, uthese, sizeof(these)))
		return -EFAULT;

	if (uts) {
		if (get_timespec64(&ts, uts))
			return -EFAULT;
	}

	ret = do_sigtimedwait(&these, &info, uts ? &ts : NULL);

	if (ret > 0 && uinfo) {
		if (copy_siginfo_to_user(uinfo, &info))
			ret = -EFAULT;
	}

	return ret;
}

#ifdef CONFIG_COMPAT_32BIT_TIME
SYSCALL_DEFINE4(rt_sigtimedwait_time32, const sigset_t __user *, uthese,
		siginfo_t __user *, uinfo,
		const struct old_timespec32 __user *, uts,
		size_t, sigsetsize)
{
	sigset_t these;
	struct timespec64 ts;
	kernel_siginfo_t info;
	int ret;

	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if (copy_from_user(&these, uthese, sizeof(these)))
		return -EFAULT;

	if (uts) {
		if (get_old_timespec32(&ts, uts))
			return -EFAULT;
	}

	ret = do_sigtimedwait(&these, &info, uts ? &ts : NULL);

	if (ret > 0 && uinfo) {
		if (copy_siginfo_to_user(uinfo, &info))
			ret = -EFAULT;
	}

	return ret;
}
#endif

#ifdef CONFIG_COMPAT
COMPAT_SYSCALL_DEFINE4(rt_sigtimedwait_time64, compat_sigset_t __user *, uthese,
		struct compat_siginfo __user *, uinfo,
		struct __kernel_timespec __user *, uts, compat_size_t, sigsetsize)
{
	sigset_t s;
	struct timespec64 t;
	kernel_siginfo_t info;
	long ret;

	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if (get_compat_sigset(&s, uthese))
		return -EFAULT;

	if (uts) {
		if (get_timespec64(&t, uts))
			return -EFAULT;
	}

	ret = do_sigtimedwait(&s, &info, uts ? &t : NULL);

	if (ret > 0 && uinfo) {
		if (copy_siginfo_to_user32(uinfo, &info))
			ret = -EFAULT;
	}

	return ret;
}

#ifdef CONFIG_COMPAT_32BIT_TIME
COMPAT_SYSCALL_DEFINE4(rt_sigtimedwait_time32, compat_sigset_t __user *, uthese,
		struct compat_siginfo __user *, uinfo,
		struct old_timespec32 __user *, uts, compat_size_t, sigsetsize)
{
	sigset_t s;
	struct timespec64 t;
	kernel_siginfo_t info;
	long ret;

	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if (get_compat_sigset(&s, uthese))
		return -EFAULT;

	if (uts) {
		if (get_old_timespec32(&t, uts))
			return -EFAULT;
	}

	ret = do_sigtimedwait(&s, &info, uts ? &t : NULL);

	if (ret > 0 && uinfo) {
		if (copy_siginfo_to_user32(uinfo, &info))
			ret = -EFAULT;
	}

	return ret;
}
#endif
#endif

/**
 * prepare_kill_siginfo - 准备kill信号的信息结构体
 * @sig: 信号编号
 * @info: 信号信息结构体指针
 *
 * 为kill系统调用准备标准的信号信息，设置发送者PID和UID
 */
static inline void prepare_kill_siginfo(int sig, struct kernel_siginfo *info)
{
	clear_siginfo(info);
	info->si_signo = sig;
	info->si_errno = 0;
	info->si_code = SI_USER;
	info->si_pid = task_tgid_vnr(current);
	info->si_uid = from_kuid_munged(current_user_ns(), current_uid());
}

/**
 *  sys_kill - send a signal to a process
 *  @pid: the PID of the process
 *  @sig: signal to be sent
 */
/**
 * sys_kill - kill系统调用实现
 * @pid: 目标进程/进程组ID
 * @sig: 信号编号
 * 返回值：成功返回0，失败返回负错误码
 */
SYSCALL_DEFINE2(kill, pid_t, pid, int, sig)
{
	struct kernel_siginfo info;

	prepare_kill_siginfo(sig, &info);  // 准备kill信号信息，设置发送者PID/UID

	return kill_something_info(sig, &info, pid);  // 根据pid值决定发送目标(进程/进程组/所有进程)
}

/*
 * Verify that the signaler and signalee either are in the same pid namespace
 * or that the signaler's pid namespace is an ancestor of the signalee's pid
 * namespace.
 */
/**
 * access_pidfd_pidns - 检查pidfd的PID命名空间访问权限
 * @pid: 目标进程的PID结构
 *
 * 验证信号发送者和接收者是否在同一个PID命名空间，
 * 或者发送者的PID命名空间是接收者的祖先命名空间
 * 返回值: 有权限访问返回true，否则返回false
 */
static bool access_pidfd_pidns(struct pid *pid)
{
	struct pid_namespace *active = task_active_pid_ns(current);
	struct pid_namespace *p = ns_of_pid(pid);

	for (;;) {
		if (!p)
			return false;
		if (p == active)
			break;
		p = p->parent;
	}

	return true;
}

/**
 * copy_siginfo_from_user_any - 从用户空间复制信号信息(兼容模式)
 * @kinfo: 内核信号信息结构体
 * @info: 用户空间信号信息指针
 *
 * 根据当前系统调用模式(32位或64位)从用户空间复制信号信息，
 * 自动处理兼容性转换
 * 返回值: 成功返回0，失败返回负值错误码
 */
static int copy_siginfo_from_user_any(kernel_siginfo_t *kinfo, siginfo_t *info)
{
#ifdef CONFIG_COMPAT
	/*
	 * Avoid hooking up compat syscalls and instead handle necessary
	 * conversions here. Note, this is a stop-gap measure and should not be
	 * considered a generic solution.
	 */
	if (in_compat_syscall())
		return copy_siginfo_from_user32(
			kinfo, (struct compat_siginfo __user *)info);
#endif
	return copy_siginfo_from_user(kinfo, info);
}

/**
 * pidfd_to_pid - 从文件描述符获取PID结构
 * @file: 文件结构体指针
 *
 * 将pidfd文件描述符转换为对应的PID结构体，
 * 支持进程pidfd和线程组pidfd两种类型
 * 返回值: 成功返回PID结构体指针，失败返回错误指针
 */
static struct pid *pidfd_to_pid(const struct file *file)
{
	struct pid *pid;

	pid = pidfd_pid(file);
	if (!IS_ERR(pid))
		return pid;

	return tgid_pidfd_to_pid(file);
}

/**
 * sys_pidfd_send_signal - Signal a process through a pidfd
 * @pidfd:  file descriptor of the process
 * @sig:    signal to send
 * @info:   signal info
 * @flags:  future flags
 *
 * The syscall currently only signals via PIDTYPE_PID which covers
 * kill(<positive-pid>, <signal>. It does not signal threads or process
 * groups.
 * In order to extend the syscall to threads and process groups the @flags
 * argument should be used. In essence, the @flags argument will determine
 * what is signaled and not the file descriptor itself. Put in other words,
 * grouping is a property of the flags argument not a property of the file
 * descriptor.
 *
 * Return: 0 on success, negative errno on failure
 */
SYSCALL_DEFINE4(pidfd_send_signal, int, pidfd, int, sig,
		siginfo_t __user *, info, unsigned int, flags)
{
	int ret;
	struct fd f;
	struct pid *pid;
	kernel_siginfo_t kinfo;

	/* Enforce flags be set to 0 until we add an extension. */
	if (flags)  // 目前不支持任何标志位，为将来扩展保留
		return -EINVAL;

	f = fdget(pidfd);  // 获取文件描述符结构
	if (!f.file)
		return -EBADF;

	/* Is this a pidfd? */
	pid = pidfd_to_pid(f.file);  // 验证这是一个有效的pidfd并获取PID结构
	if (IS_ERR(pid)) {
		ret = PTR_ERR(pid);
		goto err;
	}

	ret = -EINVAL;
	if (!access_pidfd_pidns(pid))
		goto err;

	if (info) {
		ret = copy_siginfo_from_user_any(&kinfo, info);
		if (unlikely(ret))
			goto err;

		ret = -EINVAL;
		if (unlikely(sig != kinfo.si_signo))
			goto err;

		/* Only allow sending arbitrary signals to yourself. */
		ret = -EPERM;
		if ((task_pid(current) != pid) &&
		    (kinfo.si_code >= 0 || kinfo.si_code == SI_TKILL))
			goto err;
	} else {
		prepare_kill_siginfo(sig, &kinfo);
	}

	ret = kill_pid_info(sig, &kinfo, pid);

err:
	fdput(f);
	return ret;
}

/**
 * do_send_specific - 向特定线程发送信号的内部实现
 * @tgid: 线程组ID
 * @pid: 线程ID
 * @sig: 信号编号
 * @info: 信号信息结构体
 *
 * tkill/tgkill系统调用的核心实现，向指定线程发送信号
 * 返回值: 成功返回0，失败返回负值错误码
 */
static int
do_send_specific(pid_t tgid, pid_t pid, int sig, struct kernel_siginfo *info)
{
	struct task_struct *p;
	int error = -ESRCH;

	rcu_read_lock();
	p = find_task_by_vpid(pid);
	if (p && (tgid <= 0 || task_tgid_vnr(p) == tgid)) {
		error = check_kill_permission(sig, info, p);
		/*
		 * The null signal is a permissions and process existence
		 * probe.  No signal is actually delivered.
		 */
		if (!error && sig) {
			error = do_send_sig_info(sig, info, p, PIDTYPE_PID);
			/*
			 * If lock_task_sighand() failed we pretend the task
			 * dies after receiving the signal. The window is tiny,
			 * and the signal is private anyway.
			 */
			if (unlikely(error == -ESRCH))
				error = 0;
		}
	}
	rcu_read_unlock();

	return error;
}

/**
 * do_tkill - tkill操作的核心实现
 * @tgid: 线程组ID(0表示不检查线程组)
 * @pid: 目标线程ID
 * @sig: 信号编号
 *
 * 准备tkill信号信息并发送到指定线程
 * 返回值: 成功返回0，失败返回负值错误码
 */
static int do_tkill(pid_t tgid, pid_t pid, int sig)
{
	struct kernel_siginfo info;

	clear_siginfo(&info);
	info.si_signo = sig;
	info.si_errno = 0;
	info.si_code = SI_TKILL;
	info.si_pid = task_tgid_vnr(current);
	info.si_uid = from_kuid_munged(current_user_ns(), current_uid());

	return do_send_specific(tgid, pid, sig, &info);
}

/**
 *  sys_tgkill - send signal to one specific thread
 *  @tgid: the thread group ID of the thread
 *  @pid: the PID of the thread
 *  @sig: signal to be sent
 *
 *  This syscall also checks the @tgid and returns -ESRCH even if the PID
 *  exists but it's not belonging to the target process anymore. This
 *  method solves the problem of threads exiting and PIDs getting reused.
 */
SYSCALL_DEFINE3(tgkill, pid_t, tgid, pid_t, pid, int, sig)
{
	/* This is only valid for single tasks */
	if (pid <= 0 || tgid <= 0)  // 检查参数有效性：PID和TGID必须为正数
		return -EINVAL;

	return do_tkill(tgid, pid, sig);  // 向指定线程组中的特定线程发送信号
}

/**
 *  sys_tkill - send signal to one specific task
 *  @pid: the PID of the task
 *  @sig: signal to be sent
 *
 *  Send a signal to only one task, even if it's a CLONE_THREAD task.
 */
/**
 * sys_tkill - tkill系统调用实现
 * @pid: 目标线程ID
 * @sig: 信号编号
 * 返回值：成功返回0，失败返回负错误码
 */
SYSCALL_DEFINE2(tkill, pid_t, pid, int, sig)
{
	/* This is only valid for single tasks */
	if (pid <= 0)  // 检查PID有效性：必须为正数
		return -EINVAL;

	return do_tkill(0, pid, sig);  // TGID=0表示不检查线程组，直接向指定线程发送信号
}

/**
 * do_rt_sigqueueinfo - 实时信号队列信息发送的内部实现
 * @pid: 目标进程ID
 * @sig: 信号编号
 * @info: 信号信息结构体
 *
 * rt_sigqueueinfo系统调用的核心实现，向进程发送携带用户数据的实时信号
 * 返回值: 成功返回0，失败返回负值错误码
 */
static int do_rt_sigqueueinfo(pid_t pid, int sig, kernel_siginfo_t *info)
{
	/* Not even root can pretend to send signals from the kernel.
	 * Nor can they impersonate a kill()/tgkill(), which adds source info.
	 */
	if ((info->si_code >= 0 || info->si_code == SI_TKILL) &&
	    (task_pid_vnr(current) != pid))
		return -EPERM;

	/* POSIX.1b doesn't mention process groups.  */
	return kill_proc_info(sig, info, pid);
}

/**
 *  sys_rt_sigqueueinfo - send signal information to a signal
 *  @pid: the PID of the thread
 *  @sig: signal to be sent
 *  @uinfo: signal info to be sent
 */
SYSCALL_DEFINE3(rt_sigqueueinfo, pid_t, pid, int, sig,
		siginfo_t __user *, uinfo)
{
	kernel_siginfo_t info;
	int ret = __copy_siginfo_from_user(sig, &info, uinfo);
	if (unlikely(ret))
		return ret;
	return do_rt_sigqueueinfo(pid, sig, &info);
}

#ifdef CONFIG_COMPAT
COMPAT_SYSCALL_DEFINE3(rt_sigqueueinfo,
			compat_pid_t, pid,
			int, sig,
			struct compat_siginfo __user *, uinfo)
{
	kernel_siginfo_t info;
	int ret = __copy_siginfo_from_user32(sig, &info, uinfo);
	if (unlikely(ret))
		return ret;
	return do_rt_sigqueueinfo(pid, sig, &info);
}
#endif

/**
 * do_rt_tgsigqueueinfo - 线程组实时信号队列信息发送的内部实现
 * @tgid: 线程组ID
 * @pid: 目标线程ID
 * @sig: 信号编号
 * @info: 信号信息结构体
 *
 * rt_tgsigqueueinfo系统调用的核心实现，向指定线程发送实时信号
 * 返回值: 成功返回0，失败返回负值错误码
 */
static int do_rt_tgsigqueueinfo(pid_t tgid, pid_t pid, int sig, kernel_siginfo_t *info)
{
	/* This is only valid for single tasks */
	if (pid <= 0 || tgid <= 0)
		return -EINVAL;

	/* Not even root can pretend to send signals from the kernel.
	 * Nor can they impersonate a kill()/tgkill(), which adds source info.
	 */
	if ((info->si_code >= 0 || info->si_code == SI_TKILL) &&
	    (task_pid_vnr(current) != pid))
		return -EPERM;

	return do_send_specific(tgid, pid, sig, info);
}

SYSCALL_DEFINE4(rt_tgsigqueueinfo, pid_t, tgid, pid_t, pid, int, sig,
		siginfo_t __user *, uinfo)
{
	kernel_siginfo_t info;
	int ret = __copy_siginfo_from_user(sig, &info, uinfo);
	if (unlikely(ret))
		return ret;
	return do_rt_tgsigqueueinfo(tgid, pid, sig, &info);
}

#ifdef CONFIG_COMPAT
COMPAT_SYSCALL_DEFINE4(rt_tgsigqueueinfo,
			compat_pid_t, tgid,
			compat_pid_t, pid,
			int, sig,
			struct compat_siginfo __user *, uinfo)
{
	kernel_siginfo_t info;
	int ret = __copy_siginfo_from_user32(sig, &info, uinfo);
	if (unlikely(ret))
		return ret;
	return do_rt_tgsigqueueinfo(tgid, pid, sig, &info);
}
#endif

/*
 * For kthreads only, must not be used if cloned with CLONE_SIGHAND
 */
/**
 * kernel_sigaction - 内核模式设置信号处理器
 * @sig: 信号编号
 * @action: 信号处理器函数指针
 *
 * 仅供内核线程使用，设置指定信号的处理器
 * 禁止在使用CLONE_SIGHAND的进程中调用
 */
void kernel_sigaction(int sig, __sighandler_t action)
{
	spin_lock_irq(&current->sighand->siglock);
	current->sighand->action[sig - 1].sa.sa_handler = action;
	if (action == SIG_IGN) {
		sigset_t mask;

		sigemptyset(&mask);
		sigaddset(&mask, sig);

		flush_sigqueue_mask(&mask, &current->signal->shared_pending);
		flush_sigqueue_mask(&mask, &current->pending);
		recalc_sigpending();
	}
	spin_unlock_irq(&current->sighand->siglock);
}
EXPORT_SYMBOL(kernel_sigaction);

void __weak sigaction_compat_abi(struct k_sigaction *act,
		struct k_sigaction *oact)
{
}

/**
 * do_sigaction - 执行信号动作设置的核心函数
 * @sig: 信号编号
 * @act: 新的信号动作结构体
 * @oact: 保存旧的信号动作结构体
 *
 * 设置或查询指定信号的处理动作，这是sigaction系统调用的核心实现
 * 返回值: 成功返回0，失败返回负值错误码
 */
int do_sigaction(int sig, struct k_sigaction *act, struct k_sigaction *oact)
{
	struct task_struct *p = current, *t;
	struct k_sigaction *k;
	sigset_t mask;

	if (!valid_signal(sig) || sig < 1 || (act && sig_kernel_only(sig)))
		return -EINVAL;

	k = &p->sighand->action[sig-1];

	spin_lock_irq(&p->sighand->siglock);
	if (oact)
		*oact = *k;

	sigaction_compat_abi(act, oact);

	if (act) {
		sigdelsetmask(&act->sa.sa_mask,
			      sigmask(SIGKILL) | sigmask(SIGSTOP));
		*k = *act;
		/*
		 * POSIX 3.3.1.3:
		 *  "Setting a signal action to SIG_IGN for a signal that is
		 *   pending shall cause the pending signal to be discarded,
		 *   whether or not it is blocked."
		 *
		 *  "Setting a signal action to SIG_DFL for a signal that is
		 *   pending and whose default action is to ignore the signal
		 *   (for example, SIGCHLD), shall cause the pending signal to
		 *   be discarded, whether or not it is blocked"
		 */
		if (sig_handler_ignored(sig_handler(p, sig), sig)) {
			sigemptyset(&mask);
			sigaddset(&mask, sig);
			flush_sigqueue_mask(&mask, &p->signal->shared_pending);
			for_each_thread(p, t)
				flush_sigqueue_mask(&mask, &t->pending);
		}
	}

	spin_unlock_irq(&p->sighand->siglock);
	return 0;
}

static int
do_sigaltstack (const stack_t *ss, stack_t *oss, unsigned long sp,
		size_t min_ss_size)
{
	struct task_struct *t = current;

	if (oss) {
		memset(oss, 0, sizeof(stack_t));
		oss->ss_sp = (void __user *) t->sas_ss_sp;
		oss->ss_size = t->sas_ss_size;
		oss->ss_flags = sas_ss_flags(sp) |
			(current->sas_ss_flags & SS_FLAG_BITS);
	}

	if (ss) {
		void __user *ss_sp = ss->ss_sp;
		size_t ss_size = ss->ss_size;
		unsigned ss_flags = ss->ss_flags;
		int ss_mode;

		if (unlikely(on_sig_stack(sp)))
			return -EPERM;

		ss_mode = ss_flags & ~SS_FLAG_BITS;
		if (unlikely(ss_mode != SS_DISABLE && ss_mode != SS_ONSTACK &&
				ss_mode != 0))
			return -EINVAL;

		if (ss_mode == SS_DISABLE) {
			ss_size = 0;
			ss_sp = NULL;
		} else {
			if (unlikely(ss_size < min_ss_size))
				return -ENOMEM;
		}

		t->sas_ss_sp = (unsigned long) ss_sp;
		t->sas_ss_size = ss_size;
		t->sas_ss_flags = ss_flags;
	}
	return 0;
}

SYSCALL_DEFINE2(sigaltstack,const stack_t __user *,uss, stack_t __user *,uoss)
{
	stack_t new, old;
	int err;
	if (uss && copy_from_user(&new, uss, sizeof(stack_t)))
		return -EFAULT;
	err = do_sigaltstack(uss ? &new : NULL, uoss ? &old : NULL,
			      current_user_stack_pointer(),
			      MINSIGSTKSZ);
	if (!err && uoss && copy_to_user(uoss, &old, sizeof(stack_t)))
		err = -EFAULT;
	return err;
}

/**
 * restore_altstack - 恢复信号备用栈设置
 * @uss: 用户空间信号栈结构体指针
 *
 * 从用户空间恢复信号备用栈的设置，用于信号处理器返回时恢复栈状态
 * 返回值: 成功返回0，失败返回负值错误码
 */
int restore_altstack(const stack_t __user *uss)
{
	stack_t new;
	if (copy_from_user(&new, uss, sizeof(stack_t)))
		return -EFAULT;
	(void)do_sigaltstack(&new, NULL, current_user_stack_pointer(),
			     MINSIGSTKSZ);
	/* squash all but EFAULT for now */
	return 0;
}

/**
 * __save_altstack - 保存当前信号备用栈设置到用户空间
 * @uss: 用户空间信号栈结构体指针
 * @sp: 当前栈指针
 *
 * 将当前进程的信号备用栈设置保存到用户空间，
 * 如果启用了SS_AUTODISARM标志则自动重置栈设置
 * 返回值: 成功返回0，失败返回负值错误码
 */
int __save_altstack(stack_t __user *uss, unsigned long sp)
{
	struct task_struct *t = current;
	int err = __put_user((void __user *)t->sas_ss_sp, &uss->ss_sp) |
		__put_user(t->sas_ss_flags, &uss->ss_flags) |
		__put_user(t->sas_ss_size, &uss->ss_size);
	if (err)
		return err;
	if (t->sas_ss_flags & SS_AUTODISARM)
		sas_ss_reset(t);
	return 0;
}

#ifdef CONFIG_COMPAT
static int do_compat_sigaltstack(const compat_stack_t __user *uss_ptr,
				 compat_stack_t __user *uoss_ptr)
{
	stack_t uss, uoss;
	int ret;

	if (uss_ptr) {
		compat_stack_t uss32;
		if (copy_from_user(&uss32, uss_ptr, sizeof(compat_stack_t)))
			return -EFAULT;
		uss.ss_sp = compat_ptr(uss32.ss_sp);
		uss.ss_flags = uss32.ss_flags;
		uss.ss_size = uss32.ss_size;
	}
	ret = do_sigaltstack(uss_ptr ? &uss : NULL, &uoss,
			     compat_user_stack_pointer(),
			     COMPAT_MINSIGSTKSZ);
	if (ret >= 0 && uoss_ptr)  {
		compat_stack_t old;
		memset(&old, 0, sizeof(old));
		old.ss_sp = ptr_to_compat(uoss.ss_sp);
		old.ss_flags = uoss.ss_flags;
		old.ss_size = uoss.ss_size;
		if (copy_to_user(uoss_ptr, &old, sizeof(compat_stack_t)))
			ret = -EFAULT;
	}
	return ret;
}

COMPAT_SYSCALL_DEFINE2(sigaltstack,
			const compat_stack_t __user *, uss_ptr,
			compat_stack_t __user *, uoss_ptr)
{
	return do_compat_sigaltstack(uss_ptr, uoss_ptr);
}

/**
 * compat_restore_altstack - 恢复32位兼容模式的信号备用栈
 * @uss: 32位用户空间信号栈结构体指针
 *
 * 32位程序在64位内核上恢复信号备用栈设置
 * 返回值: 成功返回0，失败返回-EFAULT
 */
int compat_restore_altstack(const compat_stack_t __user *uss)
{
	int err = do_compat_sigaltstack(uss, NULL);
	/* squash all but -EFAULT for now */
	return err == -EFAULT ? err : 0;
}

/**
 * __compat_save_altstack - 保存32位兼容模式的信号备用栈设置
 * @uss: 32位用户空间信号栈结构体指针
 * @sp: 当前栈指针
 *
 * 将当前进程的信号备用栈设置保存为32位格式到用户空间
 * 返回值: 成功返回0，失败返回负值错误码
 */
int __compat_save_altstack(compat_stack_t __user *uss, unsigned long sp)
{
	int err;
	struct task_struct *t = current;
	err = __put_user(ptr_to_compat((void __user *)t->sas_ss_sp),
			 &uss->ss_sp) |
		__put_user(t->sas_ss_flags, &uss->ss_flags) |
		__put_user(t->sas_ss_size, &uss->ss_size);
	if (err)
		return err;
	if (t->sas_ss_flags & SS_AUTODISARM)
		sas_ss_reset(t);
	return 0;
}
#endif

#ifdef __ARCH_WANT_SYS_SIGPENDING

/**
 *  sys_sigpending - examine pending signals
 *  @uset: where mask of pending signal is returned
 */
SYSCALL_DEFINE1(sigpending, old_sigset_t __user *, uset)
{
	sigset_t set;

	if (sizeof(old_sigset_t) > sizeof(*uset))
		return -EINVAL;

	do_sigpending(&set);

	if (copy_to_user(uset, &set, sizeof(old_sigset_t)))
		return -EFAULT;

	return 0;
}

#ifdef CONFIG_COMPAT
COMPAT_SYSCALL_DEFINE1(sigpending, compat_old_sigset_t __user *, set32)
{
	sigset_t set;

	do_sigpending(&set);

	return put_user(set.sig[0], set32);
}
#endif

#endif

#ifdef __ARCH_WANT_SYS_SIGPROCMASK
/**
 *  sys_sigprocmask - examine and change blocked signals
 *  @how: whether to add, remove, or set signals
 *  @nset: signals to add or remove (if non-null)
 *  @oset: previous value of signal mask if non-null
 *
 * Some platforms have their own version with special arguments;
 * others support only sys_rt_sigprocmask.
 */

SYSCALL_DEFINE3(sigprocmask, int, how, old_sigset_t __user *, nset,
		old_sigset_t __user *, oset)
{
	old_sigset_t old_set, new_set;
	sigset_t new_blocked;

	old_set = current->blocked.sig[0];

	if (nset) {
		if (copy_from_user(&new_set, nset, sizeof(*nset)))
			return -EFAULT;

		new_blocked = current->blocked;

		switch (how) {
		case SIG_BLOCK:
			sigaddsetmask(&new_blocked, new_set);
			break;
		case SIG_UNBLOCK:
			sigdelsetmask(&new_blocked, new_set);
			break;
		case SIG_SETMASK:
			new_blocked.sig[0] = new_set;
			break;
		default:
			return -EINVAL;
		}

		set_current_blocked(&new_blocked);
	}

	if (oset) {
		if (copy_to_user(oset, &old_set, sizeof(*oset)))
			return -EFAULT;
	}

	return 0;
}
#endif /* __ARCH_WANT_SYS_SIGPROCMASK */

#ifndef CONFIG_ODD_RT_SIGACTION
/**
 *  sys_rt_sigaction - alter an action taken by a process
 *  @sig: signal to be sent
 *  @act: new sigaction
 *  @oact: used to save the previous sigaction
 *  @sigsetsize: size of sigset_t type
 */
SYSCALL_DEFINE4(rt_sigaction, int, sig,
		const struct sigaction __user *, act,
		struct sigaction __user *, oact,
		size_t, sigsetsize)
{
	struct k_sigaction new_sa, old_sa;
	int ret;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if (act && copy_from_user(&new_sa.sa, act, sizeof(new_sa.sa)))
		return -EFAULT;

	ret = do_sigaction(sig, act ? &new_sa : NULL, oact ? &old_sa : NULL);
	if (ret)
		return ret;

	if (oact && copy_to_user(oact, &old_sa.sa, sizeof(old_sa.sa)))
		return -EFAULT;

	return 0;
}
#ifdef CONFIG_COMPAT
COMPAT_SYSCALL_DEFINE4(rt_sigaction, int, sig,
		const struct compat_sigaction __user *, act,
		struct compat_sigaction __user *, oact,
		compat_size_t, sigsetsize)
{
	struct k_sigaction new_ka, old_ka;
#ifdef __ARCH_HAS_SA_RESTORER
	compat_uptr_t restorer;
#endif
	int ret;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(compat_sigset_t))
		return -EINVAL;

	if (act) {
		compat_uptr_t handler;
		ret = get_user(handler, &act->sa_handler);
		new_ka.sa.sa_handler = compat_ptr(handler);
#ifdef __ARCH_HAS_SA_RESTORER
		ret |= get_user(restorer, &act->sa_restorer);
		new_ka.sa.sa_restorer = compat_ptr(restorer);
#endif
		ret |= get_compat_sigset(&new_ka.sa.sa_mask, &act->sa_mask);
		ret |= get_user(new_ka.sa.sa_flags, &act->sa_flags);
		if (ret)
			return -EFAULT;
	}

	ret = do_sigaction(sig, act ? &new_ka : NULL, oact ? &old_ka : NULL);
	if (!ret && oact) {
		ret = put_user(ptr_to_compat(old_ka.sa.sa_handler), 
			       &oact->sa_handler);
		ret |= put_compat_sigset(&oact->sa_mask, &old_ka.sa.sa_mask,
					 sizeof(oact->sa_mask));
		ret |= put_user(old_ka.sa.sa_flags, &oact->sa_flags);
#ifdef __ARCH_HAS_SA_RESTORER
		ret |= put_user(ptr_to_compat(old_ka.sa.sa_restorer),
				&oact->sa_restorer);
#endif
	}
	return ret;
}
#endif
#endif /* !CONFIG_ODD_RT_SIGACTION */

#ifdef CONFIG_OLD_SIGACTION
SYSCALL_DEFINE3(sigaction, int, sig,
		const struct old_sigaction __user *, act,
	        struct old_sigaction __user *, oact)
{
	struct k_sigaction new_ka, old_ka;
	int ret;

	if (act) {
		old_sigset_t mask;
		if (!access_ok(act, sizeof(*act)) ||
		    __get_user(new_ka.sa.sa_handler, &act->sa_handler) ||
		    __get_user(new_ka.sa.sa_restorer, &act->sa_restorer) ||
		    __get_user(new_ka.sa.sa_flags, &act->sa_flags) ||
		    __get_user(mask, &act->sa_mask))
			return -EFAULT;
#ifdef __ARCH_HAS_KA_RESTORER
		new_ka.ka_restorer = NULL;
#endif
		siginitset(&new_ka.sa.sa_mask, mask);
	}

	ret = do_sigaction(sig, act ? &new_ka : NULL, oact ? &old_ka : NULL);

	if (!ret && oact) {
		if (!access_ok(oact, sizeof(*oact)) ||
		    __put_user(old_ka.sa.sa_handler, &oact->sa_handler) ||
		    __put_user(old_ka.sa.sa_restorer, &oact->sa_restorer) ||
		    __put_user(old_ka.sa.sa_flags, &oact->sa_flags) ||
		    __put_user(old_ka.sa.sa_mask.sig[0], &oact->sa_mask))
			return -EFAULT;
	}

	return ret;
}
#endif
#ifdef CONFIG_COMPAT_OLD_SIGACTION
COMPAT_SYSCALL_DEFINE3(sigaction, int, sig,
		const struct compat_old_sigaction __user *, act,
	        struct compat_old_sigaction __user *, oact)
{
	struct k_sigaction new_ka, old_ka;
	int ret;
	compat_old_sigset_t mask;
	compat_uptr_t handler, restorer;

	if (act) {
		if (!access_ok(act, sizeof(*act)) ||
		    __get_user(handler, &act->sa_handler) ||
		    __get_user(restorer, &act->sa_restorer) ||
		    __get_user(new_ka.sa.sa_flags, &act->sa_flags) ||
		    __get_user(mask, &act->sa_mask))
			return -EFAULT;

#ifdef __ARCH_HAS_KA_RESTORER
		new_ka.ka_restorer = NULL;
#endif
		new_ka.sa.sa_handler = compat_ptr(handler);
		new_ka.sa.sa_restorer = compat_ptr(restorer);
		siginitset(&new_ka.sa.sa_mask, mask);
	}

	ret = do_sigaction(sig, act ? &new_ka : NULL, oact ? &old_ka : NULL);

	if (!ret && oact) {
		if (!access_ok(oact, sizeof(*oact)) ||
		    __put_user(ptr_to_compat(old_ka.sa.sa_handler),
			       &oact->sa_handler) ||
		    __put_user(ptr_to_compat(old_ka.sa.sa_restorer),
			       &oact->sa_restorer) ||
		    __put_user(old_ka.sa.sa_flags, &oact->sa_flags) ||
		    __put_user(old_ka.sa.sa_mask.sig[0], &oact->sa_mask))
			return -EFAULT;
	}
	return ret;
}
#endif

#ifdef CONFIG_SGETMASK_SYSCALL

/*
 * For backwards compatibility.  Functionality superseded by sigprocmask.
 */
SYSCALL_DEFINE0(sgetmask)
{
	/* SMP safe */
	return current->blocked.sig[0];
}

SYSCALL_DEFINE1(ssetmask, int, newmask)
{
	int old = current->blocked.sig[0];
	sigset_t newset;

	siginitset(&newset, newmask);
	set_current_blocked(&newset);

	return old;
}
#endif /* CONFIG_SGETMASK_SYSCALL */

#ifdef __ARCH_WANT_SYS_SIGNAL
/*
 * For backwards compatibility.  Functionality superseded by sigaction.
 */
SYSCALL_DEFINE2(signal, int, sig, __sighandler_t, handler)
{
	struct k_sigaction new_sa, old_sa;
	int ret;

	new_sa.sa.sa_handler = handler;
	new_sa.sa.sa_flags = SA_ONESHOT | SA_NOMASK;
	sigemptyset(&new_sa.sa.sa_mask);

	ret = do_sigaction(sig, &new_sa, &old_sa);

	return ret ? ret : (unsigned long)old_sa.sa.sa_handler;
}
#endif /* __ARCH_WANT_SYS_SIGNAL */

#ifdef __ARCH_WANT_SYS_PAUSE

SYSCALL_DEFINE0(pause)
{
	while (!signal_pending(current)) {
		__set_current_state(TASK_INTERRUPTIBLE);
		schedule();
	}
	return -ERESTARTNOHAND;
}

#endif

/**
 * sigsuspend - 信号挂起的核心实现
 * @set: 临时信号掩码
 *
 * 临时替换进程的信号掩码并挂起进程，直到收到信号为止
 * 实现sigsuspend系统调用的核心逻辑
 * 返回值: 总是返回-ERESTARTNOHAND
 */
static int sigsuspend(sigset_t *set)
{
	current->saved_sigmask = current->blocked;
	set_current_blocked(set);

	while (!signal_pending(current)) {
		__set_current_state(TASK_INTERRUPTIBLE);
		schedule();
	}
	set_restore_sigmask();
	return -ERESTARTNOHAND;
}

/**
 *  sys_rt_sigsuspend - replace the signal mask for a value with the
 *	@unewset value until a signal is received
 *  @unewset: new signal mask value
 *  @sigsetsize: size of sigset_t type
 */
SYSCALL_DEFINE2(rt_sigsuspend, sigset_t __user *, unewset, size_t, sigsetsize)
{
	sigset_t newset;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if (copy_from_user(&newset, unewset, sizeof(newset)))
		return -EFAULT;
	return sigsuspend(&newset);
}
 
#ifdef CONFIG_COMPAT
COMPAT_SYSCALL_DEFINE2(rt_sigsuspend, compat_sigset_t __user *, unewset, compat_size_t, sigsetsize)
{
	sigset_t newset;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if (get_compat_sigset(&newset, unewset))
		return -EFAULT;
	return sigsuspend(&newset);
}
#endif

#ifdef CONFIG_OLD_SIGSUSPEND
SYSCALL_DEFINE1(sigsuspend, old_sigset_t, mask)
{
	sigset_t blocked;
	siginitset(&blocked, mask);
	return sigsuspend(&blocked);
}
#endif
#ifdef CONFIG_OLD_SIGSUSPEND3
SYSCALL_DEFINE3(sigsuspend, int, unused1, int, unused2, old_sigset_t, mask)
{
	sigset_t blocked;
	siginitset(&blocked, mask);
	return sigsuspend(&blocked);
}
#endif

__weak const char *arch_vma_name(struct vm_area_struct *vma)
{
	return NULL;
}

/**
 * siginfo_buildtime_checks - 信号信息结构体编译时检查
 *
 * 在编译时检查用户态和内核态siginfo结构体的兼容性，
 * 确保字段偏移量和大小匹配，包括32位兼容性检查
 */
static inline void siginfo_buildtime_checks(void)
{
	BUILD_BUG_ON(sizeof(struct siginfo) != SI_MAX_SIZE);

	/* Verify the offsets in the two siginfos match */
#define CHECK_OFFSET(field) \
	BUILD_BUG_ON(offsetof(siginfo_t, field) != offsetof(kernel_siginfo_t, field))

	/* kill */
	CHECK_OFFSET(si_pid);
	CHECK_OFFSET(si_uid);

	/* timer */
	CHECK_OFFSET(si_tid);
	CHECK_OFFSET(si_overrun);
	CHECK_OFFSET(si_value);

	/* rt */
	CHECK_OFFSET(si_pid);
	CHECK_OFFSET(si_uid);
	CHECK_OFFSET(si_value);

	/* sigchld */
	CHECK_OFFSET(si_pid);
	CHECK_OFFSET(si_uid);
	CHECK_OFFSET(si_status);
	CHECK_OFFSET(si_utime);
	CHECK_OFFSET(si_stime);

	/* sigfault */
	CHECK_OFFSET(si_addr);
	CHECK_OFFSET(si_addr_lsb);
	CHECK_OFFSET(si_lower);
	CHECK_OFFSET(si_upper);
	CHECK_OFFSET(si_pkey);

	/* sigpoll */
	CHECK_OFFSET(si_band);
	CHECK_OFFSET(si_fd);

	/* sigsys */
	CHECK_OFFSET(si_call_addr);
	CHECK_OFFSET(si_syscall);
	CHECK_OFFSET(si_arch);
#undef CHECK_OFFSET

	/* usb asyncio */
	BUILD_BUG_ON(offsetof(struct siginfo, si_pid) !=
		     offsetof(struct siginfo, si_addr));
	if (sizeof(int) == sizeof(void __user *)) {
		BUILD_BUG_ON(sizeof_field(struct siginfo, si_pid) !=
			     sizeof(void __user *));
	} else {
		BUILD_BUG_ON((sizeof_field(struct siginfo, si_pid) +
			      sizeof_field(struct siginfo, si_uid)) !=
			     sizeof(void __user *));
		BUILD_BUG_ON(offsetofend(struct siginfo, si_pid) !=
			     offsetof(struct siginfo, si_uid));
	}
#ifdef CONFIG_COMPAT
	BUILD_BUG_ON(offsetof(struct compat_siginfo, si_pid) !=
		     offsetof(struct compat_siginfo, si_addr));
	BUILD_BUG_ON(sizeof_field(struct compat_siginfo, si_pid) !=
		     sizeof(compat_uptr_t));
	BUILD_BUG_ON(sizeof_field(struct compat_siginfo, si_pid) !=
		     sizeof_field(struct siginfo, si_pid));
#endif
}

/**
 * signals_init - 初始化信号子系统
 *
 * 在内核启动时初始化信号子系统，进行编译时检查和创建信号队列缓存
 */
void __init signals_init(void)
{
	siginfo_buildtime_checks();  // 编译时检查siginfo结构体兼容性

	sigqueue_cachep = KMEM_CACHE(sigqueue, SLAB_PANIC);  // 创建信号队列对象缓存
}

#ifdef CONFIG_KGDB_KDB
#include <linux/kdb.h>
/*
 * kdb_send_sig - Allows kdb to send signals without exposing
 * signal internals.  This function checks if the required locks are
 * available before calling the main signal code, to avoid kdb
 * deadlocks.
 */
/**
 * kdb_send_sig - KDB调试器发送信号接口
 * @t: 目标进程
 * @sig: 信号编号
 *
 * 供KDB调试器使用的信号发送函数，在调试时避免死锁
 * 会检查锁的可用性和进程状态以确保安全发送信号
 */
void kdb_send_sig(struct task_struct *t, int sig)
{
	static struct task_struct *kdb_prev_t;
	int new_t, ret;
	if (!spin_trylock(&t->sighand->siglock)) {
		kdb_printf("Can't do kill command now.\n"
			   "The sigmask lock is held somewhere else in "
			   "kernel, try again later\n");
		return;
	}
	new_t = kdb_prev_t != t;
	kdb_prev_t = t;
	if (t->state != TASK_RUNNING && new_t) {
		spin_unlock(&t->sighand->siglock);
		kdb_printf("Process is not RUNNING, sending a signal from "
			   "kdb risks deadlock\n"
			   "on the run queue locks. "
			   "The signal has _not_ been sent.\n"
			   "Reissue the kill command if you want to risk "
			   "the deadlock.\n");
		return;
	}
	ret = send_signal(sig, SEND_SIG_PRIV, t, PIDTYPE_PID);
	spin_unlock(&t->sighand->siglock);
	if (ret)
		kdb_printf("Fail to deliver Signal %d to process %d.\n",
			   sig, t->pid);
	else
		kdb_printf("Signal %d is sent to process %d.\n", sig, t->pid);
}
#endif	/* CONFIG_KGDB_KDB */
