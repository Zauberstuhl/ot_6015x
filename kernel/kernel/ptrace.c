/*
 * linux/kernel/ptrace.c
 *
 * (C) Copyright 1999 Linus Torvalds
 *
 * Common interfaces for "ptrace()" which we do not want
 * to continually duplicate across every architecture.
 */

#include <linux/capability.h>
#include <linux/export.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/ptrace.h>
#include <linux/security.h>
#include <linux/signal.h>
#include <linux/audit.h>
#include <linux/pid_namespace.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/regset.h>
#include <linux/hw_breakpoint.h>
#include <linux/cn_proc.h>


static int ptrace_trapping_sleep_fn(void *flags)
{
	schedule();
	return 0;
}

/*
 * ptrace a task: make the debugger its new parent and
 * move it to the ptrace list.
 *
 * Must be called with the tasklist lock write-held.
 */
void __ptrace_link(struct task_struct *child, struct task_struct *new_parent)
{
	BUG_ON(!list_empty(&child->ptrace_entry));
	list_add(&child->ptrace_entry, &new_parent->ptraced);
	child->parent = new_parent;
}

/**
 * __ptrace_unlink - unlink ptracee and restore its execution state
 * @child: ptracee to be unlinked
 *
 * Remove @child from the ptrace list, move it back to the original parent,
 * and restore the execution state so that it conforms to the group stop
 * state.
 *
 * Unlinking can happen via two paths - explicit PTRACE_DETACH or ptracer
 * exiting.  For PTRACE_DETACH, unless the ptracee has been killed between
 * ptrace_check_attach() and here, it's guaranteed to be in TASK_TRACED.
 * If the ptracer is exiting, the ptracee can be in any state.
 *
 * After detach, the ptracee should be in a state which conforms to the
 * group stop.  If the group is stopped or in the process of stopping, the
 * ptracee should be put into TASK_STOPPED; otherwise, it should be woken
 * up from TASK_TRACED.
 *
 * If the ptracee is in TASK_TRACED and needs to be moved to TASK_STOPPED,
 * it goes through TRACED -> RUNNING -> STOPPED transition which is similar
 * to but in the opposite direction of what happens while attaching to a
 * stopped task.  However, in this direction, the intermediate RUNNING
 * state is not hidden even from the current ptracer and if it immediately
 * re-attaches and performs a WNOHANG wait(2), it may fail.
 *
 * CONTEXT:
 * write_lock_irq(tasklist_lock)
 */
void __ptrace_unlink(struct task_struct *child)
{
	BUG_ON(!child->ptrace);

	child->ptrace = 0;
	child->parent = child->real_parent;
	list_del_init(&child->ptrace_entry);

	spin_lock(&child->sighand->siglock);

	/*
	 * Clear all pending traps and TRAPPING.  TRAPPING should be
	 * cleared regardless of JOBCTL_STOP_PENDING.  Do it explicitly.
	 */
	task_clear_jobctl_pending(child, JOBCTL_TRAP_MASK);
	task_clear_jobctl_trapping(child);

	/*
	 * Reinstate JOBCTL_STOP_PENDING if group stop is in effect and
	 * @child isn't dead.
	 */
	if (!(child->flags & PF_EXITING) &&
	    (child->signal->flags & SIGNAL_STOP_STOPPED ||
	     child->signal->group_stop_count)) {
		child->jobctl |= JOBCTL_STOP_PENDING;

		/*
		 * This is only possible if this thread was cloned by the
		 * traced task running in the stopped group, set the signal
		 * for the future reports.
		 * FIXME: we should change ptrace_init_task() to handle this
		 * case.
		 */
		if (!(child->jobctl & JOBCTL_STOP_SIGMASK))
			child->jobctl |= SIGSTOP;
	}

	/*
	 * If transition to TASK_STOPPED is pending or in TASK_TRACED, kick
	 * @child in the butt.  Note that @resume should be used iff @child
	 * is in TASK_TRACED; otherwise, we might unduly disrupt
	 * TASK_KILLABLE sleeps.
	 */
	if (child->jobctl & JOBCTL_STOP_PENDING || task_is_traced(child))
		signal_wake_up(child, task_is_traced(child));

	spin_unlock(&child->sighand->siglock);
}

