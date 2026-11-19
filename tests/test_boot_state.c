// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Copyright (C) 2026 Denis Vojtuk
 *
 * Unit tests for boot_state.c with a fake SMC.
 *
 * Build and run with "make test". The build turns on the address and
 * undefined-behavior sanitizers, so a stray read or write fails the run.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot_state.h"

static unsigned int checks;
static unsigned int failures;

#define CHECK(cond)								\
	do {									\
		checks++;							\
		if (!(cond)) {							\
			failures++;						\
			fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #cond); \
		}								\
	} while (0)

enum { IDX_EFMV, IDX_EFMS, IDX_EFBS, IDX_COUNT };

#define MAX_LOGGED_WRITES 8

struct fake_write {
	u32 key;
	size_t len;
	u8 data[T2BS_EFMV_LEN];
};

struct fake_smc {
	/* The SMC keys and their sizes. */
	u8 efmv[T2BS_EFMV_LEN];
	u8 efms;
	u8 efbs;
	u8 size[IDX_COUNT];

	/* Faults to inject. Zero and -1 mean none. */
	int key_size_err[IDX_COUNT];
	int read_efmv_err;
	int write_err[IDX_COUNT];
	int poll_err;
	int poll_err_at;	/* fail this EFMS read after the commit */

	/* The T2: it answers at this EFMS read after the commit, -1 is never. */
	int respond_at;
	u8 response;

	u64 now;
	unsigned int ms_per_sleep;

	/* What the code under test did. */
	bool committed;
	int polls;
	unsigned int sleeps;
	unsigned int bad_sleeps;
	unsigned int nwrites;
	struct fake_write writes[MAX_LOGGED_WRITES];
};

static const u32 version_air[T2BS_VERSION_FIELDS] = { 24, 16, 6005, 0, 0, 0 };

static int key_index(u32 key)
{
	switch (key) {
	case T2BS_KEY_EFMV:
		return IDX_EFMV;
	case T2BS_KEY_EFMS:
		return IDX_EFMS;
	case T2BS_KEY_EFBS:
		return IDX_EFBS;
	default:
		return -1;
	}
}

static int fake_key_size(void *ctx, u32 key, u8 *size)
{
	struct fake_smc *smc = ctx;
	int i = key_index(key);

	if (i < 0)
		return -ENOENT;
	if (smc->key_size_err[i])
		return smc->key_size_err[i];
	*size = smc->size[i];
	return 0;
}

static int fake_read(void *ctx, u32 key, u8 *buf, size_t len)
{
	struct fake_smc *smc = ctx;

	switch (key) {
	case T2BS_KEY_EFMV:
		if (smc->read_efmv_err)
			return smc->read_efmv_err;
		if (len != T2BS_EFMV_LEN)
			return -EINVAL;
		memcpy(buf, smc->efmv, len);
		return 0;
	case T2BS_KEY_EFMS:
		if (len != T2BS_STATE_LEN)
			return -EINVAL;
		if (smc->committed) {
			int n = smc->polls++;

			if (n == smc->poll_err_at)
				return smc->poll_err;
			if (smc->respond_at >= 0 && n >= smc->respond_at)
				smc->efms = smc->response;
		}
		buf[0] = smc->efms;
		return 0;
	default:
		return -ENOENT;
	}
}

static int fake_write(void *ctx, u32 key, const u8 *buf, size_t len)
{
	struct fake_smc *smc = ctx;
	int i = key_index(key);

	if (smc->nwrites < MAX_LOGGED_WRITES && len <= T2BS_EFMV_LEN) {
		struct fake_write *w = &smc->writes[smc->nwrites];

		w->key = key;
		w->len = len;
		memcpy(w->data, buf, len);
	}
	smc->nwrites++;

	if (i < 0)
		return -ENOENT;
	if (smc->write_err[i])
		return smc->write_err[i];

	switch (i) {
	case IDX_EFMV:
		if (len != T2BS_EFMV_LEN)
			return -EINVAL;
		memcpy(smc->efmv, buf, len);
		break;
	case IDX_EFMS:
		if (len != T2BS_STATE_LEN)
			return -EINVAL;
		smc->efms = buf[0];
		break;
	default:
		if (len != T2BS_STATE_LEN)
			return -EINVAL;
		smc->efbs = buf[0];
		if (buf[0] == T2BS_EFBS_COMMIT)
			smc->committed = true;
		break;
	}
	return 0;
}

