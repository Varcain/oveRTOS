/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/stream.h"
#include "ove/storage.h"
#include "ove_backend_common.h"
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <errno.h>
static void ms_to_abstime(uint32_t ms, struct timespec *ts)
{
  clock_gettime(CLOCK_REALTIME, ts);
  ts->tv_sec += ms / 1000;
  ts->tv_nsec += (ms % 1000) * 1000000L;
  if (ts->tv_nsec >= 1000000000L)
    {
      ts->tv_sec++;
      ts->tv_nsec -= 1000000000L;
    }
}

/* ─── _init / _deinit ────────────────────────────────────────────────── */

int ove_stream_init(ove_stream_t *stream,
                        ove_stream_storage_t *storage,
                        void *buffer, size_t size, size_t trigger)
{
  (void)trigger;

  if (stream == NULL || storage == NULL || buffer == NULL || size == 0)
    {
      return OVE_ERR_INVALID_PARAM;
    }

  storage->buffer = (unsigned char *)buffer;
  storage->size = size;
  storage->head = 0;
  storage->tail = 0;
  storage->count = 0;
  pthread_mutex_init(&storage->lock, NULL);
  pthread_cond_init(&storage->not_empty, NULL);
  pthread_cond_init(&storage->not_full, NULL);

  *stream = storage;
  return OVE_OK;
}

void ove_stream_deinit(ove_stream_t stream)
{
  if (stream != NULL)
    {
      struct ove_stream *ns = stream;
      pthread_cond_destroy(&ns->not_full);
      pthread_cond_destroy(&ns->not_empty);
      pthread_mutex_destroy(&ns->lock);
    }
}

/* ─── _create / _destroy ─────────────────────────────────────────────── */

#ifdef OVE_HEAP_STREAM
int ove_stream_create(ove_stream_t *stream, size_t size,
                                size_t trigger)
{
  struct ove_stream *ns;

  (void)trigger;

  if (stream == NULL || size == 0)
    {
      return OVE_ERR_INVALID_PARAM;
    }

  ns = OVE_BACKEND_MALLOC(sizeof(*ns));
  if (ns == NULL)
    {
      return OVE_ERR_NO_MEMORY;
    }

  ns->buffer = OVE_BACKEND_MALLOC(size);
  if (ns->buffer == NULL)
    {
      OVE_BACKEND_FREE(ns);
      return OVE_ERR_NO_MEMORY;
    }

  ns->size = size;
  ns->head = 0;
  ns->tail = 0;
  ns->count = 0;
  pthread_mutex_init(&ns->lock, NULL);
  pthread_cond_init(&ns->not_empty, NULL);
  pthread_cond_init(&ns->not_full, NULL);

  *stream = ns;
  return OVE_OK;
}

void ove_stream_destroy(ove_stream_t stream)
{
  if (stream != NULL)
    {
      struct ove_stream *ns = stream;
      pthread_cond_destroy(&ns->not_full);
      pthread_cond_destroy(&ns->not_empty);
      pthread_mutex_destroy(&ns->lock);
      OVE_BACKEND_FREE(ns->buffer);
      OVE_BACKEND_FREE(ns);
    }
}
#endif /* OVE_HEAP_STREAM */

/* ─── Operations ─────────────────────────────────────────────────────── */

int ove_stream_send(ove_stream_t stream,
                              const void *data, size_t len,
                              uint32_t timeout_ms,
                              size_t *bytes_sent)
{
  struct ove_stream *ns = stream;
  const unsigned char *src;
  size_t written = 0;

  if (ns == NULL)
    {
      return OVE_ERR_INVALID_PARAM;
    }

  src = (const unsigned char *)data;

  pthread_mutex_lock(&ns->lock);

  while (written < len)
    {
      while (ns->count >= ns->size)
        {
          if (timeout_ms == OVE_WAIT_FOREVER)
            {
              pthread_cond_wait(&ns->not_full, &ns->lock);
            }
          else
            {
              struct timespec ts;
              ms_to_abstime(timeout_ms, &ts);
              int ret = pthread_cond_timedwait(&ns->not_full,
                                               &ns->lock, &ts);
              if (ret != 0)
                {
                  goto out;
                }
            }
        }

      size_t avail = ns->size - ns->count;
      size_t chunk = len - written;
      if (chunk > avail)
        chunk = avail;

      size_t contig = ns->size - ns->head;
      if (contig < chunk)
        {
          memcpy(ns->buffer + ns->head, src + written, contig);
          memcpy(ns->buffer, src + written + contig, chunk - contig);
        }
      else
        {
          memcpy(ns->buffer + ns->head, src + written, chunk);
        }

      ns->head = (ns->head + chunk) % ns->size;
      ns->count += chunk;
      written += chunk;
    }

out:
  if (written > 0)
    {
      pthread_cond_signal(&ns->not_empty);
    }

  pthread_mutex_unlock(&ns->lock);

  if (bytes_sent != NULL)
    {
      *bytes_sent = written;
    }

  return OVE_OK;
}

