/* SPDX-License-Identifier: BSD-2-Clause */

#include <tilck_gen_headers/mod_tracing.h>

#include <tilck/common/basic_defs.h>
#include <tilck/common/string_util.h>
#include <tilck/common/utils.h>

#include <tilck/kernel/process.h>
#include <tilck/kernel/signal.h>
#include <tilck/kernel/errno.h>
#include <tilck/kernel/user.h>
#include <tilck/kernel/syscalls.h>
#include <tilck/kernel/sys_types.h>

#include <tilck/mods/tracing.h>

typedef void (*action_type)(struct task *, int signum);

static void add_pending_sig(struct task *ti, int signum)
{
   ASSERT(signum > 0);
   signum--;

   int slot = signum / NBITS;
   int index = signum % NBITS;

   if (slot >= K_SIGACTION_MASK_WORDS)
      return; /* just silently ignore signals that we don't support */

   ti->pending_signums[slot] |= (1 << index);
}

static void del_pending_sig(struct task *ti, int signum)
{
   ASSERT(signum > 0);
   signum--;

   int slot = signum / NBITS;
   int index = signum % NBITS;

   if (slot >= K_SIGACTION_MASK_WORDS)
      return; /* just silently ignore signals that we don't support */

   ti->pending_signums[slot] &= ~(1 << index);
}

static bool is_pending_sig(struct task *ti, int signum)
{
   ASSERT(signum > 0);
   signum--;

   int slot = signum / NBITS;
   int index = signum % NBITS;

   if (slot >= K_SIGACTION_MASK_WORDS)
      return false; /* just silently ignore signals that we don't support */

   return !!(ti->pending_signums[slot] & (1 << index));
}

static int get_first_pending_sig(struct task *ti)
{
   for (u32 i = 0; i < K_SIGACTION_MASK_WORDS; i++) {

      ulong val = ti->pending_signums[i];

      if (val != 0) {
         u32 idx = get_first_set_bit_index_l(val);
         u32 signum = i * NBITS + idx + 1;
         return (int)signum;
      }
   }

   return -1;
}

bool process_signals(void)
{
   ASSERT(!is_preemption_enabled());
   struct task *curr = get_curr_task();
   int sig;

   sig = get_first_pending_sig(curr);

   if (sig > 0) {
      trace_signal_delivered(curr->tid, sig);
      enable_preemption();
      terminate_process(0, sig);
      NOT_REACHED();
   }

   return false;
}

static void action_terminate(struct task *ti, int signum)
{
   ASSERT(!is_preemption_enabled());
   ASSERT(!is_kernel_thread(ti));

   if (ti == get_curr_task()) {

      enable_preemption();
      ASSERT(is_preemption_enabled());

      terminate_process(0, signum);
      NOT_REACHED();
   }

   add_pending_sig(ti, signum);

   if (!ti->vfork_stopped) {

      if (ti->state == TASK_STATE_SLEEPING) {

         /*
          * We must NOT wake up tasks waiting on a mutex or on a semaphore:
          * supporting spurious wake-ups there is just a waste of resources.
          * On the contrary, if a task is waiting on a condition or sleeping
          * in kernel_sleep(), we HAVE to wake it up.
          */

         if (ti->wobj.type != WOBJ_KMUTEX && ti->wobj.type != WOBJ_SEM)
            task_change_state(ti, TASK_STATE_RUNNABLE);
      }


      ti->stopped = false;

   } else {

      /*
       * The task is vfork_stopped: we cannot make it runnable, nor kill it
       * right now. Just registering the signal as pending is enough. As soon
       * as the process wakes up, the killing signal will be delivered.
       * Supporting the killing a of vforked process (while its child is still
       * alive and has not called execve()) is just too tricky.
       *
       * TODO: consider supporting killing of vforked process.
       */
   }
}

static void action_ignore(struct task *ti, int signum)
{
   if (ti->tid == 1) {
      printk(
         "WARNING: ignoring signal %s[%d] sent to init (pid 1)\n",
         get_signal_name(signum), signum
      );
   }
}