static u64 fake_now(void *ctx)
{
	struct fake_smc *smc = ctx;

	return smc->now;
}

static void fake_sleep(void *ctx, unsigned int ms)
{
	struct fake_smc *smc = ctx;

	smc->sleeps++;
	if (ms != T2BS_POLL_MS)
		smc->bad_sleeps++;
	smc->now += smc->ms_per_sleep;
}

static const struct t2bs_smc_ops fake_ops = {
	.key_size = fake_key_size,
	.read = fake_read,
	.write = fake_write,
	.now_ms = fake_now,
	.sleep_ms = fake_sleep,
};

/* A T2 Mac after a Linux boot: EFMV pristine, a stale answer left in EFMS. */
static void fake_init(struct fake_smc *smc)
{
	memset(smc, 0, sizeof(*smc));
	smc->efmv[0] = T2BS_EFMV_PRISTINE;
	smc->efms = 1;
	smc->size[IDX_EFMV] = T2BS_EFMV_LEN;
	smc->size[IDX_EFMS] = T2BS_STATE_LEN;
	smc->size[IDX_EFBS] = T2BS_STATE_LEN;
	smc->poll_err_at = -1;
	smc->respond_at = 0;
	smc->response = 1;
	smc->now = 1000;
	smc->ms_per_sleep = T2BS_POLL_MS;
}

static struct t2bs_result run(struct fake_smc *smc)
{
	/* Start from garbage: publish must set every field. */
	struct t2bs_result result = {
		.state = T2BS_IN_PROGRESS, .response = 0xee, .error = 12345, .wrote = true,
	};

	t2bs_publish(&fake_ops, smc, version_air, 1, 0, true, &result);
	return result;
}

static struct t2bs_result probe(struct fake_smc *smc)
{
	struct t2bs_result result = {
		.state = T2BS_IN_PROGRESS, .response = 0xee, .error = 12345, .wrote = true,
	};

	t2bs_publish(&fake_ops, smc, version_air, 1, 0, false, &result);
	return result;
}

static void check_untouched(const struct fake_smc *smc)
{
	static const u8 pristine[T2BS_EFMV_LEN] = { T2BS_EFMV_PRISTINE };

	CHECK(smc->nwrites == 0);
	CHECK(!smc->committed);
	CHECK(smc->sleeps == 0);
	CHECK(smc->efbs == 0);
	CHECK(smc->efms == 1);
	/* Only tests that keep EFMV pristine call this. */
	CHECK(memcmp(smc->efmv, pristine, sizeof(pristine)) == 0 || smc->efmv[0] != T2BS_EFMV_PRISTINE ||
	      smc->efmv[T2BS_EFMV_LEN - 1] != 0);
}

static void test_success(void)
{
	static const u8 expected_efmv[T2BS_EFMV_LEN] = {
		0x01, 0x01, 0x00, 0x00,		/* format, epoch 1.0, zero */
		0x18, 0x00, 0x00, 0x00,		/* 24 */
		0x10, 0x00, 0x00, 0x00,		/* 16 */
		0x75, 0x17, 0x00, 0x00,		/* 6005 */
	};
	struct fake_smc smc;
	struct t2bs_result r;

	fake_init(&smc);
	r = run(&smc);

	CHECK(r.state == T2BS_RESPONSE_RECEIVED);
	CHECK(r.response == 1);
	CHECK(r.error == 0);
	CHECK(r.wrote);

	/* Exactly three writes, in the firmware's order. */
	CHECK(smc.nwrites == 3);
	CHECK(smc.writes[0].key == T2BS_KEY_EFMV);
	CHECK(smc.writes[0].len == T2BS_EFMV_LEN);
	CHECK(memcmp(smc.writes[0].data, expected_efmv, T2BS_EFMV_LEN) == 0);
	CHECK(smc.writes[1].key == T2BS_KEY_EFMS);
	CHECK(smc.writes[1].len == 1 && smc.writes[1].data[0] == 0);
	CHECK(smc.writes[2].key == T2BS_KEY_EFBS);
	CHECK(smc.writes[2].len == 1 && smc.writes[2].data[0] == T2BS_EFBS_COMMIT);

	CHECK(smc.polls == 1);
	CHECK(smc.sleeps == 0);
}

