/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "model.h"
/* Distinct names avoid collisions with libc's real, unused pthread types. */
#define pthread_mutex_t model_mutex_t
#define pthread_mutexattr_t model_attr_t
#define pthread_once_t model_once_t
typedef unsigned model_mutex_t, model_attr_t, model_once_t;
#define PTHREAD_ONCE_INIT 0
#define PTHREAD_MUTEX_RECURSIVE 1
#define pthread_mutexattr_init(p) ((void)(p), model_effect())
#define pthread_mutexattr_settype(p, t) ((void)(p), (void)(t), model_effect())
#define pthread_mutexattr_destroy(p) ((void)(p), model_effect())
#define pthread_mutex_init(p, a) ((void)(p), (void)(a), model_effect())
#define pthread_mutex_lock(p) ((void)(p), model_effect())
#define pthread_mutex_unlock(p) ((void)(p), model_effect())
#define pthread_once(p, f) ((void)(p), (void)(f), model_effect())
