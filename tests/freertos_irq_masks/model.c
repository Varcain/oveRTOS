/* Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "FreeRTOS.h"
#include "ove/irq.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Initialized scalar models only; no native task or exception frame exists. */
struct model_masks {
	uint32_t p, b, f;
};
unsigned model_bits = 4, model_device_bits = 4;
uint64_t model_threshold = 128;
static struct model_masks registers, required, target;
static unsigned exception, priority[256], group, control = 4, depth;
static unsigned checks, failures, cases, rejected, delivered, enters, exits;
static unsigned bad_access, weakened, bad_write, writes, raises, lowers, bad_origin;
static uint32_t isr_keys[8];
static unsigned isr_depth;
static bool expect_reject, exiting, restored;

static void check(bool ok, const char *message)
{
	++checks;
	if (!ok) {
		++failures;
		printf("FAIL: %s\n", message);
	}
}
static void footer(void)
{
	printf("irq masks: cases=%u checks=%u failures=%u rejected=%u\n", cases, checks, failures,
	       rejected);
}
static bool privileged(void)
{
	return exception != 0 || !(control & 1u);
}
static void access_check(void)
{
	bad_access += !privileged();
}
static bool blocked(struct model_masks value, unsigned urgency)
{
	return value.p || value.f || (value.b && urgency >= value.b);
}
static void observe(void)
{
	if (exiting && registers.p == target.p && registers.b == target.b &&
	    registers.f == target.f)
		restored = true;
	struct model_masks needed = exiting && restored ? target : required;
	for (unsigned urgency = 0; urgency < 256; urgency += 16) {
		/* An active handler already excludes equal/lower urgency handlers. */
		if (exception && exception < 256 && urgency >= priority[exception])
			continue;
		weakened += blocked(needed, urgency) && !blocked(registers, urgency);
	}
	weakened += needed.f && !registers.f;
}
uint32_t event_mask_primask(void)
{
	access_check();
	observe();
	return registers.p;
}
uint32_t event_mask_basepri(void)
{
	access_check();
	observe();
	return registers.b;
}
uint32_t event_mask_faultmask(void)
{
	access_check();
	observe();
	return registers.f;
}
void event_mask_disable_faults(void)
{
	access_check();
	++writes;
	registers.f = 1;
	observe();
}
void event_mask_disable_irqs(void)
{
	access_check();
	++writes;
	registers.p = 1;
	observe();
}
void event_mask_restore_basepri(uint32_t value)
{
	access_check();
	++writes;
	bad_write += !registers.p;
	registers.b = value;
	observe();
}
void event_mask_restore_primask(uint32_t value)
{
	access_check();
	++writes;
	/* This is the intentional boundary restoration, after the native exit. */
	if (exiting && registers.b == target.b && value == target.p)
		restored = true;
	registers.p = value;
	observe();
}
void event_mask_restore_faultmask(uint32_t value)
{
	access_check();
	++writes;
	registers.f = value;
	observe();
}
uint32_t irq_mask_exception(void)
{
	return exception;
}
uint32_t irq_mask_control(void)
{
	return control;
}
uint32_t irq_mask_group(void)
{
	access_check();
	return group;
}
uint32_t irq_mask_priority(uint32_t value)
{
	access_check();
	if (value >= 256) {
		fputs("fixture rejected: out-of-range priority lookup\n", stderr);
		exit(2);
	}
	return priority[value];
}
BaseType_t xPortIsInsideInterrupt(void)
{
	return exception != 0;
}
BaseType_t model_privileged(void)
{
	return !(control & 1u);
}
#if OVE_MODEL_MPU
extern unsigned char __start_freertos_system_calls[], __stop_freertos_system_calls[];
static void syscall_anchor(void) FREERTOS_SYSTEM_CALL __attribute__((used));
static void syscall_anchor(void)
{
}
#endif
__attribute__((noinline)) void model_raise(void)
{
	++raises;
#if OVE_MODEL_MPU
	uintptr_t pc = (uintptr_t)__builtin_return_address(0);
	bad_origin += pc < (uintptr_t)__start_freertos_system_calls ||
		      pc >= (uintptr_t)__stop_freertos_system_calls;
#endif
	/* Priority-zero SVC remains serviceable with nonzero BASEPRI. */
	bad_access += exception != 0 || registers.p || registers.f || priority[11] != 0;
	control &= ~1u;
}
void model_lower(void)
{
	++lowers;
	control |= 1u;
}
static void native_raise(void)
{
#if OVE_MODEL_ERRATUM
	registers.p = 1;
	observe();
#endif
	registers.b = (uint32_t)model_threshold;
	observe();
#if OVE_MODEL_ERRATUM
	registers.p = 0;
	observe();
#endif
}
void model_enter_critical(void) FREERTOS_SYSTEM_CALL;
void model_exit_critical(void) FREERTOS_SYSTEM_CALL;
void model_enter_critical(void)
{
	++enters;
	bool promote = !privileged();
	if (promote) {
#if OVE_MODEL_MPU && OVE_MODEL_ALLOW
		model_raise();
#else
		++bad_access;
#endif
	}
	native_raise();
	++depth;
#if OVE_MODEL_MPU && OVE_MODEL_ALLOW
	if (promote)
		model_lower();
#endif
}
void model_exit_critical(void)
{
	++exits;
	bool promote = !privileged();
	if (promote) {
#if OVE_MODEL_MPU && OVE_MODEL_ALLOW
		model_raise();
#else
		++bad_access;
#endif
	}
	check(depth != 0, "native task exit has matching ownership");
	if (depth && --depth == 0)
		registers.b = 0;
	observe();
#if OVE_MODEL_MPU && OVE_MODEL_ALLOW
	if (promote)
		model_lower();
#endif
}
UBaseType_t model_enter_critical_from_isr(void)
{
	++enters;
	if (isr_depth == 8)
		exit(2);
	uint32_t previous = UINT32_C(0xa5123400) | registers.b;
	isr_keys[isr_depth++] = previous;
	native_raise();
	return previous;
}
void model_exit_critical_from_isr(UBaseType_t previous)
{
	++exits;
	check(isr_depth && previous == isr_keys[isr_depth - 1],
	      "native ISR cookie retains all bits");
	if (!isr_depth)
		exit(2);
	/* Restore initialized owned scalar backing even when a mutant key is wrong. */
	registers.b = isr_keys[--isr_depth] & 255u;
	observe();
}