/**
 * ptrace_check_attach - check whether ptracee is ready for ptrace operation
 * @child: ptracee to check for
 * @ignore_state: don't check whether @child is currently %TASK_TRACED
 *
 * Check whether @child is being ptraced by %current and ready for further
 * ptrace operations.  If @ignore_state is %false, @child also should be in
 * %TASK_TRACED state and on return the child is guaranteed to be traced
 * and not executing.  If @ignore_state is %true, @child can be in any
 * state.
 *
 * CONTEXT:
 * Grabs and releases tasklist_lock and @child->sighand->siglock.
 *
 * RETURNS:
 * 0 on success, -ESRCH if %child is not ready.
 */
int ptrace_check_attach(struct task_struct *child, bool ignore_state)
{
	int ret = -ESRCH;

	/*
	 * We take the read lock around doing both checks to close a
	 * possible race where someone else was tracing our child and
	 * detached between these two checks.  After this locked check,
	 * we are sure that this is our traced child and that can only
	 * be changed by us so it's not changing right after this.
	 */
	read_lock(&tasklist_lock);
	if ((child->ptrace & PT_PTRACED) && child->parent == current) {
		/*
		 * child->sighand can't be NULL, release_task()
		 * does ptrace_unlink() before __exit_signal().
		 */
		spin_lock_irq(&child->sighand->siglock);
		WARN_ON_ONCE(task_is_stopped(child));
		if (ignore_state || (task_is_traced(child) &&
				     !(child->jobctl & JOBCTL_LISTENING)))
			ret = 0;
		spin_unlock_irq(&child->sighand->siglock);
	}
	read_unlock(&tasklist_lock);

	if (!ret && !ignore_state)
		ret = wait_task_inactive(child, TASK_TRACED) ? 0 : -ESRCH;

	/* All systems go.. */
	return ret;
}

static int ptrace_has_cap(struct user_namespace *ns, unsigned int mode)
{
	if (mode & PTRACE_MODE_NOAUDIT)
		return has_ns_capability_noaudit(current, ns, CAP_SYS_PTRACE);
	else
		return has_ns_capability(current, ns, CAP_SYS_PTRACE);
}

int __ptrace_may_access(struct task_struct *task, unsigned int mode)
{
	const struct cred *cred = current_cred(), *tcred;

	/* May we inspect the given task?
	 * This check is used both for attaching with ptrace
	 * and for allowing access to sensitive information in /proc.
	 *
	 * ptrace_attach denies several cases that /proc allows
	 * because setting up the necessary parent/child relationship
	 * or halting the specified task is impossible.
	 */
	int dumpable = 0;
	/* Don't let security modules deny introspection */
	if (task == current)
		return 0;
	rcu_read_lock();
	tcred = __task_cred(task);
	if (cred->user->user_ns == tcred->user->user_ns &&
	    (cred->uid == tcred->euid &&
	     cred->uid == tcred->suid &&
	     cred->uid == tcred->uid  &&
	     cred->gid == tcred->egid &&
	     cred->gid == tcred->sgid &&
	     cred->gid == tcred->gid))
		goto ok;
	if (ptrace_has_cap(tcred->user->user_ns, mode))
		goto ok;
	rcu_read_unlock();
	return -EPERM;
ok:
	rcu_read_unlock();
	smp_rmb();
	if (task->mm)
		dumpable = get_dumpable(task->mm);
	if (!dumpable  && !ptrace_has_cap(task_user_ns(task), mode))
		return -EPERM;

	return security_ptrace_access_check(task, mode);
}

bool ptrace_may_access(struct task_struct *task, unsigned int mode)
{
	int err;
	task_lock(task);
	err = __ptrace_may_access(task, mode);
	task_unlock(task);
	return !err;
}

