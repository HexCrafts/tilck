/* SPDX-License-Identifier: BSD-2-Clause */

#include <tilck_gen_headers/config_mm.h>
#include <tilck_gen_headers/config_debug.h>

#include <tilck/common/basic_defs.h>
#include <tilck/common/utils.h>
#include <tilck/common/unaligned.h>

#include <tilck/kernel/sched.h>
#include <tilck/kernel/process.h>
#include <tilck/kernel/process_mm.h>
#include <tilck/kernel/process_int.h>
#include <tilck/kernel/kmalloc.h>
#include <tilck/kernel/worker_thread.h>
#include <tilck/kernel/debug_utils.h>
#include <tilck/kernel/hal.h>
#include <tilck/kernel/signal.h>
#include <tilck/kernel/errno.h>
#include <tilck/kernel/syscalls.h>
#include <tilck/kernel/paging_hw.h>
#include <tilck/kernel/irq.h>
#include <tilck/kernel/user.h>
#include <tilck/kernel/vdso.h>
#include <tilck/kernel/switch.h>

#include <tilck/mods/tracing.h>

void asm_trap_entry_resume(void);

STATIC_ASSERT(
   OFFSET_OF(struct task, fault_resume_regs) == TI_F_RESUME_RS_OFF
);
STATIC_ASSERT(
   OFFSET_OF(struct task, faults_resume_mask) == TI_FAULTS_MASK_OFF
);

STATIC_ASSERT(sizeof(struct task_and_process) <= 2048);

int setup_sig_handler(struct task *ti,
                      enum sig_state sig_state,
                      regs_t *r,
                      ulong user_func,
                      int signum)
{
   if (ti->nested_sig_handlers == 0) {

      int rc;

      if (sig_state == sig_pre_syscall)
         r->a0 = (ulong) -EINTR;

      if ((rc = save_regs_on_user_stack(r)) < 0)
         return rc;
   }

   regs_set_ip(r, user_func);
   regs_set_usersp(r,
                   regs_get_usersp(r) -
                   SIG_HANDLER_ALIGN_ADJUST -
                   sizeof(ulong));
   set_return_register(r, signum);
   set_return_addr(r, post_sig_handler_user_vaddr);
   ti->nested_sig_handlers++;

   ASSERT((regs_get_usersp(r) & (USERMODE_STACK_ALIGN - 1)) == 0);
   return 0;
}

NODISCARD int
kthread_create2(kthread_func_ptr func, const char *name, int fl, void *arg)
{
   struct task *ti;
   int tid, ret = -ENOMEM;
   ASSERT(name != NULL);

   regs_t r =  {
      .kernel_resume_pc = (ulong)&asm_trap_entry_resume,
      .sepc = (ulong)func,
      .sstatus = SR_SPIE | SR_SPP | SR_SIE | SR_SUM,
   };

   disable_preemption();

   tid = create_new_kernel_tid();

   if (tid < 0) {
      ret = -EAGAIN;
      goto end;
   }

   ti = allocate_new_thread(kernel_process->pi, tid, !!(fl & KTH_ALLOC_BUFS));

   if (!ti)
      goto end;

   ASSERT(is_kernel_thread(ti));

   if (*name == '&')
      name++;         /* see the macro kthread_create() */

   ti->kthread_name = name;
   ti->state = TASK_STATE_RUNNABLE;
   ti->running_in_kernel = true;
   task_info_reset_kernel_stack(ti);

   r.a0 = (ulong)arg;
   r.ra = (ulong)&kthread_exit;
   r.sp = (ulong)ti->state_regs;
   ti->state_regs = (void *)ti->state_regs - sizeof(regs_t);
   memcpy(ti->state_regs, &r, sizeof(r));

   ret = ti->tid;

   if (fl & KTH_WORKER_THREAD)
      ti->worker_thread = arg;

   /*
    * After the following call to add_task(), given that preemption is enabled,
    * there is NO GUARANTEE that the `tid` returned by this function will still
    * belong to a valid kernel thread. For example, the kernel thread might run
    * and terminate before the caller has the chance to run. Therefore, it is up
    * to the caller to be prepared for that.
    */

   add_task(ti);
   enable_preemption();

end:
   return ret; /* tid or error */
}

void
setup_usermode_task_regs(regs_t *r, void *entry, void *stack_addr)
{
   *r = (regs_t) {
      .kernel_resume_pc = (ulong)&asm_trap_entry_resume,
      .sepc = (ulong)entry,
      .sp = 0,
      .usersp = (ulong)stack_addr,
      .sstatus = (ulong)SR_SPIE | SR_SUM, /* User mode, enable interrupt */
   };
}

/*
 * Sched functions that are here because of arch-specific statements.
 */

static inline bool
is_fpu_enabled_for_task(struct task *ti)
{
   return get_task_arch_fields(ti)->fpu_regs &&
          (ti->state_regs->sstatus & SR_FS);
}