int ove_stream_receive(ove_stream_t stream,
                                 void *buf, size_t len,
                                 uint32_t timeout_ms,
                                 size_t *bytes_received)
{
  struct ove_stream *ns = stream;
  unsigned char *dst;
  size_t read_bytes = 0;

  if (ns == NULL)
    {
      return OVE_ERR_INVALID_PARAM;
    }

  dst = (unsigned char *)buf;

  pthread_mutex_lock(&ns->lock);

  while (read_bytes < len)
    {
      while (ns->count == 0)
        {
          if (read_bytes > 0)
            {
              goto out;
            }

          if (timeout_ms == OVE_WAIT_FOREVER)
            {
              pthread_cond_wait(&ns->not_empty, &ns->lock);
            }
          else
            {
              struct timespec ts;
              ms_to_abstime(timeout_ms, &ts);
              int ret = pthread_cond_timedwait(&ns->not_empty,
                                               &ns->lock, &ts);
              if (ret != 0)
                {
                  goto out;
                }
            }
        }

      size_t avail = ns->count;
      size_t chunk = len - read_bytes;
      if (chunk > avail)
        chunk = avail;

      size_t contig = ns->size - ns->tail;
      if (contig < chunk)
        {
          memcpy(dst + read_bytes, ns->buffer + ns->tail, contig);
          memcpy(dst + read_bytes + contig, ns->buffer, chunk - contig);
        }
      else
        {
          memcpy(dst + read_bytes, ns->buffer + ns->tail, chunk);
        }

      ns->tail = (ns->tail + chunk) % ns->size;
      ns->count -= chunk;
      read_bytes += chunk;
    }

out:
  if (read_bytes > 0)
    {
      pthread_cond_signal(&ns->not_full);
    }

  pthread_mutex_unlock(&ns->lock);

  if (bytes_received != NULL)
    {
      *bytes_received = read_bytes;
    }

  return OVE_OK;
}

int ove_stream_send_from_isr(ove_stream_t stream,
                                       const void *data, size_t len,
                                       size_t *bytes_sent)
{
  struct ove_stream *ns = stream;
  const unsigned char *src = (const unsigned char *)data;
  size_t written = 0;

  pthread_mutex_lock(&ns->lock);

  while (written < len && ns->count < ns->size)
    {
      size_t avail = ns->size - ns->count;
      size_t chunk = len - written;
      if (chunk > avail)
        chunk = avail;

      size_t contig = ns->size - ns->head;
      if (contig < chunk)
        {
          memcpy(ns->buffer + ns->head, src + written, contig);
          memcpy(ns->buffer, src + written + contig, chunk - contig);
        }
      else
        {
          memcpy(ns->buffer + ns->head, src + written, chunk);
        }

      ns->head = (ns->head + chunk) % ns->size;
      ns->count += chunk;
      written += chunk;
    }

  if (written > 0)
    {
      pthread_cond_signal(&ns->not_empty);
    }

  pthread_mutex_unlock(&ns->lock);

  if (bytes_sent != NULL)
    {
      *bytes_sent = written;
    }

  return OVE_OK;
}

int ove_stream_receive_from_isr(ove_stream_t stream,
                                          void *buf, size_t len,
                                          size_t *bytes_received)
{
  struct ove_stream *ns = stream;
  unsigned char *dst = (unsigned char *)buf;
  size_t read_bytes = 0;

  pthread_mutex_lock(&ns->lock);

  while (read_bytes < len && ns->count > 0)
    {
      size_t avail = ns->count;
      size_t chunk = len - read_bytes;
      if (chunk > avail)
        chunk = avail;

      size_t contig = ns->size - ns->tail;
      if (contig < chunk)
        {
          memcpy(dst + read_bytes, ns->buffer + ns->tail, contig);
          memcpy(dst + read_bytes + contig, ns->buffer, chunk - contig);
        }
      else
        {
          memcpy(dst + read_bytes, ns->buffer + ns->tail, chunk);
        }

      ns->tail = (ns->tail + chunk) % ns->size;
      ns->count -= chunk;
      read_bytes += chunk;
    }

  if (read_bytes > 0)
    {
      pthread_cond_signal(&ns->not_full);
    }

  pthread_mutex_unlock(&ns->lock);

  if (bytes_received != NULL)
    {
      *bytes_received = read_bytes;
    }

  return OVE_OK;
}

int ove_stream_reset(ove_stream_t stream)
{
  struct ove_stream *ns = stream;

  pthread_mutex_lock(&ns->lock);
  ns->head = 0;
  ns->tail = 0;
  ns->count = 0;
  pthread_cond_signal(&ns->not_full);
  pthread_mutex_unlock(&ns->lock);

  return OVE_OK;
}

size_t ove_stream_bytes_available(ove_stream_t stream)
{
  struct ove_stream *ns = stream;
  size_t count;

  pthread_mutex_lock(&ns->lock);
  count = ns->count;
  pthread_mutex_unlock(&ns->lock);

  return count;
}
