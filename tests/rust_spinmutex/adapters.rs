// Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

// Inserted inside the actual source module only in the host observer. The
// adapters retain actual leases without ever dereferencing protected data.
#[allow(dead_code)]
pub(crate) struct Admission<'a, T: ?Sized> {
    lease: Lease<'a>,
    owner: core::marker::PhantomData<&'a SpinMutex<T>>,
}

pub(crate) fn admit<T: ?Sized>(owner: &SpinMutex<T>) -> Option<Admission<'_, T>> {
    Lease::try_acquire(&owner.claimed).map(|lease| Admission {
        lease,
        owner: core::marker::PhantomData,
    })
}

pub(crate) fn claimed<T: ?Sized>(owner: &SpinMutex<T>) -> bool {
    owner.claimed.load(core::sync::atomic::Ordering::Acquire)
}