static inline void
save_curr_fpu_ctx_if_enabled(void)
{
   if (is_fpu_enabled_for_task(get_curr_task())) {
      save_current_fpu_regs(false);
   }
}


NORETURN void
switch_to_task(struct task *ti)
{
   /* Save the value of ti->state_regs as it will be reset below */
   regs_t *state = ti->state_regs;
   struct task *curr = get_curr_task();

   ASSERT(curr != NULL);

   if (UNLIKELY(ti != curr)) {
      ASSERT(curr->state != TASK_STATE_RUNNING);
      ASSERT_TASK_STATE(ti->state, TASK_STATE_RUNNABLE);
   }

   ASSERT(!is_preemption_enabled());
   switch_to_task_safety_checks(curr, ti);

   /* Do as much as possible work before disabling the interrupts */
   task_change_state_idempotent(ti, TASK_STATE_RUNNING);
   ti->ticks.timeslice = 0;

   if (!is_kernel_thread(curr) && curr->state != TASK_STATE_ZOMBIE)
      save_curr_fpu_ctx_if_enabled();

   if (!is_kernel_thread(ti)) {

      if (get_curr_pdir() != ti->pi->pdir) {
         set_curr_pdir(ti->pi->pdir);
      }

      if (!ti->running_in_kernel && !(state->sstatus & SR_SPP))
         process_signals(ti, sig_in_usermode, state);

      if (is_fpu_enabled_for_task(ti)) {
         restore_fpu_regs(ti, false);
      }
   }

   /* From here until the end, we have to be as fast as possible */
   disable_interrupts_forced();
   switch_to_task_pop_nested_interrupts();
   enable_preemption_nosched();
   ASSERT(is_preemption_enabled());

   if (!ti->running_in_kernel)
      task_info_reset_kernel_stack(ti);
   else
      adjust_nested_interrupts_for_task_in_kernel(ti);

   set_curr_task(ti);
   ti->timer_ready = false;

   context_switch(state);
}

bool
arch_specific_new_task_setup(struct task *ti, struct task *parent)
{
   arch_task_members_t *arch = get_task_arch_fields(ti);

   if (FORK_NO_COW) {

      if (parent) {

         /*
          * We parent is set, we're forking a task and we must NOT preserve the
          * arch fields. But, if we're not forking (parent is set), it means
          * we're in execve(): in that case there's no point to reset the arch
          * fields. Actually, here, in the NO_COW case, we MUST NOT do it, in
          * order to be sure we won't fail.
          */

         bzero(arch, sizeof(arch_task_members_t));
      }

      if (arch->fpu_regs) {

         /*
          * We already have an FPU regs buffer: just clear its contents and
          * keep it allocated.
          */
         bzero(arch->fpu_regs, arch->fpu_regs_size);

      } else {

         /* We don't have a FPU regs buffer: unless this is kthread, allocate */
         if (LIKELY(!is_kernel_thread(ti)))
            if (!allocate_fpu_regs(arch))
               return false; // out-of-memory
      }

   } else {

      /*
       * We're not in the NO_COW case. We have to free the arch specific fields
       * (like the fpu_regs buffer) if the parent is NULL. Otherwise, just reset
       * its members to zero.
       */

      if (parent) {
         bzero(arch, sizeof(*arch));
      } else {
         arch_specific_free_task(ti);
      }
   }

   return true;
}

void
arch_specific_free_task(struct task *ti)
{
   arch_task_members_t *arch = get_task_arch_fields(ti);
   kfree2(arch->fpu_regs, arch->fpu_regs_size);
   arch->fpu_regs = NULL;
   arch->fpu_regs_size = 0;
}

void
arch_specific_new_proc_setup(struct process *pi, struct process *parent)
{
   if (!parent)
      return;      /* we're done */

   pi->set_child_tid = NULL;
}

void
arch_specific_free_proc(struct process *pi)
{
   /* do nothing */
   return;
}

static void
handle_fatal_error(regs_t *r, int signum)
{
   send_signal(get_curr_tid(), signum, SIG_FL_PROCESS | SIG_FL_FAULT);
}

/* Access fault handler */
void handle_generic_fault_int(regs_t *r, const char *fault_name)
{
   if (!get_curr_task() || is_kernel_thread(get_curr_task()))
      panic("FAULT. Error: %s\n", fault_name);

   handle_fatal_error(r, SIGSEGV);
}

/* Instruction illegal fault handler */
void handle_inst_illegal_fault_int(regs_t *r, const char *fault_name)
{
   if (!get_curr_task() || is_kernel_thread(get_curr_task()))
      panic("FAULT. Error: %s\n", fault_name);

   handle_fatal_error(r, SIGILL);
}

/* Misaligned fault handler */
void handle_bus_fault_int(regs_t *r, const char *fault_name)
{
   if (!get_curr_task() || is_kernel_thread(get_curr_task()))
      panic("FAULT. Error: %s\n", fault_name);

   handle_fatal_error(r, SIGBUS);
}

