/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

#include "ove/watchdog.h"
#include "ove_backend_common.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <nuttx/timers/watchdog.h>

int ove_watchdog_create(ove_watchdog_t *wdt,
                                 uint32_t timeout_ms)
{
  struct ove_watchdog *nw;

  if (wdt == NULL || timeout_ms == 0)
    {
      return OVE_ERR_INVALID_PARAM;
    }

  nw = OVE_BACKEND_MALLOC(sizeof(*nw));
  if (nw == NULL)
    {
      return OVE_ERR_NO_MEMORY;
    }

  nw->fd = open("/dev/watchdog0", O_RDONLY);
  if (nw->fd < 0)
    {
      OVE_BACKEND_FREE(nw);
      return OVE_ERR_NOT_SUPPORTED;
    }

  nw->timeout_ms = timeout_ms;

  int ret = ioctl(nw->fd, WDIOC_SETTIMEOUT,
                  (unsigned long)timeout_ms);
  if (ret < 0)
    {
      close(nw->fd);
      OVE_BACKEND_FREE(nw);
      return OVE_ERR_NOT_SUPPORTED;
    }

  *wdt = nw;
  return OVE_OK;
}

void ove_watchdog_destroy(ove_watchdog_t wdt)
{
  if (wdt != NULL)
    {
      struct ove_watchdog *nw = wdt;
      close(nw->fd);
      OVE_BACKEND_FREE(nw);
    }
}

int ove_watchdog_start(ove_watchdog_t wdt)
{
  struct ove_watchdog *nw = wdt;
  int ret = ioctl(nw->fd, WDIOC_START, 0);
  return (ret == 0) ? OVE_OK : OVE_ERR_NOT_SUPPORTED;
}

int ove_watchdog_stop(ove_watchdog_t wdt)
{
  struct ove_watchdog *nw = wdt;
  int ret = ioctl(nw->fd, WDIOC_STOP, 0);
  return (ret == 0) ? OVE_OK : OVE_ERR_NOT_SUPPORTED;
}

int ove_watchdog_feed(ove_watchdog_t wdt)
{
  struct ove_watchdog *nw = wdt;
  int ret = ioctl(nw->fd, WDIOC_KEEPALIVE, 0);
  return (ret == 0) ? OVE_OK : OVE_ERR_NOT_SUPPORTED;
}