static int ptrace_attach(struct task_struct *task, long request,
			 unsigned long addr,
			 unsigned long flags)
{
	bool seize = (request == PTRACE_SEIZE);
	int retval;

	retval = -EIO;
	if (seize) {
		if (addr != 0)
			goto out;
		if (flags & ~(unsigned long)PTRACE_O_MASK)
			goto out;
		flags = PT_PTRACED | PT_SEIZED | (flags << PT_OPT_FLAG_SHIFT);
	} else {
		flags = PT_PTRACED;
	}

	audit_ptrace(task);

	retval = -EPERM;
	if (unlikely(task->flags & PF_KTHREAD))
		goto out;
	if (same_thread_group(task, current))
		goto out;

	/*
	 * Protect exec's credential calculations against our interference;
	 * SUID, SGID and LSM creds get determined differently
	 * under ptrace.
	 */
	retval = -ERESTARTNOINTR;
	if (mutex_lock_interruptible(&task->signal->cred_guard_mutex))
		goto out;

	task_lock(task);
	retval = __ptrace_may_access(task, PTRACE_MODE_ATTACH);
	task_unlock(task);
	if (retval)
		goto unlock_creds;

	write_lock_irq(&tasklist_lock);
	retval = -EPERM;
	if (unlikely(task->exit_state))
		goto unlock_tasklist;
	if (task->ptrace)
		goto unlock_tasklist;

	if (seize)
		flags |= PT_SEIZED;
	if (ns_capable(task_user_ns(task), CAP_SYS_PTRACE))
		flags |= PT_PTRACE_CAP;
	task->ptrace = flags;

	__ptrace_link(task, current);

	/* SEIZE doesn't trap tracee on attach */
	if (!seize)
		send_sig_info(SIGSTOP, SEND_SIG_FORCED, task);

	spin_lock(&task->sighand->siglock);

	/*
	 * If the task is already STOPPED, set JOBCTL_TRAP_STOP and
	 * TRAPPING, and kick it so that it transits to TRACED.  TRAPPING
	 * will be cleared if the child completes the transition or any
	 * event which clears the group stop states happens.  We'll wait
	 * for the transition to complete before returning from this
	 * function.
	 *
	 * This hides STOPPED -> RUNNING -> TRACED transition from the
	 * attaching thread but a different thread in the same group can
	 * still observe the transient RUNNING state.  IOW, if another
	 * thread's WNOHANG wait(2) on the stopped tracee races against
	 * ATTACH, the wait(2) may fail due to the transient RUNNING.
	 *
	 * The following task_is_stopped() test is safe as both transitions
	 * in and out of STOPPED are protected by siglock.
	 */
	if (task_is_stopped(task) &&
	    task_set_jobctl_pending(task, JOBCTL_TRAP_STOP | JOBCTL_TRAPPING))
		signal_wake_up(task, 1);

	spin_unlock(&task->sighand->siglock);

	retval = 0;
unlock_tasklist:
	write_unlock_irq(&tasklist_lock);
unlock_creds:
	mutex_unlock(&task->signal->cred_guard_mutex);
out:
	if (!retval) {
		wait_on_bit(&task->jobctl, JOBCTL_TRAPPING_BIT,
			    ptrace_trapping_sleep_fn, TASK_UNINTERRUPTIBLE);
		proc_ptrace_connector(task, PTRACE_ATTACH);
	}

	return retval;
}

/**
 * ptrace_traceme  --  helper for PTRACE_TRACEME
 *
 * Performs checks and sets PT_PTRACED.
 * Should be used by all ptrace implementations for PTRACE_TRACEME.
 */
static int ptrace_traceme(void)
{
	int ret = -EPERM;

	write_lock_irq(&tasklist_lock);
	/* Are we already being traced? */
	if (!current->ptrace) {
		ret = security_ptrace_traceme(current->parent);
		/*
		 * Check PF_EXITING to ensure ->real_parent has not passed
		 * exit_ptrace(). Otherwise we don't report the error but
		 * pretend ->real_parent untraces us right after return.
		 */
		if (!ret && !(current->real_parent->flags & PF_EXITING)) {
			current->ptrace = PT_PTRACED;
			__ptrace_link(current, current->real_parent);
		}
	}
	write_unlock_irq(&tasklist_lock);

	return ret;
}

/*
 * Called with irqs disabled, returns true if childs should reap themselves.
 */
static int ignoring_children(struct sighand_struct *sigh)
{
	int ret;
	spin_lock(&sigh->siglock);
	ret = (sigh->action[SIGCHLD-1].sa.sa_handler == SIG_IGN) ||
	      (sigh->action[SIGCHLD-1].sa.sa_flags & SA_NOCLDWAIT);
	spin_unlock(&sigh->siglock);
	return ret;
}