/* A probe checks the same preconditions and writes nothing. */
static void test_probe(void)
{
	struct fake_smc smc;
	struct t2bs_result r;

	fake_init(&smc);
	r = probe(&smc);
	CHECK(r.state == T2BS_READY);
	CHECK(r.error == 0);
	CHECK(r.response == 0);
	CHECK(!r.wrote);
	check_untouched(&smc);

	/* A publish after the probe still works. */
	r = run(&smc);
	CHECK(r.state == T2BS_RESPONSE_RECEIVED);
	CHECK(smc.nwrites == 3);

	/* And now EFMV is no longer pristine, so a probe says so. */
	r = probe(&smc);
	CHECK(r.state == T2BS_REJECTED);
	CHECK(r.error == -EALREADY);
	CHECK(!r.wrote);
	CHECK(smc.nwrites == 3);

	fake_init(&smc);
	smc.size[IDX_EFBS] = 4;
	r = probe(&smc);
	CHECK(r.state == T2BS_REJECTED && r.error == -EPROTO && !r.wrote);
	check_untouched(&smc);

	fake_init(&smc);
	smc.read_efmv_err = -EIO;
	r = probe(&smc);
	CHECK(r.state == T2BS_FAILED && r.error == -EIO && !r.wrote);
	check_untouched(&smc);
}

static void test_response_after_some_polls(void)
{
	struct fake_smc smc;
	struct t2bs_result r;

	fake_init(&smc);
	smc.respond_at = 5;
	smc.response = 3;
	r = run(&smc);

	CHECK(r.state == T2BS_RESPONSE_RECEIVED);
	CHECK(r.response == 3);
	CHECK(r.wrote);
	CHECK(smc.polls == 6);
	CHECK(smc.sleeps == 5);
	CHECK(smc.bad_sleeps == 0);
	CHECK(smc.nwrites == 3);
}

/* The stale answer must be cleared before the commit, so it cannot be read back. */
static void test_stale_answer_is_cleared(void)
{
	struct fake_smc smc;
	struct t2bs_result r;

	fake_init(&smc);
	smc.efms = 7;
	smc.respond_at = 2;
	smc.response = 1;
	r = run(&smc);

	CHECK(r.state == T2BS_RESPONSE_RECEIVED);
	CHECK(r.response == 1);
	CHECK(smc.polls == 3);
}

static void test_wrong_key_size(void)
{
	static const u8 wrong[IDX_COUNT] = { 31, 2, 0 };
	int i;

	for (i = 0; i < IDX_COUNT; i++) {
		struct fake_smc smc;
		struct t2bs_result r;

		fake_init(&smc);
		smc.size[i] = wrong[i];
		r = run(&smc);

		CHECK(r.state == T2BS_REJECTED);
		CHECK(r.error == -EPROTO);
		CHECK(!r.wrote);
		check_untouched(&smc);
	}
}

static void test_key_size_error(void)
{
	int i;

	for (i = 0; i < IDX_COUNT; i++) {
		struct fake_smc smc;
		struct t2bs_result r;

		fake_init(&smc);
		smc.key_size_err[i] = -ENOENT;
		r = run(&smc);

		CHECK(r.state == T2BS_FAILED);
		CHECK(r.error == -ENOENT);
		CHECK(!r.wrote);
		check_untouched(&smc);
	}
}

static void test_efmv_read_error(void)
{
	struct fake_smc smc;
	struct t2bs_result r;

	fake_init(&smc);
	smc.read_efmv_err = -EIO;
	r = run(&smc);

	CHECK(r.state == T2BS_FAILED);
	CHECK(r.error == -EIO);
	CHECK(!r.wrote);
	check_untouched(&smc);
}

