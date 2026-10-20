/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/* ── Scheduler-driven observed state (sched_note driver) ────────────
 *
 * CONFIG_SCHED_INSTRUMENTATION_SWITCH calls the registered note drivers'
 * suspend, ready and resume hooks inside the scheduler's critical section.
 * A wait shows as BLOCKED and a wake as READY until the task runs.
 *
 * The PID table holds only oveRTOS tasks registered by task_wrapper().
 * Critical sections protect the table and tracker transitions.
 */
#define OBS_MAX_TASKS 32

static struct {
	pid_t pid;
	struct ove_thread *t;
} obs_tasks[OBS_MAX_TASKS];
static bool driver_registered;

static void obs_transition(struct ove_thread *t, int s)
{
	int prev = t->st.cur_state;
	if (prev == s || prev == OVE_THREAD_STATE_TERMINATED)
		return;
	/* One clock read, shared by the trace record and the tracker. */
	uint64_t now = ove_state_stats_now_us();
	ove_trace_emit_state_at((uintptr_t)t, prev, s, now);
	ove_state_track_transition_at(&t->st, s, now);
}

static struct ove_thread *obs_find(pid_t pid)
{
	for (int i = 0; i < OBS_MAX_TASKS; i++)
		if (obs_tasks[i].t && obs_tasks[i].pid == pid)
			return obs_tasks[i].t;
	return NULL;
}

/* The ove_thread running as task @pid, or NULL (kernel / other tasks).
 * Used by trace descriptor enumeration for the shared thread identity. */
struct ove_thread *ove_nuttx_thread_by_pid(pid_t pid)
{
	irqstate_t flags = enter_critical_section();
	struct ove_thread *t = obs_find(pid);
	leave_critical_section(flags);
	return t;
}

static void obs_note_suspend(FAR struct note_driver_s *drv, FAR struct tcb_s *tcb)
{
	(void)drv;
	/* armv7-m/armv8-m arm_doirq() calls nxsched_switch_context(from, to)
	 * after EVERY interrupt, also when from == to.  A task that really
	 * leaves the CPU is never still RUNNING here (the scheduler has made
	 * it READYTORUN, INVALID on exit, or a wait state), so a RUNNING task
	 * did not switch: ignore the note and the matching resume is a
	 * no-op. */
	if (tcb->task_state == TSTATE_TASK_RUNNING)
		return;
	/* The stop note closes exiting tasks' lanes. */
	if (tcb->task_state == TSTATE_TASK_INVALID)
		return;
	struct ove_thread *t = obs_find(tcb->pid);

	int next;
	if (tcb->task_state == TSTATE_TASK_READYTORUN)
		next = OVE_THREAD_STATE_READY; /* preempted or yielded */
	else
		next = OVE_THREAD_STATE_BLOCKED;
	if (t)
		obs_transition(t, next);
	else
		ove_trace_emit_kthread_state((uintptr_t)tcb, next, OVE_TRACE_KT_ALWAYS);
}

static void obs_note_resume(FAR struct note_driver_s *drv, FAR struct tcb_s *tcb)
{
	(void)drv;
	struct ove_thread *t = obs_find(tcb->pid);
	if (t)
		obs_transition(t, OVE_THREAD_STATE_RUNNING);
	else
		ove_trace_emit_kthread_state((uintptr_t)tcb, OVE_THREAD_STATE_RUNNING,
					     OVE_TRACE_KT_ALWAYS);
}

static void obs_note_ready(FAR struct note_driver_s *drv, FAR struct tcb_s *tcb)
{
	(void)drv;
	struct ove_thread *t = obs_find(tcb->pid);
	if (t)
		obs_transition(t, OVE_THREAD_STATE_READY);
	else
		ove_trace_emit_kthread_state((uintptr_t)tcb, OVE_THREAD_STATE_READY,
					     OVE_TRACE_KT_FROM_WAITING);
}

/* Called inside a critical section. */
static void obs_unregister(struct ove_thread *t)
{
	for (int i = 0; i < OBS_MAX_TASKS; i++)
		if (obs_tasks[i].t == t)
			obs_tasks[i].t = NULL;
}

static void obs_close(struct ove_thread *t)
{
	irqstate_t flags = enter_critical_section();
	obs_transition(t, OVE_THREAD_STATE_TERMINATED);
	/* The stop note removes the entry before NuttX releases the PID. */
	if (!driver_registered)
		obs_unregister(t);
	leave_critical_section(flags);
}

static void obs_note_stop(FAR struct note_driver_s *drv, FAR struct tcb_s *tcb)
{
	(void)drv;
	/* nxtask_terminate() can raise the stop note outside its critical section. */
	irqstate_t flags = enter_critical_section();
	struct ove_thread *t = obs_find(tcb->pid);
	if (t) {
		obs_transition(t, OVE_THREAD_STATE_TERMINATED);
		obs_unregister(t);
	}
	/* The TCB may have claimed a kernel-thread slot before obs_register(). */
	ove_trace_emit_kthread_state((uintptr_t)tcb, OVE_THREAD_STATE_TERMINATED,
				     OVE_TRACE_KT_ALWAYS);
	leave_critical_section(flags);
}

static const struct note_driver_ops_s obs_note_ops = {
	.stop = obs_note_stop,
	.suspend = obs_note_suspend,
	.resume = obs_note_resume,
	.ready = obs_note_ready,
};

static struct note_driver_s obs_note_driver = {
	.ops = &obs_note_ops,
};

/* Called by the task itself on entry, before it marks itself RUNNING. */
static void obs_register(struct ove_thread *t)
{
	irqstate_t flags = enter_critical_section();
	if (!driver_registered) {
		/* No unregister API exists; one static driver for the system. */
		driver_registered = note_driver_register(&obs_note_driver) == OK;
	}
	for (int i = 0; i < OBS_MAX_TASKS; i++) {
		if (!obs_tasks[i].t) {
			obs_tasks[i].pid = getpid();
			obs_tasks[i].t = t;
			break;
		}
	}
	leave_critical_section(flags);
}
