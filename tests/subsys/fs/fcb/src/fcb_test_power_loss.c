/*
 * Copyright (c) 2026 Harsimranjeet Singh
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/stats/stats.h>

#include "fcb_test.h"

/*
 * These tests cut power in the middle of a flash write using the flash
 * simulator: the write numbered max_write_calls keeps only its first
 * max_len bytes, and every write after it is dropped.
 */

static uint32_t *write_calls;
static uint32_t *max_write_calls;
static uint32_t *max_len;

static int find_sim_stat(struct stats_hdr *hdr, void *arg, const char *name, uint16_t off)
{
	uint32_t *stat = (uint32_t *)((uint8_t *)hdr + off);

	if (strcmp(name, "flash_write_calls") == 0) {
		write_calls = stat;
	} else if (strcmp(name, "max_write_calls") == 0) {
		max_write_calls = stat;
	} else if (strcmp(name, "max_len") == 0) {
		max_len = stat;
	}

	return 0;
}

/* Cut power during the next flash write, keeping only its first bytes. */
static void cut_power_on_next_write(uint32_t bytes)
{
	*max_len = bytes;
	*max_write_calls = *write_calls + 1;
}

static void restore_power(void)
{
	*max_write_calls = 0;
	*max_len = 0;
}

/* Throw away the FCB state in RAM, like a reboot, and init it again. */
static int reboot_fcb(struct fcb *fcb)
{
	memset(fcb, 0, sizeof(*fcb));
	fcb->f_sector_cnt = 2;
	fcb->f_sectors = test_fcb_sector;

	return fcb_init(TEST_FCB_FLASH_AREA_ID, fcb);
}

/* Write one complete entry using the same data pattern as the other tests. */
static void append_entry(struct fcb *fcb, uint16_t len)
{
	struct fcb_entry loc;
	uint8_t data[64];
	int rc;

	zassert_true(len <= sizeof(data));

	rc = fcb_append(fcb, len, &loc);
	zassert_equal(rc, 0, "fcb_append failed: %d", rc);

	for (int i = 0; i < len; i++) {
		data[i] = fcb_test_append_data(len, i);
	}

	rc = flash_area_write(fcb->fap, FCB_ENTRY_FA_DATA_OFF(loc), data, len);
	zassert_equal(rc, 0, "flash_area_write failed: %d", rc);

	rc = fcb_append_finish(fcb, &loc);
	zassert_equal(rc, 0, "fcb_append_finish failed: %d", rc);
}

/* #121748: power cut while writing the 2 byte length of an entry */
ZTEST(fcb_test_power_loss, test_fcb_torn_len)
{
	struct fcb *fcb = &test_fcb;
	struct fcb_entry loc;
	int cnt;
	int rc;

	append_entry(fcb, 32);
	append_entry(fcb, 33);

	/* 200 bytes needs a 2 byte length, only the first byte gets written */
	cut_power_on_next_write(1);
	rc = fcb_append(fcb, 200, &loc);
	zassert_equal(rc, 0, "fcb_append failed: %d", rc);
	restore_power();

	rc = reboot_fcb(fcb);
	zassert_equal(rc, 0, "fcb_init after torn length failed: %d", rc);

	/* the two entries written before the power cut are still there */
	cnt = 32;
	rc = fcb_walk(fcb, NULL, fcb_test_data_walk_cb, &cnt);
	zassert_equal(rc, 0, "fcb_walk failed: %d", rc);
	zassert_equal(cnt, 34, "entries written before the power cut are missing");

	/* and we can still add new ones */
	append_entry(fcb, 34);

	cnt = 32;
	rc = fcb_walk(fcb, NULL, fcb_test_data_walk_cb, &cnt);
	zassert_equal(rc, 0, "fcb_walk failed: %d", rc);
	zassert_equal(cnt, 35, "new entry missing after recovery");
}

static void power_loss_before(void *data)
{
	stats_walk(stats_group_find("flash_sim_stats"), find_sim_stat, NULL);
	stats_walk(stats_group_find("flash_sim_thresholds"), find_sim_stat, NULL);
	zassert_not_null(write_calls);
	zassert_not_null(max_write_calls);
	zassert_not_null(max_len);

	restore_power();
	fcb_tc_pretest(2, &test_fcb);
}

/* restore power even if a test failed half way, so other tests aren't affected */
static void power_loss_after(void *data)
{
	if (max_write_calls != NULL) {
		restore_power();
	}
}

ZTEST_SUITE(fcb_test_power_loss, NULL, NULL, power_loss_before, power_loss_after, NULL);