/*
 * Called with tasklist_lock held for writing.
 * Unlink a traced task, and clean it up if it was a traced zombie.
 * Return true if it needs to be reaped with release_task().
 * (We can't call release_task() here because we already hold tasklist_lock.)
 *
 * If it's a zombie, our attachedness prevented normal parent notification
 * or self-reaping.  Do notification now if it would have happened earlier.
 * If it should reap itself, return true.
 *
 * If it's our own child, there is no notification to do. But if our normal
 * children self-reap, then this child was prevented by ptrace and we must
 * reap it now, in that case we must also wake up sub-threads sleeping in
 * do_wait().
 */
static bool __ptrace_detach(struct task_struct *tracer, struct task_struct *p)
{
	bool dead;

	__ptrace_unlink(p);

	if (p->exit_state != EXIT_ZOMBIE)
		return false;

	dead = !thread_group_leader(p);

	if (!dead && thread_group_empty(p)) {
		if (!same_thread_group(p->real_parent, tracer))
			dead = do_notify_parent(p, p->exit_signal);
		else if (ignoring_children(tracer->sighand)) {
			__wake_up_parent(p, tracer);
			dead = true;
		}
	}
	/* Mark it as in the process of being reaped. */
	if (dead)
		p->exit_state = EXIT_DEAD;
	return dead;
}

static int ptrace_detach(struct task_struct *child, unsigned int data)
{
	bool dead = false;

	if (!valid_signal(data))
		return -EIO;

	/* Architecture-specific hardware disable .. */
	ptrace_disable(child);
	clear_tsk_thread_flag(child, TIF_SYSCALL_TRACE);

	write_lock_irq(&tasklist_lock);
	/*
	 * This child can be already killed. Make sure de_thread() or
	 * our sub-thread doing do_wait() didn't do release_task() yet.
	 */
	if (child->ptrace) {
		child->exit_code = data;
		dead = __ptrace_detach(current, child);
	}
	write_unlock_irq(&tasklist_lock);

	proc_ptrace_connector(child, PTRACE_DETACH);
	if (unlikely(dead))
		release_task(child);

	return 0;
}

/*
 * Detach all tasks we were using ptrace on. Called with tasklist held
 * for writing, and returns with it held too. But note it can release
 * and reacquire the lock.
 */
void exit_ptrace(struct task_struct *tracer)
	__releases(&tasklist_lock)
	__acquires(&tasklist_lock)
{
	struct task_struct *p, *n;
	LIST_HEAD(ptrace_dead);

	if (likely(list_empty(&tracer->ptraced)))
		return;

	list_for_each_entry_safe(p, n, &tracer->ptraced, ptrace_entry) {
		if (__ptrace_detach(tracer, p))
			list_add(&p->ptrace_entry, &ptrace_dead);
	}

	write_unlock_irq(&tasklist_lock);
	BUG_ON(!list_empty(&tracer->ptraced));

	list_for_each_entry_safe(p, n, &ptrace_dead, ptrace_entry) {
		list_del_init(&p->ptrace_entry);
		release_task(p);
	}

	write_lock_irq(&tasklist_lock);
}

int ptrace_readdata(struct task_struct *tsk, unsigned long src, char __user *dst, int len)
{
	int copied = 0;

	while (len > 0) {
		char buf[128];
		int this_len, retval;

		this_len = (len > sizeof(buf)) ? sizeof(buf) : len;
		retval = access_process_vm(tsk, src, buf, this_len, 0);
		if (!retval) {
			if (copied)
				break;
			return -EIO;
		}
		if (copy_to_user(dst, buf, retval))
			return -EFAULT;
		copied += retval;
		src += retval;
		dst += retval;
		len -= retval;
	}
	return copied;
}

int ptrace_writedata(struct task_struct *tsk, char __user *src, unsigned long dst, int len)
{
	int copied = 0;

	while (len > 0) {
		char buf[128];
		int this_len, retval;

		this_len = (len > sizeof(buf)) ? sizeof(buf) : len;
		if (copy_from_user(buf, src, this_len))
			return -EFAULT;
		retval = access_process_vm(tsk, dst, buf, this_len, 1);
		if (!retval) {
			if (copied)
				break;
			return -EIO;
		}
		copied += retval;
		src += retval;
		dst += retval;
		len -= retval;
	}
	return copied;
}

