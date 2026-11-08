// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

// Inserted inside the actual source module only in the host observer. Both
// adapters retain actual leases without ever dereferencing protected data.
#[allow(dead_code)]
pub(crate) struct Admission<'a, T: ?Sized> {
    #[cfg(ove_spin_parent)]
    old: SpinMutexGuard<'a, T>,
    #[cfg(not(ove_spin_parent))]
    lease: Lease<'a>,
    #[cfg(not(ove_spin_parent))]
    owner: core::marker::PhantomData<&'a SpinMutex<T>>,
}

pub(crate) fn admit<T: ?Sized>(owner: &SpinMutex<T>) -> Option<Admission<'_, T>> {
    #[cfg(ove_spin_parent)]
    {
        Some(Admission { old: owner.lock() })
    }
    #[cfg(not(ove_spin_parent))]
    {
        Lease::try_acquire(&owner.claimed).map(|lease| Admission {
            lease,
            owner: core::marker::PhantomData,
        })
    }
}

#[cfg(not(ove_spin_parent))]
pub(crate) fn claimed<T: ?Sized>(owner: &SpinMutex<T>) -> bool {
    owner.claimed.load(core::sync::atomic::Ordering::Acquire)
}
