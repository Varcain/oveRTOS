/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "ove/ove.h"
uint32_t sync_binding_context_flags(void)
{
	return ove_sync_context();
}
int sync_binding_checked_wait(ove_condvar_t cv, ove_mutex_t mutex, uint64_t timeout, bool *held)
{
	return ove_condvar_wait_checked(cv, mutex, timeout, held);
}

int sync_binding_compile_consumer(void)
{
	ove_mutex_storage_t m = {0}, recursive = {0};
	ove_sem_storage_t sem = {0};
	ove_event_storage_t event = {0};
	ove_condvar_storage_t cv = {0};
	ove_mutex_t mh = NULL, rh = NULL;
	ove_sem_t sh = NULL;
	ove_event_t eh = NULL;
	ove_condvar_t ch = NULL;
	int status = 1;
	if (ove_mutex_init(&mh, &m))
		return 1;
	if (ove_recursive_mutex_init(&rh, &recursive))
		goto close_mutex;
	if (ove_sem_init(&sh, &sem, 0, 2))
		goto close_recursive;
	if (ove_event_init(&eh, &event))
		goto close_sem;
	if (ove_condvar_init(&ch, &cv))
		goto close_event;
	status = 0;
	ove_condvar_deinit(ch);
close_event:
	ove_event_deinit(eh);
close_sem:
	ove_sem_deinit(sh);
close_recursive:
	ove_recursive_mutex_deinit(rh);
close_mutex:
	ove_mutex_deinit(mh);
	return status;
}
void ove_main(void)
{
	ove_run();
}