static int ptrace_setoptions(struct task_struct *child, unsigned long data)
{
	unsigned flags;

	if (data & ~(unsigned long)PTRACE_O_MASK)
		return -EINVAL;

	/* Avoid intermediate state when all opts are cleared */
	flags = child->ptrace;
	flags &= ~(PTRACE_O_MASK << PT_OPT_FLAG_SHIFT);
	flags |= (data << PT_OPT_FLAG_SHIFT);
	child->ptrace = flags;

	if (data & PTRACE_O_TRACESECCOMP)
		child->ptrace |= PT_TRACE_SECCOMP;

	return 0;
}

static int ptrace_getsiginfo(struct task_struct *child, siginfo_t *info)
{
	unsigned long flags;
	int error = -ESRCH;

	if (lock_task_sighand(child, &flags)) {
		error = -EINVAL;
		if (likely(child->last_siginfo != NULL)) {
			*info = *child->last_siginfo;
			error = 0;
		}
		unlock_task_sighand(child, &flags);
	}
	return error;
}

static int ptrace_setsiginfo(struct task_struct *child, const siginfo_t *info)
{
	unsigned long flags;
	int error = -ESRCH;

	if (lock_task_sighand(child, &flags)) {
		error = -EINVAL;
		if (likely(child->last_siginfo != NULL)) {
			*child->last_siginfo = *info;
			error = 0;
		}
		unlock_task_sighand(child, &flags);
	}
	return error;
}


#ifdef PTRACE_SINGLESTEP
#define is_singlestep(request)		((request) == PTRACE_SINGLESTEP)
#else
#define is_singlestep(request)		0
#endif

#ifdef PTRACE_SINGLEBLOCK
#define is_singleblock(request)		((request) == PTRACE_SINGLEBLOCK)
#else
#define is_singleblock(request)		0
#endif

#ifdef PTRACE_SYSEMU
#define is_sysemu_singlestep(request)	((request) == PTRACE_SYSEMU_SINGLESTEP)
#else
#define is_sysemu_singlestep(request)	0
#endif

static int ptrace_resume(struct task_struct *child, long request,
			 unsigned long data)
{
	if (!valid_signal(data))
		return -EIO;

	if (request == PTRACE_SYSCALL)
		set_tsk_thread_flag(child, TIF_SYSCALL_TRACE);
	else
		clear_tsk_thread_flag(child, TIF_SYSCALL_TRACE);

#ifdef TIF_SYSCALL_EMU
	if (request == PTRACE_SYSEMU || request == PTRACE_SYSEMU_SINGLESTEP)
		set_tsk_thread_flag(child, TIF_SYSCALL_EMU);
	else
		clear_tsk_thread_flag(child, TIF_SYSCALL_EMU);
#endif

	if (is_singleblock(request)) {
		if (unlikely(!arch_has_block_step()))
			return -EIO;
		user_enable_block_step(child);
	} else if (is_singlestep(request) || is_sysemu_singlestep(request)) {
		if (unlikely(!arch_has_single_step()))
			return -EIO;
		user_enable_single_step(child);
	} else {
		user_disable_single_step(child);
	}

	child->exit_code = data;
	wake_up_state(child, __TASK_TRACED);

	return 0;
}

#ifdef CONFIG_HAVE_ARCH_TRACEHOOK

static const struct user_regset *
find_regset(const struct user_regset_view *view, unsigned int type)
{
	const struct user_regset *regset;
	int n;

	for (n = 0; n < view->n; ++n) {
		regset = view->regsets + n;
		if (regset->core_note_type == type)
			return regset;
	}

	return NULL;
}

static int ptrace_regset(struct task_struct *task, int req, unsigned int type,
			 struct iovec *kiov)
{
	const struct user_regset_view *view = task_user_regset_view(task);
	const struct user_regset *regset = find_regset(view, type);
	int regset_no;

	if (!regset || (kiov->iov_len % regset->size) != 0)
		return -EINVAL;

