/* Engine-neutral removable-media ownership tests. */
#include "../framework/ove_test.h"

#include "ove/media.h"

static void test_filesystem_and_raw_arbitration(void **state)
{
	(void)state;
	struct ove_media_lease reader = {0};
	struct ove_media_lease writer = {0};

	ove_media_observe(10u, 1);
	assert_int_equal(ove_media_fs_acquire(), OVE_OK);
	/* Read-only inspection is allowed; a raw writer is exclusive. */
	assert_int_equal(ove_media_raw_acquire(&reader, 0u, 10u), OVE_OK);
	assert_int_equal(ove_media_raw_acquire(&writer, OVE_MEDIA_RAW_WRITE, 10u), OVE_ERR_BUSY);
	ove_media_raw_release(&reader);
	ove_media_fs_release();

	assert_int_equal(ove_media_raw_acquire(&writer, OVE_MEDIA_RAW_WRITE, 10u), OVE_OK);
	assert_int_equal(ove_media_fs_acquire(), OVE_ERR_BUSY);
	assert_int_equal(ove_media_raw_acquire(&reader, 0u, 10u), OVE_ERR_BUSY);
	ove_media_raw_release(&writer);
}

static void test_generation_invalidates_old_lease(void **state)
{
	(void)state;
	struct ove_media_lease reader = {0};

	ove_media_observe(20u, 1);
	assert_int_equal(ove_media_raw_acquire(&reader, 0u, 20u), OVE_OK);
	assert_true(ove_media_raw_valid(&reader, 0u));
	ove_media_observe(20u, 0);
	assert_false(ove_media_raw_valid(&reader, 0u));
	ove_media_observe(21u, 1);
	assert_false(ove_media_raw_valid(&reader, 0u));
	ove_media_raw_release(&reader);
	assert_int_equal(ove_media_raw_acquire(&reader, 0u, 20u), OVE_ERR_NOT_REGISTERED);
	assert_int_equal(ove_media_raw_acquire(&reader, 0u, 21u), OVE_OK);
	ove_media_raw_release(&reader);
}

int test_media_run(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_filesystem_and_raw_arbitration),
		cmocka_unit_test(test_generation_invalidates_old_lease),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