#include OVE_IRQ_SOURCE

_Noreturn void irq_mask_reject(void)
{
	++rejected;
	check(expect_reject, "only an explicitly unsupported entry rejects");
	check(enters == 0 && exits == 0 && writes == 0 && !bad_access && !bad_origin,
	      "rejection precedes native acquisition and masks");
	check(delivered == 0, "rejection delivers no successful key or callback");
	footer();
	exit(failures != 0);
}
static bool equal(struct model_masks a, struct model_masks b)
{
	return a.p == b.p && a.b == b.b && a.f == b.f;
}
static void initialize(struct model_masks incoming, unsigned active, bool user, unsigned nesting)
{
	registers = required = target = incoming;
	exception = active;
	group = 0;
	control = 4u | user;
	depth = nesting;
	memset(priority, 0, sizeof(priority));
	priority[14] = priority[15] = 240;
	if (active >= 16 && active < 256)
		priority[active] = 144;
	model_bits = model_device_bits = 4;
	model_threshold = 128;
	enters = exits = writes = delivered = rejected = 0;
	bad_access = weakened = bad_write = raises = lowers = bad_origin = isr_depth = 0;
	expect_reject = exiting = restored = false;
}
static void release(ove_irq_key_t key, struct model_masks incoming)
{
	target = incoming;
	required = registers;
	exiting = true;
	restored = false;
	ove_irq_unlock(key);
	exiting = false;
	required = incoming;
}
static void roundtrip(struct model_masks incoming, unsigned active, bool user, unsigned nesting)
{
	++cases;
	initialize(incoming, active, user, nesting);
	ove_irq_key_t outer = ove_irq_lock();
	struct model_masks held = {incoming.p, incoming.b && incoming.b < 128 ? incoming.b : 128,
				   incoming.f};
	check(equal(registers, held),
	      "entry preserves stronger input and removes temporary envelopes");
	check(control == (4u | user),
	      "public lock preserves caller mode and unrelated control bits");
	check(depth == nesting + (active == 0), "task native nesting changes exactly once");
	check(weakened == 0, "entry never weakens incoming effective exclusion");
	required = held;
	ove_irq_key_t inner = ove_irq_lock();
	check(equal(registers, held), "nested acquisition preserves the active state");
	release(inner, held);
	check(equal(registers, held), "inner key restores its own captured state");
	release(outer, incoming);
	check(equal(registers, incoming), "outer key restores exact original mask state");
	check(depth == nesting && isr_depth == 0, "all native ownership is balanced");
	check(enters == 2 && exits == 2, "native dispatch executes once for each operation");
	check(control == (4u | user), "public unlock restores caller mode");
	check(!bad_access && !bad_origin && !bad_write && !weakened,
	      "all wrapper transitions, mask writes and privilege origins are valid");
	check(!active || (raises == 0 && lowers == 0),
	      "handlers never alter interrupted Thread privilege");
}
static void body_controls(void)
{
	for (unsigned original = 0; original < 2; ++original) {
		++cases;
		struct model_masks incoming = {0, 0, original};
		initialize(incoming, 0, false, 0);
		ove_irq_key_t key = ove_irq_lock();
		/* Explicit privileged body-current changes; no actual fault is raised. */
		registers = (struct model_masks){0, 64, !original};
		release(key, incoming);
		check(equal(registers, incoming) && !weakened,
		      "exit protects current and target fault masks");
	}
	for (unsigned strong = 0; strong < 2; ++strong) {
		++cases;
		struct model_masks incoming = {strong, strong ? 0 : 64, 0};
		initialize(incoming, 0, false, 0);
		ove_irq_key_t key = ove_irq_lock();
		/* Deliberate body observation of the stock operation, separate from
		 * wrapper-transition assertions. It still excludes kernel-aware IRQs. */
		required = (struct model_masks){0, 128, 0};
		model_enter_critical();
		model_exit_critical();
		check(registers.b == 128 && (!OVE_MODEL_ERRATUM || !registers.p),
		      "native body retains its documented stronger-mask limitation");
		check(blocked(registers, 144), "ordinary native kernel-aware exclusion remains");
		check(blocked(registers, 64) == (strong && !OVE_MODEL_ERRATUM),
		      "strong-input body behavior matches the native limitation");
		release(key, incoming);
		check(equal(registers, incoming),
		      "wrapper restores input after a native body operation");
	}
}
static void metadata_controls(void)
{
	for (unsigned bits = 1; bits <= 8; ++bits) {
		unsigned maximum = bits == 8 ? 0 : 7 - bits;
		for (unsigned grouping = 0; grouping <= maximum; ++grouping)
			for (unsigned role = 0; role < 4; ++role) {
				++cases;
				initialize((struct model_masks){0, 0, 0},
					   role == 0   ? 0
					   : role == 1 ? 17
					   : role == 2 ? 14
						       : 15,
					   false, 0);
				model_bits = model_device_bits = bits;
				model_threshold = bits == 8 ? 2 : 1u << (8 - bits);
				group = grouping;
				if (exception)
					priority[exception] = (unsigned)model_threshold;
				ove_irq_key_t key = ove_irq_lock();
				check(registers.b == model_threshold && !registers.p &&
					      !registers.f,
				      "valid metadata and threshold-equal handlers acquire");
				release(key, (struct model_masks){0, 0, 0});
				check(equal(registers, (struct model_masks){0, 0, 0}) &&
					      !bad_access && !bad_origin && !bad_write && !weakened,
				      "metadata boundary restores masks without forbidden access");
				check(depth == 0 && isr_depth == 0,
				      "metadata control retires native ownership");
			}
	}
}
static void reject_case(const char *name)
{
	++cases;
	initialize((struct model_masks){0, 0, 0}, 17, false, 0);
	expect_reject = true;
	if (!strcmp(name, "fast"))
		priority[17] = 64;
	else if (!strcmp(name, "systick")) {
		exception = 15;
		priority[15] = 64;
	} else if (!strcmp(name, "pendsv")) {
		exception = 14;
		priority[14] = 64;
	} else if (!strcmp(name, "nmi"))
		exception = 2;
	else if (!strcmp(name, "hardfault"))
		exception = 3;
	else if (!strcmp(name, "fault")) {
		exception = 5;
		priority[5] = 240;
	} else if (!strcmp(name, "supervisor")) {
		exception = 11;
		priority[11] = 240;
	} else if (!strcmp(name, "debug")) {
		exception = 12;
		priority[12] = 240;
	} else if (!strcmp(name, "reserved"))
		exception = 13;
	else if (!strcmp(name, "impossible"))
		exception = 511;
	else if (!strcmp(name, "group"))
		group = 4;
	else if (!strcmp(name, "bits-low"))
		model_bits = model_device_bits = 0;
	else if (!strcmp(name, "bits-high"))
		model_bits = model_device_bits = 9;
	else if (!strcmp(name, "bits-mismatch"))
		model_device_bits = 3;
	else if (!strcmp(name, "threshold-zero"))
		model_threshold = 0;
	else if (!strcmp(name, "threshold-wide"))
		model_threshold = UINT64_C(1) << 40;
	else if (!strcmp(name, "threshold-unaligned"))
		model_threshold = 129;
	else if (!strcmp(name, "eight-odd")) {
		model_bits = model_device_bits = 8;
		model_threshold = 129;
	} else if (!strcmp(name, "eight-group")) {
		model_bits = model_device_bits = 8;
		group = 1;
	} else if (!strcmp(name, "ordinary-user")) {
		exception = 0;
		control |= 1u;
	} else if (strcmp(name, "unsupported"))
		exit(2);
	ove_irq_key_t key = ove_irq_lock();
	++delivered;
	ove_irq_unlock(key);
	check(false, "unsupported entry returned success instead of rejecting");
	footer();
}
int main(int argc, char **argv)
{
	if (argc != 2 && argc != 3)
		return 2;
	bool canary = strstr(argv[1], "canary") != NULL;
	if (canary)
		check(false, "early failure survives every later IRQ control");
	if (!strncmp(argv[1], "reject", 6)) {
		if (argc != 3)
			return 2;
		reject_case(argv[2]);
		return failures != 0;
	}
	if (strcmp(argv[1], "normal") && strcmp(argv[1], "canary"))
		return 2;
	static const unsigned bases[] = {0, 16, 64, 128, 192, 240};
	for (unsigned p = 0; p < 2; ++p)
		for (unsigned b = 0; b < 6; ++b)
			for (unsigned f = 0; f < 2; ++f) {
				struct model_masks state = {p, bases[b], f};
				roundtrip(state, 0, false, 0);
				roundtrip(state, 0, false, UINT32_C(0xaaaaaaaa));
				if (p || f || (bases[b] && bases[b] <= 128))
					roundtrip(state, 0, false, 3);
				for (unsigned role = 0; role < 3; ++role)
					for (unsigned mode = 0; mode < 2; ++mode)
						roundtrip(state,
							  role == 0   ? 17
							  : role == 1 ? 14
								      : 15,
							  mode, 0);
#if OVE_MODEL_MPU
				if (!p && !f)
					roundtrip(state, 0, true, 0);
#endif
			}
	body_controls();
	metadata_controls();
	footer();
	return failures != 0;
}