static void test_efmv_not_pristine(void)
{
	int variant;

	for (variant = 0; variant < 4; variant++) {
		struct fake_smc smc;
		struct t2bs_result r;
		u8 before[T2BS_EFMV_LEN];

		fake_init(&smc);
		switch (variant) {
		case 0:		/* a record is already there */
			smc.efmv[0] = T2BS_EFMV_FORMAT;
			break;
		case 1:		/* sentinel, but the last byte is set */
			smc.efmv[T2BS_EFMV_LEN - 1] = 1;
			break;
		case 2:		/* sentinel, but the second byte is set */
			smc.efmv[1] = 0xff;
			break;
		default:	/* all zeros */
			smc.efmv[0] = 0;
			break;
		}
		memcpy(before, smc.efmv, sizeof(before));
		r = run(&smc);

		CHECK(r.state == T2BS_REJECTED);
		CHECK(r.error == -EALREADY);
		CHECK(!r.wrote);
		CHECK(smc.nwrites == 0);
		CHECK(!smc.committed);
		CHECK(memcmp(before, smc.efmv, sizeof(before)) == 0);
	}
}

static void test_write_error(void)
{
	int i;

	for (i = 0; i < IDX_COUNT; i++) {
		struct fake_smc smc;
		struct t2bs_result r;

		fake_init(&smc);
		smc.write_err[i] = -EBUSY;
		r = run(&smc);

		CHECK(r.state == T2BS_COMMIT_UNCERTAIN);
		CHECK(r.error == -EBUSY);
		CHECK(r.wrote);
		/* It stops at the failed write and never polls. */
		CHECK(smc.nwrites == (unsigned int)i + 1);
		CHECK(!smc.committed);
		CHECK(smc.polls == 0);
		CHECK(smc.sleeps == 0);
	}
}

static void test_poll_read_error(void)
{
	struct fake_smc smc;
	struct t2bs_result r;

	fake_init(&smc);
	smc.respond_at = -1;
	smc.poll_err_at = 3;
	smc.poll_err = -EIO;
	r = run(&smc);

	CHECK(r.state == T2BS_COMMIT_UNCERTAIN);
	CHECK(r.error == -EIO);
	CHECK(r.wrote);
	CHECK(smc.polls == 4);
	CHECK(smc.nwrites == 3);
}

static void test_timeout(void)
{
	struct fake_smc smc;
	struct t2bs_result r;

	fake_init(&smc);
	smc.respond_at = -1;
	r = run(&smc);

	CHECK(r.state == T2BS_COMMIT_UNCERTAIN);
	CHECK(r.error == -ETIMEDOUT);
	CHECK(r.wrote);
	CHECK(smc.now - 1000 == T2BS_TIMEOUT_MS);
	CHECK(smc.sleeps == T2BS_TIMEOUT_MS / T2BS_POLL_MS);
	CHECK(smc.bad_sleeps == 0);
	CHECK(smc.nwrites == 3);
}

/* A slow machine: each sleep takes far longer than asked. */
static void test_timeout_slow_sleep(void)
{
	struct fake_smc smc;
	struct t2bs_result r;

	fake_init(&smc);
	smc.respond_at = -1;
	smc.ms_per_sleep = 7000;
	r = run(&smc);

	CHECK(r.state == T2BS_COMMIT_UNCERTAIN);
	CHECK(r.error == -ETIMEDOUT);
	CHECK(smc.sleeps == 6);
}

/* The loop must end even when the clock stands still. */
static void test_timeout_stuck_clock(void)
{
	struct fake_smc smc;
	struct t2bs_result r;

	fake_init(&smc);
	smc.respond_at = -1;
	smc.ms_per_sleep = 0;
	r = run(&smc);

	CHECK(r.state == T2BS_COMMIT_UNCERTAIN);
	CHECK(r.error == -ETIMEDOUT);
	CHECK(smc.sleeps == T2BS_TIMEOUT_MS / T2BS_POLL_MS);
}

