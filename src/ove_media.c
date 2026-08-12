/* Engine-neutral removable-media ownership policy. */
#include "ove/media.h"

#define MEDIA_FS 0x00000001u
#define MEDIA_WRITER 0x00000002u
#define MEDIA_READER_ONE 0x00000004u

static unsigned int g_media_owners;
static uint32_t g_media_generation;
static unsigned int g_media_present;

void ove_media_observe(uint32_t generation, int present)
{
	/* A zero generation is reserved for "not observed yet". */
	if (generation == 0u)
		generation = 1u;
	__atomic_store_n(&g_media_generation, generation, __ATOMIC_RELEASE);
	__atomic_store_n(&g_media_present, present != 0, __ATOMIC_RELEASE);
}

void ove_media_removed(void)
{
	__atomic_store_n(&g_media_present, 0u, __ATOMIC_RELEASE);
}

int ove_media_fs_acquire(void)
{
	unsigned int state = __atomic_load_n(&g_media_owners, __ATOMIC_ACQUIRE);
	for (;;) {
		if ((state & (MEDIA_FS | MEDIA_WRITER)) != 0u)
			return OVE_ERR_BUSY;
		unsigned int desired = state | MEDIA_FS;
		if (__atomic_compare_exchange_n(&g_media_owners, &state, desired, 0,
						__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			return OVE_OK;
	}
}

void ove_media_fs_release(void)
{
	(void)__atomic_fetch_and(&g_media_owners, ~MEDIA_FS, __ATOMIC_ACQ_REL);
}

int ove_media_raw_acquire(struct ove_media_lease *lease, unsigned flags, uint32_t generation)
{
	if (!lease || lease->active || (flags & ~OVE_MEDIA_RAW_WRITE) != 0u || generation == 0u)
		return OVE_ERR_INVALID_PARAM;
	if (!__atomic_load_n(&g_media_present, __ATOMIC_ACQUIRE) ||
	    __atomic_load_n(&g_media_generation, __ATOMIC_ACQUIRE) != generation)
		return OVE_ERR_NOT_REGISTERED;

	unsigned int state = __atomic_load_n(&g_media_owners, __ATOMIC_ACQUIRE);
	for (;;) {
		unsigned int desired;
		if ((flags & OVE_MEDIA_RAW_WRITE) != 0u) {
			if (state != 0u)
				return OVE_ERR_BUSY;
			desired = MEDIA_WRITER;
		} else {
			if ((state & MEDIA_WRITER) != 0u || state > UINT32_MAX - MEDIA_READER_ONE)
				return OVE_ERR_BUSY;
			desired = state + MEDIA_READER_ONE;
		}
		if (__atomic_compare_exchange_n(&g_media_owners, &state, desired, 0,
						__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			break;
	}

	lease->generation = generation;
	lease->access = (uint8_t)flags;
	lease->active = 1u;
	if (!ove_media_raw_valid(lease, flags)) {
		ove_media_raw_release(lease);
		return OVE_ERR_NOT_REGISTERED;
	}
	return OVE_OK;
}

void ove_media_raw_release(struct ove_media_lease *lease)
{
	if (!lease || !lease->active)
		return;
	if ((lease->access & OVE_MEDIA_RAW_WRITE) != 0u) {
		(void)__atomic_fetch_and(&g_media_owners, ~MEDIA_WRITER, __ATOMIC_ACQ_REL);
	} else {
		unsigned int state = __atomic_load_n(&g_media_owners, __ATOMIC_ACQUIRE);
		while (state >= MEDIA_READER_ONE &&
		       !__atomic_compare_exchange_n(&g_media_owners, &state,
						    state - MEDIA_READER_ONE, 0, __ATOMIC_ACQ_REL,
						    __ATOMIC_ACQUIRE)) {
		}
	}
	lease->generation = 0u;
	lease->access = 0u;
	lease->active = 0u;
}

int ove_media_raw_valid(const struct ove_media_lease *lease, unsigned required_flags)
{
	if (!lease || !lease->active || (required_flags & ~OVE_MEDIA_RAW_WRITE) != 0u ||
	    (required_flags & OVE_MEDIA_RAW_WRITE) > (lease->access & OVE_MEDIA_RAW_WRITE))
		return 0;
	return __atomic_load_n(&g_media_present, __ATOMIC_ACQUIRE) != 0u &&
	       __atomic_load_n(&g_media_generation, __ATOMIC_ACQUIRE) == lease->generation;
}