	regset_no = regset - view->regsets;
	kiov->iov_len = min(kiov->iov_len,
			    (__kernel_size_t) (regset->n * regset->size));

	if (req == PTRACE_GETREGSET)
		return copy_regset_to_user(task, view, regset_no, 0,
					   kiov->iov_len, kiov->iov_base);
	else
		return copy_regset_from_user(task, view, regset_no, 0,
					     kiov->iov_len, kiov->iov_base);
}

#endif

int ptrace_request(struct task_struct *child, long request,
		   unsigned long addr, unsigned long data)
{
	bool seized = child->ptrace & PT_SEIZED;
	int ret = -EIO;
	siginfo_t siginfo, *si;
	void __user *datavp = (void __user *) data;
	unsigned long __user *datalp = datavp;
	unsigned long flags;

	switch (request) {
	case PTRACE_PEEKTEXT:
	case PTRACE_PEEKDATA:
		return generic_ptrace_peekdata(child, addr, data);
	case PTRACE_POKETEXT:
	case PTRACE_POKEDATA:
		return generic_ptrace_pokedata(child, addr, data);

#ifdef PTRACE_OLDSETOPTIONS
	case PTRACE_OLDSETOPTIONS:
#endif
	case PTRACE_SETOPTIONS:
		ret = ptrace_setoptions(child, data);
		break;
	case PTRACE_GETEVENTMSG:
		ret = put_user(child->ptrace_message, datalp);
		break;

	case PTRACE_GETSIGINFO:
		ret = ptrace_getsiginfo(child, &siginfo);
		if (!ret)
			ret = copy_siginfo_to_user(datavp, &siginfo);
		break;

	case PTRACE_SETSIGINFO:
		if (copy_from_user(&siginfo, datavp, sizeof siginfo))
			ret = -EFAULT;
		else
			ret = ptrace_setsiginfo(child, &siginfo);
		break;

	case PTRACE_INTERRUPT:
		/*
		 * Stop tracee without any side-effect on signal or job
		 * control.  At least one trap is guaranteed to happen
		 * after this request.  If @child is already trapped, the
		 * current trap is not disturbed and another trap will
		 * happen after the current trap is ended with PTRACE_CONT.
		 *
		 * The actual trap might not be PTRACE_EVENT_STOP trap but
		 * the pending condition is cleared regardless.
		 */
		if (unlikely(!seized || !lock_task_sighand(child, &flags)))
			break;

		/*
		 * INTERRUPT doesn't disturb existing trap sans one
		 * exception.  If ptracer issued LISTEN for the current
		 * STOP, this INTERRUPT should clear LISTEN and re-trap
		 * tracee into STOP.
		 */
		if (likely(task_set_jobctl_pending(child, JOBCTL_TRAP_STOP)))
			signal_wake_up(child, child->jobctl & JOBCTL_LISTENING);

		unlock_task_sighand(child, &flags);
		ret = 0;
		break;

	case PTRACE_LISTEN:
		/*
		 * Listen for events.  Tracee must be in STOP.  It's not
		 * resumed per-se but is not considered to be in TRACED by
		 * wait(2) or ptrace(2).  If an async event (e.g. group
		 * stop state change) happens, tracee will enter STOP trap
		 * again.  Alternatively, ptracer can issue INTERRUPT to
		 * finish listening and re-trap tracee into STOP.
		 */
		if (unlikely(!seized || !lock_task_sighand(child, &flags)))
			break;

		si = child->last_siginfo;
		if (likely(si && (si->si_code >> 8) == PTRACE_EVENT_STOP)) {
			child->jobctl |= JOBCTL_LISTENING;
			/*
			 * If NOTIFY is set, it means event happened between
			 * start of this trap and now.  Trigger re-trap.
			 */
			if (child->jobctl & JOBCTL_TRAP_NOTIFY)
				signal_wake_up(child, true);
			ret = 0;
		}
		unlock_task_sighand(child, &flags);
		break;

	case PTRACE_DETACH:	 /* detach a process that was attached. */
		ret = ptrace_detach(child, data);
		break;

#ifdef CONFIG_BINFMT_ELF_FDPIC
	case PTRACE_GETFDPIC: {
		struct mm_struct *mm = get_task_mm(child);
		unsigned long tmp = 0;

		ret = -ESRCH;
		if (!mm)
			break;

		switch (addr) {
		case PTRACE_GETFDPIC_EXEC:
			tmp = mm->context.exec_fdpic_loadmap;
			break;
		case PTRACE_GETFDPIC_INTERP:
			tmp = mm->context.interp_fdpic_loadmap;
			break;
		default:
			break;
		}
		mmput(mm);

		ret = put_user(tmp, datalp);
		break;
	}
#endif

#ifdef PTRACE_SINGLESTEP
	case PTRACE_SINGLESTEP:
#endif
#ifdef PTRACE_SINGLEBLOCK
	case PTRACE_SINGLEBLOCK:
#endif
#ifdef PTRACE_SYSEMU
	case PTRACE_SYSEMU:
	case PTRACE_SYSEMU_SINGLESTEP:
#endif
	case PTRACE_SYSCALL:
	case PTRACE_CONT:
		return ptrace_resume(child, request, data);

	case PTRACE_KILL:
		if (child->exit_state)	/* already dead */
			return 0;
		return ptrace_resume(child, request, SIGKILL);

#ifdef CONFIG_HAVE_ARCH_TRACEHOOK
	case PTRACE_GETREGSET:
	case PTRACE_SETREGSET:
	{
		struct iovec kiov;
		struct iovec __user *uiov = datavp;

		if (!access_ok(VERIFY_WRITE, uiov, sizeof(*uiov)))
			return -EFAULT;

		if (__get_user(kiov.iov_base, &uiov->iov_base) ||
		    __get_user(kiov.iov_len, &uiov->iov_len))
			return -EFAULT;

		ret = ptrace_regset(child, request, addr, &kiov);
		if (!ret)
			ret = __put_user(kiov.iov_len, &uiov->iov_len);
		break;
	}
#endif
	default:
		break;
	}

	return ret;
}