/* The answer arrives at the very last poll before the limit. */
static void test_answer_at_last_poll(void)
{
	struct fake_smc smc;
	struct t2bs_result r;

	fake_init(&smc);
	smc.respond_at = T2BS_TIMEOUT_MS / T2BS_POLL_MS;
	smc.response = 1;
	r = run(&smc);

	CHECK(r.state == T2BS_RESPONSE_RECEIVED);
	CHECK(r.response == 1);
}

/* The key size normally comes from the size field. */
static void test_key_info_size(void)
{
	/* A correct kernel: size and flags as the SMC reports them. */
	CHECK(t2bs_key_info_size(32, 0xc4) == 32);
	CHECK(t2bs_key_info_size(1, 0xc0) == 1);
	CHECK(t2bs_key_info_size(1, 0xd0) == 1);
	CHECK(t2bs_key_info_size(1, 0) == 1);
	CHECK(t2bs_key_info_size(255, 0x20) == 255);

	/* t2linux 7.3.5-1: size zero, the real size in the flags field. */
	CHECK(t2bs_key_info_size(0, 32) == 32);
	CHECK(t2bs_key_info_size(0, 1) == 1);

	/* Nothing usable at all stays zero, which no expected size matches. */
	CHECK(t2bs_key_info_size(0, 0) == 0);

	/* A zero size next to real flags gives a wrong size, and a rejection. */
	CHECK(t2bs_key_info_size(0, 0xc4) != T2BS_EFMV_LEN);
	CHECK(t2bs_key_info_size(0, 0xc0) != T2BS_STATE_LEN);
	CHECK(t2bs_key_info_size(0, 0xd0) != T2BS_STATE_LEN);
}

static void test_build_efmv(void)
{
	static const u32 version[T2BS_VERSION_FIELDS] = {
		0x01020304, 0xffffffff, 0, 1, 0x80000000, 0x0000ff00,
	};
	static const u8 expected[T2BS_EFMV_LEN] = {
		0x01, 0xab, 0xcd, 0x00,
		0x04, 0x03, 0x02, 0x01,
		0xff, 0xff, 0xff, 0xff,
		0x00, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x80,
		0x00, 0xff, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
	};
	/* Guard bytes around the record catch a write outside it. */
	u8 buf[T2BS_EFMV_LEN + 2];

	memset(buf, 0x5a, sizeof(buf));
	t2bs_build_efmv(&buf[1], version, 0xab, 0xcd);

	CHECK(buf[0] == 0x5a);
	CHECK(buf[sizeof(buf) - 1] == 0x5a);
	CHECK(memcmp(&buf[1], expected, T2BS_EFMV_LEN) == 0);
}

static void expect_parse(const char *text, int want)
{
	static const u32 untouched[T2BS_VERSION_FIELDS] = { 11, 22, 33, 44, 55, 66 };
	u32 version[T2BS_VERSION_FIELDS];
	int ret;

	memcpy(version, untouched, sizeof(version));
	ret = t2bs_parse_bridge_version(text, version);
	if (ret != want) {
		checks++;
		failures++;
		fprintf(stderr, "FAIL parse \"%s\": got %d, want %d\n", text ? text : "(null)", ret, want);
		return;
	}
	checks++;
	/* A failed parse leaves the output alone. */
	if (ret)
		CHECK(memcmp(version, untouched, sizeof(version)) == 0);
}