static void action_stop(struct task *ti, int signum)
{
   ASSERT(!is_kernel_thread(ti));

   trace_signal_delivered(ti->tid, signum);
   ti->stopped = true;
   ti->wstatus = STOPCODE(signum);
   wake_up_tasks_waiting_on(ti, task_stopped);

   if (ti == get_curr_task()) {
      kernel_yield_preempt_disabled();
   }
}

static void action_continue(struct task *ti, int signum)
{
   ASSERT(!is_kernel_thread(ti));

   if (ti->vfork_stopped)
      return;

   trace_signal_delivered(ti->tid, signum);
   ti->stopped = false;
   ti->wstatus = CONTINUED;
   wake_up_tasks_waiting_on(ti, task_continued);
}

static const action_type signal_default_actions[_NSIG] =
{
   [SIGHUP] = action_terminate,
   [SIGINT] = action_terminate,
   [SIGQUIT] = action_terminate,
   [SIGILL] = action_terminate,
   [SIGABRT] = action_terminate,
   [SIGFPE] = action_terminate,
   [SIGKILL] = action_terminate,
   [SIGSEGV] = action_terminate,
   [SIGPIPE] = action_terminate,
   [SIGALRM] = action_terminate,
   [SIGTERM] = action_terminate,
   [SIGUSR1] = action_terminate,
   [SIGUSR2] = action_terminate,

   [SIGCHLD] = action_ignore,
   [SIGCONT] = action_continue,
   [SIGSTOP] = action_stop,
   [SIGTSTP] = action_stop,
   [SIGTTIN] = action_stop,
   [SIGTTOU] = action_stop,

   [SIGBUS] = action_terminate,
   [SIGPOLL] = action_terminate,
   [SIGPROF] = action_terminate,
   [SIGSYS] = action_terminate,
   [SIGTRAP] = action_terminate,

   [SIGURG] = action_ignore,

   [SIGVTALRM] = action_terminate,
   [SIGXCPU] = action_terminate,
   [SIGXFSZ] = action_terminate,
};

static void do_send_signal(struct task *ti, int signum)
{
   ASSERT(IN_RANGE(signum, 0, _NSIG));

   if (signum == 0) {

      /*
       * Do nothing, but don't treat it as an error.
       *
       * From kill(2):
       *    If sig is 0, then no signal is sent, but error checking is still
       *    performed; this can be used to check for the existence of a
       *    process ID or process group ID.
       */
      return;
   }

   __sighandler_t h = ti->pi->sa_handlers[signum - 1];

   if (h == SIG_IGN)
      return action_ignore(ti, signum);

   if (h != SIG_DFL) {

      /* DIRTY HACK: treat custom signal handlers as SIG_IGN */
      return;
   }

   /*
    * For the moment, we don't support anything else than SIG_DFL and SIG_IGN.
    * TODO: actually support custom signal handlers.
    */
   ASSERT(h == SIG_DFL);

   action_type action_func =
      signal_default_actions[signum] != NULL
         ? signal_default_actions[signum]
         : action_terminate;

   if (!action_func)
      return; /* unknown signal, just do nothing */

   action_func(ti, signum);
}

int send_signal2(int pid, int tid, int signum, bool whole_process)
{
   struct task *ti;
   int rc = -ESRCH;

   disable_preemption();

   if (!(ti = get_task(tid)))
      goto err_end;

   if (is_kernel_thread(ti))
      goto err_end; /* cannot send signals to kernel threads */

   /* When `whole_process` is true, tid must be == pid */
   if (whole_process && ti->pi->pid != tid)
      goto err_end;

   if (ti->pi->pid != pid)
      goto err_end;

   if (signum == 0)
      goto end; /* the user app is just checking permissions */

   if (ti->state == TASK_STATE_ZOMBIE)
      goto end; /* do nothing */

   /* TODO: update this code when thread support is added */
   do_send_signal(ti, signum);

end:
   rc = 0;

err_end:
   enable_preemption();
   return rc;
}

/*
 * -------------------------------------
 * SYSCALLS
 * -------------------------------------
 */

