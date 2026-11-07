// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ove/sync.hpp"
#include "ove/app.hpp"
extern "C" int sync_binding_checked_wait(ove_condvar_t cv, ove_mutex_t mutex, uint64_t timeout,
					 bool *held)
{
	return ove_condvar_wait_checked(cv, mutex, timeout, held);
}

extern "C" int sync_binding_compile_consumer(void)
{
	ove::Mutex mutex;
	ove::RecursiveMutex recursive;
	ove::Semaphore semaphore{0, 2};
	ove::Event event;
	ove::CondVar cv;
	return 0;
}
extern "C" void ove_main(void)
{
	ove::run();
}
