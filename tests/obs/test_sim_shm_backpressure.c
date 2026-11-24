/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of oveRTOS.
 */

/*
 * Host tests for guest event backpressure and the shared-memory audio layout.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include <cmocka.h>

#include "ove_sim_transport_shm_guest.c"

uint8_t fake_sim[FAKE_SIM_BYTES];
uint8_t fake_audio[FAKE_AUDIO_BYTES];
uint32_t fake_pos[2];
bool fake_fail_next_read;

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

static uint32_t read_u32(const uint8_t *src)
{
	uint32_t v;
	memcpy(&v, src, sizeof(v));
	return v;
}

static uint32_t hdr_u32(size_t off)
{
	return read_u32(fake_sim + off);
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
	memset(fake_audio, 0, sizeof(fake_audio));
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

static void test_event_backpressure(void **state)
{
	struct ove_sim_transport t;

	test_no_reader_never_overwrites(&t);
	test_positions_wrap(&t);
	test_small_events_fill_whole(&t);
	test_stale_read_position(&t);
	assert_int_equal(failures, 0);
}

/* Wire offsets from the host's shm.py. */
enum { OUT_PCM = 32, IN_HEADER = 65568, IN_PCM = 65600, PCM_CAPACITY = 65536 };
static const uint8_t playback[] = {1, 2, 3, 4, 5, 6, 7, 8};
static const uint8_t capture[] = {8, 7, 6, 5, 4, 3, 2, 1};

static void test_first_audio_push(void **state)
{
	struct ove_sim_transport t;
	open_fresh(&t);
	assert_int_equal(t.ops->push_audio(&t, playback, sizeof(playback), 48000, 2, 16), OVE_OK);
	assert_int_equal(read_u32(fake_audio), sizeof(playback));
	assert_int_equal(read_u32(fake_audio + 8), 48000);
	assert_memory_equal(fake_audio + 12, "\x02\x00", 2);
	assert_memory_equal(fake_audio + 14, "\x10\x00", 2);
	assert_int_equal(read_u32(fake_audio + 16), PCM_CAPACITY);
	assert_memory_equal(fake_audio + OUT_PCM, playback, sizeof(playback));
}

static void pull_queued_audio(struct ove_sim_transport *t, uint32_t read_pos)
{
	uint32_t write_pos = read_pos + sizeof(capture);
	uint8_t pcm[sizeof(capture)];
	memcpy(fake_audio + IN_PCM + read_pos, capture, sizeof(capture));
	memcpy(fake_audio + IN_HEADER, &write_pos, sizeof(write_pos));
	memcpy(fake_audio + IN_HEADER + 4, &read_pos, sizeof(read_pos));
	assert_int_equal(t->ops->pull_audio(t, pcm, sizeof(pcm)), sizeof(capture));
	assert_memory_equal(pcm, capture, sizeof(capture));
	assert_int_equal(read_u32(fake_audio + IN_HEADER), write_pos);
	assert_int_equal(read_u32(fake_audio + IN_HEADER + 4), write_pos);
	assert_int_equal(read_u32(fake_audio + IN_HEADER + 16), PCM_CAPACITY);
}

static void test_host_audio_pull(void **state)
{
	struct ove_sim_transport t;
	open_fresh(&t);
	assert_int_equal(t.ops->push_audio(&t, playback, sizeof(playback), 48000, 2, 16), OVE_OK);
	pull_queued_audio(&t, 0);
}

static void test_audio_pull_after_failed_init_read(void **state)
{
	uint8_t pcm[sizeof(capture)];
	struct ove_sim_transport t;
	open_fresh(&t);
	fake_fail_next_read = true; /* The input read position during init. */
	assert_int_equal(t.ops->pull_audio(&t, pcm, sizeof(pcm)), 0);
	assert_false(fake_fail_next_read);
	pull_queued_audio(&t, 0);
}

static void test_pull_before_push(void **state)
{
	struct ove_sim_transport t;
	open_fresh(&t);
	pull_queued_audio(&t, 16);
	assert_int_equal(read_u32(fake_audio + 8), 0);
	assert_int_equal(t.ops->push_audio(&t, playback, sizeof(playback), 44100, 2, 32), OVE_OK);
	assert_int_equal(read_u32(fake_audio + 8), 44100);
	assert_memory_equal(fake_audio + 12, "\x02\x00", 2);
	assert_memory_equal(fake_audio + 14, "\x20\x00", 2);
}

static void test_audio_push_wrap(void **state)
{
	static const uint8_t fill[PCM_CAPACITY - 4];
	uint8_t input_header[32];
	struct ove_sim_transport t;
	open_fresh(&t);
	assert_int_equal(t.ops->push_audio(&t, fill, sizeof(fill), 48000, 2, 16), OVE_OK);
	memcpy(input_header, fake_audio + IN_HEADER, sizeof(input_header));
	/* The host still has all of the playback queued. */
	assert_int_equal(t.ops->push_audio(&t, playback, sizeof(playback), 48000, 2, 16), OVE_OK);
	assert_memory_equal(fake_audio + IN_HEADER - 4, playback, 4);
	assert_memory_equal(fake_audio + OUT_PCM, playback + 4, 4);
	assert_int_equal(read_u32(fake_audio), sizeof(fill) + sizeof(playback));
	assert_int_equal(read_u32(fake_audio + 4), 0);
	assert_memory_equal(fake_audio + IN_HEADER, input_header, sizeof(input_header));
}

static void test_audio_push_exceeds_capacity(void **state)
{
	static const uint8_t fill[2 * PCM_CAPACITY + 1];
	uint8_t input_header[32];
	struct ove_sim_transport t;
	open_fresh(&t);
	assert_int_equal(t.ops->push_audio(&t, playback, sizeof(playback), 48000, 2, 16), OVE_OK);
	memset(input_header, 0xA5, sizeof(input_header));
	memcpy(fake_audio + IN_HEADER, input_header, sizeof(input_header));
	assert_int_equal(t.ops->push_audio(&t, fill, PCM_CAPACITY + 1, 48000, 2, 16), OVE_OK);
	assert_int_equal(read_u32(fake_audio), sizeof(playback));
	assert_int_equal(t.ops->push_audio(&t, fill, sizeof(fill), 48000, 2, 16), OVE_OK);
#if SIZE_MAX > UINT32_MAX
	assert_int_equal(t.ops->push_audio(&t, fill, (size_t)UINT32_MAX + 9, 48000, 2, 16), OVE_OK);
#endif
	assert_memory_equal(fake_audio + IN_HEADER, input_header, sizeof(input_header));
	assert_memory_equal(fake_audio + OUT_PCM, playback, sizeof(playback));
	assert_int_equal(read_u32(fake_audio), sizeof(playback));
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_event_backpressure),
		cmocka_unit_test(test_first_audio_push),
		cmocka_unit_test(test_host_audio_pull),
		cmocka_unit_test(test_audio_pull_after_failed_init_read),
		cmocka_unit_test(test_pull_before_push),
		cmocka_unit_test(test_audio_push_wrap),
		cmocka_unit_test(test_audio_push_exceeds_capacity),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
