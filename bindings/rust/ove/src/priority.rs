// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This file is part of oveRTOS.

//! Thread priority levels for oveRTOS.

/// Thread priority levels, matching `ove_prio_t`.
///
/// Variants are ordered from lowest (`Idle`) to highest (`Critical`) so that
/// standard comparison operators (`<`, `>`) work intuitively.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
#[repr(u32)]
pub enum Priority {
    /// Lowest priority, typically the RTOS idle task level.
    Idle = 0,
    /// Low background priority.
    Low = 1,
    /// Below-normal priority.
    BelowNormal = 2,
    /// Default priority for most application threads.
    Normal = 3,
    /// Above-normal priority for time-sensitive work.
    AboveNormal = 4,
    /// High priority for latency-critical tasks.
    High = 5,
    /// Real-time priority; preempts most other threads.
    Realtime = 6,
    /// Highest priority; reserved for system-critical tasks.
    Critical = 7,
}