static void test_parse(void)
{
	u32 v[T2BS_VERSION_FIELDS] = { 0 };

	CHECK(t2bs_parse_bridge_version("2103.160.2.0.0 (iBridge: 24.16.6005.0.0,0)", v) == 0);
	CHECK(v[0] == 24 && v[1] == 16 && v[2] == 6005 && v[3] == 0 && v[4] == 0 && v[5] == 0);

	CHECK(t2bs_parse_bridge_version("1968.140.7.0.0 (iBridge: 20.16.6072.0.0,0)", v) == 0);
	CHECK(v[0] == 20 && v[2] == 6072);

	CHECK(t2bs_parse_bridge_version("(iBridge: 4294967295.0.1.2.3,4294967295)", v) == 0);
	CHECK(v[0] == 0xffffffffu && v[1] == 0 && v[2] == 1 && v[3] == 2 && v[4] == 3 && v[5] == 0xffffffffu);

	CHECK(t2bs_parse_bridge_version("(iBridge: 007.0.0.0.0,0)", v) == 0);
	CHECK(v[0] == 7);

	/* No T2 chip. */
	expect_parse(NULL, -ENODEV);
	expect_parse("", -ENODEV);
	expect_parse("MBP114.88Z.0177.B00.1708080033", -ENODEV);
	expect_parse("2103.160.2.0.0 (ibridge: 24.16.6005.0.0,0)", -ENODEV);
	expect_parse("2103.160.2.0.0 (iBridge:24.16.6005.0.0,0)", -ENODEV);

	/* A T2 chip, but not the expected form. */
	expect_parse("(iBridge: ", -EINVAL);
	expect_parse("(iBridge: )", -EINVAL);
	expect_parse("(iBridge: 24)", -EINVAL);
	expect_parse("(iBridge: 24.16.6005.0.0)", -EINVAL);
	expect_parse("(iBridge: 24.16.6005.0.0,)", -EINVAL);
	expect_parse("(iBridge: 24.16.6005.0.0,0", -EINVAL);
	expect_parse("(iBridge: 24.16.6005.0.0.0)", -EINVAL);
	expect_parse("(iBridge: 24,16.6005.0.0,0)", -EINVAL);
	expect_parse("(iBridge: 24.16.6005.0.0,0,1)", -EINVAL);
	expect_parse("(iBridge: 24.16.6005.0.0,0) ", -EINVAL);
	expect_parse("(iBridge: 24.16.6005.0.0,0)\n", -EINVAL);
	expect_parse("(iBridge: 24.16.6005.0.0,0)x", -EINVAL);
	expect_parse("(iBridge: 24..6005.0.0,0)", -EINVAL);
	expect_parse("(iBridge: .16.6005.0.0,0)", -EINVAL);
	expect_parse("(iBridge:  24.16.6005.0.0,0)", -EINVAL);
	expect_parse("(iBridge: 24. 16.6005.0.0,0)", -EINVAL);
	expect_parse("(iBridge: +24.16.6005.0.0,0)", -EINVAL);
	expect_parse("(iBridge: -24.16.6005.0.0,0)", -EINVAL);
	expect_parse("(iBridge: 0x18.16.6005.0.0,0)", -EINVAL);
	expect_parse("(iBridge: 24a.16.6005.0.0,0)", -EINVAL);
	expect_parse("(iBridge: 4294967296.16.6005.0.0,0)", -EINVAL);
	expect_parse("(iBridge: 24.16.6005.0.0,99999999999999999999999999)", -EINVAL);
}

/*
 * Feed the parser many strings: valid ones, and valid ones with a few random
 * changes. Each string sits in a heap block of its exact size, so the address
 * sanitizer reports any read past the end.
 */
static unsigned int prng(unsigned int *seed)
{
	*seed = *seed * 1664525u + 1013904223u;
	return *seed >> 8;
}

static void test_parse_random(void)
{
	static const char noise[] = "0123456789.,)( -+xiBridge:\n";
	unsigned int seed = 0x2545f491u;
	unsigned int n, accepted = 0, refused = 0;

	for (n = 0; n < 200000; n++) {
		u32 want[T2BS_VERSION_FIELDS], got[T2BS_VERSION_FIELDS];
		char base[128];
		unsigned int i, changes;
		size_t len;
		char *text;
		int ret;

		for (i = 0; i < T2BS_VERSION_FIELDS; i++) {
			/* Mostly small numbers, sometimes the full range. */
			want[i] = prng(&seed) % 4 ? prng(&seed) % 10000 : prng(&seed) * 257u;
		}
		len = (size_t)snprintf(base, sizeof(base), "%s(iBridge: %u.%u.%u.%u.%u,%u)",
				       prng(&seed) % 2 ? "2103.160.2.0.0 " : "",
				       want[0], want[1], want[2], want[3], want[4], want[5]);

		changes = prng(&seed) % 4;
		for (i = 0; i < changes && len > 0; i++) {
			size_t at = prng(&seed) % len;

			switch (prng(&seed) % 3) {
			case 0:		/* replace a character */
				base[at] = noise[prng(&seed) % (sizeof(noise) - 1)];
				break;
			case 1:		/* delete a character */
				memmove(&base[at], &base[at + 1], len - at);
				len--;
				break;
			default:	/* cut the string short */
				len = at;
				base[len] = '\0';
				break;
			}
		}

		text = malloc(len + 1);
		if (!text)
			abort();
		memcpy(text, base, len);
		text[len] = '\0';

		ret = t2bs_parse_bridge_version(text, got);
		if (ret == 0) {
			accepted++;
			/* An unchanged string must give back its numbers. */
			if (!changes)
				CHECK(memcmp(want, got, sizeof(want)) == 0);
		} else {
			refused++;
			CHECK(ret == -ENODEV || ret == -EINVAL);
			CHECK(changes != 0);
		}
		free(text);
	}
	printf("random parse: %u strings accepted, %u refused\n", accepted, refused);
	CHECK(accepted > 10000 && refused > 10000);
}