static struct task_struct *ptrace_get_task_struct(pid_t pid)
{
	struct task_struct *child;

	rcu_read_lock();
	child = find_task_by_vpid(pid);
	if (child)
		get_task_struct(child);
	rcu_read_unlock();

	if (!child)
		return ERR_PTR(-ESRCH);
	return child;
}

#ifndef arch_ptrace_attach
#define arch_ptrace_attach(child)	do { } while (0)
#endif

SYSCALL_DEFINE4(ptrace, long, request, long, pid, unsigned long, addr,
		unsigned long, data)
{
	struct task_struct *child;
	long ret;

	if (request == PTRACE_TRACEME) {
		ret = ptrace_traceme();
		if (!ret)
			arch_ptrace_attach(current);
		goto out;
	}

	child = ptrace_get_task_struct(pid);
	if (IS_ERR(child)) {
		ret = PTR_ERR(child);
		goto out;
	}

	if (request == PTRACE_ATTACH || request == PTRACE_SEIZE) {
		ret = ptrace_attach(child, request, addr, data);
		/*
		 * Some architectures need to do book-keeping after
		 * a ptrace attach.
		 */
		if (!ret)
			arch_ptrace_attach(child);
		goto out_put_task_struct;
	}

	ret = ptrace_check_attach(child, request == PTRACE_KILL ||
				  request == PTRACE_INTERRUPT);
	if (ret < 0)
		goto out_put_task_struct;

	ret = arch_ptrace(child, request, addr, data);

 out_put_task_struct:
	put_task_struct(child);
 out:
	return ret;
}

int generic_ptrace_peekdata(struct task_struct *tsk, unsigned long addr,
			    unsigned long data)
{
	unsigned long tmp;
	int copied;

	copied = access_process_vm(tsk, addr, &tmp, sizeof(tmp), 0);
	if (copied != sizeof(tmp))
		return -EIO;
	return put_user(tmp, (unsigned long __user *)data);
}

int generic_ptrace_pokedata(struct task_struct *tsk, unsigned long addr,
			    unsigned long data)
{
	int copied;

	copied = access_process_vm(tsk, addr, &data, sizeof(data), 1);
	return (copied == sizeof(data)) ? 0 : -EIO;
}