static int
sigaction_int(int signum, const struct k_sigaction *user_act)
{
   struct task *curr = get_curr_task();
   struct k_sigaction act;

   if (copy_from_user(&act, user_act, sizeof(act)) != 0)
      return -EFAULT;

   if (act.sa_flags & SA_NOCLDSTOP) {
      return -EINVAL; /* not supported */
   }

   if (act.sa_flags & SA_NOCLDWAIT) {
      return -EINVAL; /* not supported */
   }

   if (act.sa_flags & SA_SIGINFO) {
      return -EINVAL; /* not supported */
   }

   if (act.sa_flags & SA_ONSTACK) {
      return -EINVAL; /* not supported */
   }

   if (act.sa_flags & SA_RESETHAND) {
      /* TODO: add support for this simple flag */
   }

   if (act.sa_flags & SA_NODEFER) {

      /*
       * Just ignore this. For the moment, Tilck will block the delivery of
       * signals with custom handlers, if ANY signal handler is running.
       */
   }

   if (act.sa_flags & SA_RESTART) {
      /* For the moment, silently signore this important flag too. */
   }

   curr->pi->sa_handlers[signum - 1] = act.handler;
   return 0;
}

int
sys_rt_sigaction(int signum,
                 const struct k_sigaction *user_act,
                 struct k_sigaction *user_oldact,
                 size_t sigsetsize)
{
   struct task *curr = get_curr_task();
   struct k_sigaction oldact;
   int rc = 0;

   if (!IN_RANGE(signum, 1, _NSIG))
      return -EINVAL;

   if (signum == SIGKILL || signum == SIGSTOP)
      return -EINVAL;

   if (sigsetsize != sizeof(user_act->sa_mask))
      return -EINVAL;

   disable_preemption();
   {
      if (user_oldact != NULL) {

         oldact = (struct k_sigaction) {
            .handler = curr->pi->sa_handlers[signum - 1],
            .sa_flags = 0,
         };

         memcpy(oldact.sa_mask, curr->pi->sa_mask, sizeof(oldact.sa_mask));
      }

      if (user_act != NULL) {
         rc = sigaction_int(signum, user_act);
      }
   }
   enable_preemption();

   if (!rc && user_oldact != NULL) {

      if (copy_to_user(user_oldact, &oldact, sizeof(oldact)) != 0)
         rc = -EFAULT;
   }

   return rc;
}

int
sys_rt_sigprocmask(int how,
                   sigset_t *user_set,
                   sigset_t *user_oldset,
                   size_t sigsetsize)
{
   struct process *pi = get_curr_proc();
   int rc;

   if (user_oldset) {

      rc = copy_to_user(user_oldset, pi->sa_mask, sigsetsize);

      if (rc)
         return -EFAULT;

      if (sigsetsize > sizeof(pi->sa_mask)) {

         const size_t diff = sigsetsize - sizeof(pi->sa_mask);

         rc = copy_to_user(
            (char *)user_oldset + sizeof(pi->sa_mask),
            zero_page,
            diff
         );

         if (rc)
            return -EFAULT;
      }
   }

   if (user_set) {

      for (u32 i = 0; i < K_SIGACTION_MASK_WORDS; i++) {

         ulong w = 0;

         rc = copy_from_user(
            &w,
            (char *)user_set + i * sizeof(ulong),
            sizeof(ulong)
         );

         if (rc)
            return -EFAULT;

         switch (how) {

            case SIG_BLOCK:
               pi->sa_mask[i] |= w;
               break;

            case SIG_UNBLOCK:
               pi->sa_mask[i] &= ~w;
               break;

            case SIG_SETMASK:
               pi->sa_mask[i] = w;
               break;

            default:
               return -EINVAL;
         }
      }
   }

   return 0;
}

int sys_sigprocmask(ulong a1, ulong a2, ulong a3)
{
   NOT_IMPLEMENTED(); // deprecated interface
}

int sys_sigaction(ulong a1, ulong a2, ulong a3)
{
   NOT_IMPLEMENTED(); // deprecated interface
}

__sighandler_t sys_signal(int signum, __sighandler_t handler)
{
   NOT_IMPLEMENTED(); // deprecated interface
}
