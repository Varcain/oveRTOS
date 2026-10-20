/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Host test for the guest side of the sim shared-memory event ring
 * (sim/src/ove_sim_transport_shm_guest.c): with no reader, or a slow one,
 * the guest must never overwrite unread events.  An event that does not
 * fit is refused with OVE_ERR_QUEUE_FULL and counted in event_dropped;
 * the free-space check survives the 32-bit wrap of the positions.
 */

#include <stddef.h>
#include <stdio.h>

#include "ove_sim_transport_shm_guest.c"

uint8_t fake_sim[FAKE_SIM_BYTES];
uint32_t fake_pos;

static int failures;

#define CHECK(cond)                                                                              \
	do {                                                                                     \
		if (!(cond)) {                                                                   \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                                              \
		}                                                                                \
	} while (0)

#define OFF(f) offsetof(struct sim_shm_header, f)
#define REC_BYTES 4096u /* length prefix + event */
#define REC_DATA (REC_BYTES - 2u - (uint32_t)sizeof(struct ove_sim_event))

static uint8_t evbuf[sizeof(struct ove_sim_event) + REC_BYTES];

static uint32_t hdr_u32(size_t off)
{
	uint32_t v;
	memcpy(&v, fake_sim + off, 4);
	return v;
}

static void set_hdr_u32(size_t off, uint32_t v)
{
	memcpy(fake_sim + off, &v, 4);
}

/* The ring byte at absolute position @pos. */
static uint8_t ring_byte(uint32_t pos)
{
	return fake_sim[SIM_SHM_EVENT_RING_OFF + (pos & (SIM_SHM_RING_SIZE - 1u))];
}

static const struct ove_sim_event *event(uint32_t data_len, uint8_t tag)
{
	struct ove_sim_event *e = (struct ove_sim_event *)evbuf;
	e->plugin_id = tag;
	e->event_type = 1;
	e->timestamp_ms = tag;
	e->data_len = data_len;
	memset(e->data, tag, data_len);
	return e;
}

static void open_fresh(struct ove_sim_transport *t)
{
	memset(fake_sim, 0xEE, sizeof(fake_sim)); /* a previous run's header */
	ove_sim_transport_shm_guest_create(t);
	t->ops->open(t, NULL);
}

/* No reader: records that fill the ring exactly; the rest are refused and
 * counted, and the first record is still intact. */
static void test_no_reader_never_overwrites(struct ove_sim_transport *t)
{
	const unsigned sent = 40, fit = SIM_SHM_RING_SIZE / REC_BYTES;
	unsigned refused = 0;

	open_fresh(t);
	CHECK(hdr_u32(OFF(event_dropped)) == 0);
	for (unsigned i = 0; i < sent; i++) {
		if (t->ops->send_event(t, event(REC_DATA, (uint8_t)(i + 1))) == OVE_ERR_QUEUE_FULL)
			refused++;
	}
	CHECK(hdr_u32(OFF(event_write_pos)) == SIM_SHM_RING_SIZE);
	CHECK(refused == sent - fit);
	CHECK(hdr_u32(OFF(event_dropped)) == sent - fit);
	CHECK(ring_byte(0) == ((REC_BYTES - 2u) & 0xFFu));
	CHECK(ring_byte(1) == ((REC_BYTES - 2u) >> 8));
	CHECK(ring_byte(2) == 1 && ring_byte(REC_BYTES - 1u) == 1);

	/* The host consumes one record: exactly one more fits. */
	set_hdr_u32(OFF(event_read_pos), REC_BYTES);
	CHECK(t->ops->send_event(t, event(REC_DATA, 0x77)) == OVE_OK);
	CHECK(t->ops->send_event(t, event(REC_DATA, 0x78)) == OVE_ERR_QUEUE_FULL);
	CHECK(hdr_u32(OFF(event_write_pos)) == SIM_SHM_RING_SIZE + REC_BYTES);
	CHECK(ring_byte(SIM_SHM_RING_SIZE + 2u) == 0x77);
	CHECK(ring_byte(REC_BYTES + 2u) == 2); /* the next unread record */

	/* A guest that reopens the transport (a reset) starts counting again:
	 * after one more refused event the count is 1, not sent - fit + 2. */
	t->ops->close(t);
	memset(fake_sim, 0xEE, sizeof(fake_sim));
	t->ops->open(t, NULL);
	CHECK(hdr_u32(OFF(event_dropped)) == 0);
	set_hdr_u32(OFF(event_read_pos), 500000u); /* host not resynced yet */
	CHECK(t->ops->send_event(t, event(10, 9)) == OVE_ERR_QUEUE_FULL);
	CHECK(hdr_u32(OFF(event_dropped)) == 1);
}

/* The positions are free-running and wrap at 2^32. */
static void test_positions_wrap(struct ove_sim_transport *t)
{
	const uint32_t start = 0xFFFFF000u;

	open_fresh(t);
	guest_priv_instance.event_wpos = start;
	set_hdr_u32(OFF(event_write_pos), start);
	set_hdr_u32(OFF(event_read_pos), start);
	for (unsigned i = 0; i < 20; i++)
		(void)t->ops->send_event(t, event(REC_DATA, (uint8_t)(i + 1)));
	CHECK((uint32_t)(hdr_u32(OFF(event_write_pos)) - start) == SIM_SHM_RING_SIZE);
}

/* Small events: the ring takes as many whole events as fit, never part of
 * one. */
static void test_small_events_fill_whole(struct ove_sim_transport *t)
{
	const unsigned sent = 10000;
	const uint32_t rec = 2u + (uint32_t)sizeof(struct ove_sim_event) + 100u;
	const unsigned fit = SIM_SHM_RING_SIZE / rec;

	open_fresh(t);
	for (unsigned i = 0; i < sent; i++)
		(void)t->ops->send_event(t, event(100, (uint8_t)i));
	CHECK(hdr_u32(OFF(event_write_pos)) == fit * rec);
	CHECK(hdr_u32(OFF(event_dropped)) == sent - fit);
}

/* A host read position ahead of the guest's write position (the host has
 * not resynchronised after a guest reset) is taken as a full ring. */
static void test_stale_read_position(struct ove_sim_transport *t)
{
	open_fresh(t);
	set_hdr_u32(OFF(event_read_pos), 500000u);
	CHECK(t->ops->send_event(t, event(10, 9)) == OVE_ERR_QUEUE_FULL);
	CHECK(hdr_u32(OFF(event_write_pos)) == 0);
}

int main(void)
{
	struct ove_sim_transport t;

	test_no_reader_never_overwrites(&t);
	test_positions_wrap(&t);
	test_small_events_fill_whole(&t);
	test_stale_read_position(&t);

	if (failures) {
		fprintf(stderr, "test_sim_shm_backpressure: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_sim_shm_backpressure: OK\n");
	return 0;
}