static void expect_text(enum t2bs_state state, u8 response, const char *want)
{
	struct t2bs_result r = { .state = state, .response = response };
	char buf[T2BS_RESULT_TEXT_LEN + 2];
	size_t len = strlen(want);
	size_t size;
	int ret;

	memset(buf, 0x5a, sizeof(buf));
	ret = t2bs_format_result(&r, buf, T2BS_RESULT_TEXT_LEN);
	CHECK(ret == (int)len);
	CHECK(strcmp(buf, want) == 0);
	CHECK((unsigned char)buf[T2BS_RESULT_TEXT_LEN] == 0x5a);

	/* Every buffer that is too small is refused and left alone. */
	for (size = 0; size <= len; size++) {
		char *small = malloc(size + 1);

		if (!small)
			abort();
		memset(small, 0x5a, size + 1);
		CHECK(t2bs_format_result(&r, small, size) == -ENOSPC);
		CHECK((unsigned char)small[0] == 0x5a);
		free(small);
	}

	/* The exact fit works. */
	{
		char *exact = malloc(len + 1);

		if (!exact)
			abort();
		CHECK(t2bs_format_result(&r, exact, len + 1) == (int)len);
		CHECK(strcmp(exact, want) == 0);
		free(exact);
	}
}

static void test_format(void)
{
	expect_text(T2BS_DISABLED, 0, "disabled");
	expect_text(T2BS_IN_PROGRESS, 0, "in-progress");
	expect_text(T2BS_NOT_APPLICABLE, 0, "not-applicable");
	expect_text(T2BS_READY, 0, "ready");
	expect_text(T2BS_REJECTED, 0, "rejected");
	expect_text(T2BS_FAILED, 0, "failed");
	expect_text(T2BS_COMMIT_UNCERTAIN, 0, "commit-uncertain");
	expect_text(T2BS_RESPONSE_RECEIVED, 0, "response-received:0");
	expect_text(T2BS_RESPONSE_RECEIVED, 1, "response-received:1");
	expect_text(T2BS_RESPONSE_RECEIVED, 3, "response-received:3");
	expect_text(T2BS_RESPONSE_RECEIVED, 10, "response-received:10");
	expect_text(T2BS_RESPONSE_RECEIVED, 255, "response-received:255");
	expect_text((enum t2bs_state)99, 0, "invalid");
	expect_text((enum t2bs_state)-1, 0, "invalid");

	CHECK(strlen("response-received:255") + 1 <= T2BS_RESULT_TEXT_LEN);
}

int main(void)
{
	test_success();
	test_probe();
	test_response_after_some_polls();
	test_stale_answer_is_cleared();
	test_wrong_key_size();
	test_key_size_error();
	test_efmv_read_error();
	test_efmv_not_pristine();
	test_write_error();
	test_poll_read_error();
	test_timeout();
	test_timeout_slow_sleep();
	test_timeout_stuck_clock();
	test_answer_at_last_poll();
	test_key_info_size();
	test_build_efmv();
	test_parse();
	test_parse_random();
	test_format();

	printf("%u checks, %u failures\n", checks, failures);
	return failures ? 1 : 0;
}