#if defined CONFIG_COMPAT
#include <linux/compat.h>

int compat_ptrace_request(struct task_struct *child, compat_long_t request,
			  compat_ulong_t addr, compat_ulong_t data)
{
	compat_ulong_t __user *datap = compat_ptr(data);
	compat_ulong_t word;
	siginfo_t siginfo;
	int ret;

	switch (request) {
	case PTRACE_PEEKTEXT:
	case PTRACE_PEEKDATA:
		ret = access_process_vm(child, addr, &word, sizeof(word), 0);
		if (ret != sizeof(word))
			ret = -EIO;
		else
			ret = put_user(word, datap);
		break;

	case PTRACE_POKETEXT:
	case PTRACE_POKEDATA:
		ret = access_process_vm(child, addr, &data, sizeof(data), 1);
		ret = (ret != sizeof(data) ? -EIO : 0);
		break;

	case PTRACE_GETEVENTMSG:
		ret = put_user((compat_ulong_t) child->ptrace_message, datap);
		break;

	case PTRACE_GETSIGINFO:
		ret = ptrace_getsiginfo(child, &siginfo);
		if (!ret)
			ret = copy_siginfo_to_user32(
				(struct compat_siginfo __user *) datap,
				&siginfo);
		break;

	case PTRACE_SETSIGINFO:
		memset(&siginfo, 0, sizeof siginfo);
		if (copy_siginfo_from_user32(
			    &siginfo, (struct compat_siginfo __user *) datap))
			ret = -EFAULT;
		else
			ret = ptrace_setsiginfo(child, &siginfo);
		break;
#ifdef CONFIG_HAVE_ARCH_TRACEHOOK
	case PTRACE_GETREGSET:
	case PTRACE_SETREGSET:
	{
		struct iovec kiov;
		struct compat_iovec __user *uiov =
			(struct compat_iovec __user *) datap;
		compat_uptr_t ptr;
		compat_size_t len;

		if (!access_ok(VERIFY_WRITE, uiov, sizeof(*uiov)))
			return -EFAULT;

		if (__get_user(ptr, &uiov->iov_base) ||
		    __get_user(len, &uiov->iov_len))
			return -EFAULT;

		kiov.iov_base = compat_ptr(ptr);
		kiov.iov_len = len;

		ret = ptrace_regset(child, request, addr, &kiov);
		if (!ret)
			ret = __put_user(kiov.iov_len, &uiov->iov_len);
		break;
	}
#endif

	default:
		ret = ptrace_request(child, request, addr, data);
	}

	return ret;
}

asmlinkage long compat_sys_ptrace(compat_long_t request, compat_long_t pid,
				  compat_long_t addr, compat_long_t data)
{
	struct task_struct *child;
	long ret;

	if (request == PTRACE_TRACEME) {
		ret = ptrace_traceme();
		goto out;
	}

	child = ptrace_get_task_struct(pid);
	if (IS_ERR(child)) {
		ret = PTR_ERR(child);
		goto out;
	}

	if (request == PTRACE_ATTACH || request == PTRACE_SEIZE) {
		ret = ptrace_attach(child, request, addr, data);
		/*
		 * Some architectures need to do book-keeping after
		 * a ptrace attach.
		 */
		if (!ret)
			arch_ptrace_attach(child);
		goto out_put_task_struct;
	}

	ret = ptrace_check_attach(child, request == PTRACE_KILL ||
				  request == PTRACE_INTERRUPT);
	if (!ret)
		ret = compat_arch_ptrace(child, request, addr, data);

 out_put_task_struct:
	put_task_struct(child);
 out:
	return ret;
}
#endif	/* CONFIG_COMPAT */

#ifdef CONFIG_HAVE_HW_BREAKPOINT
int ptrace_get_breakpoints(struct task_struct *tsk)
{
	if (atomic_inc_not_zero(&tsk->ptrace_bp_refcnt))
		return 0;

	return -1;
}

void ptrace_put_breakpoints(struct task_struct *tsk)
{
	if (atomic_dec_and_test(&tsk->ptrace_bp_refcnt))
		flush_ptrace_hw_breakpoint(tsk);
}
#endif /* CONFIG_HAVE_HW_BREAKPOINT */