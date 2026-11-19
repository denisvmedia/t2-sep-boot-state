// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Copyright (C) 2026 Denis Vojtuk
 *
 * The T2 SEP boot-state transaction.
 *
 * Apple's EFI firmware tells the Secure Enclave (SEP) in the T2 chip which
 * system is booting. It does so through three SMC keys: it writes a version
 * record to EFMV, clears EFMS, commits with EFBS, and the T2 answers in EFMS.
 * A non-macOS boot leaves EFMV at a sentinel value, and the SEP then does not
 * serve Touch ID. This file repeats the firmware's transaction.
 *
 * The sequence, the values and the result names come from the applesmc
 * boot-state patch of the t2touch project (https://github.com/macintog/t2touch,
 * GPL-2.0-only).
 *
 * No allocation, no kernel API: the kernel module and the unit tests build
 * this same file.
 */
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#endif

#include "boot_state.h"

#define T2BS_ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))
#define T2BS_IBRIDGE_TAG	"(iBridge: "
/* Offset of the first version field in EFMV, and the size of one field. */
#define T2BS_EFMV_VERSION_AT	4
#define T2BS_EFMV_FIELD_LEN	4

_Static_assert(T2BS_EFMV_VERSION_AT + T2BS_VERSION_FIELDS * T2BS_EFMV_FIELD_LEN <=
	       T2BS_EFMV_LEN, "the version fields must fit into EFMV");

/**
 * t2bs_key_info_size - the size of a key, from the kernel's key information
 * @size: the size field of the key information
 * @flags: the flags field of the key information
 *
 * The t2linux 7.3.5-1 kernel reads the key information of a T2 Mac from the
 * wrong registers: the size comes back as zero, and the real size sits in the
 * flags field. No SMC key has a size of zero, so in that case the size is
 * taken from the flags. The caller still compares it with the size it expects.
 */
u8 t2bs_key_info_size(u8 size, u8 flags)
{
	return size ? size : flags;
}

/*
 * Parse one unsigned decimal number: digits only, at least one, no overflow.
 * Returns the position after the number, or NULL.
 */
static const char *t2bs_parse_u32(const char *s, u32 *value)
{
	const char *p = s;
	u64 v = 0;

	while (*p >= '0' && *p <= '9') {
		v = v * 10 + (u64)(*p - '0');
		if (v > 0xffffffffull)
			return NULL;
		p++;
	}
	if (p == s)
		return NULL;

	*value = (u32)v;
	return p;
}

/**
 * t2bs_parse_bridge_version - read the bridgeOS version from the BIOS version
 * @bios_version: the DMI BIOS version, "... (iBridge: a.b.c.d.e,f)"
 * @version: receives the six numbers, and only on success
 *
 * Returns zero, -ENODEV when the string names no iBridge (the machine has no
 * T2 chip), or -EINVAL when the version does not have the expected form.
 */
int t2bs_parse_bridge_version(const char *bios_version,
			      u32 version[T2BS_VERSION_FIELDS])
{
	static const char after[T2BS_VERSION_FIELDS] = { '.', '.', '.', '.', ',', ')' };
	u32 parsed[T2BS_VERSION_FIELDS];
	const char *p;
	unsigned int i;

	if (!bios_version)
		return -ENODEV;

	p = strstr(bios_version, T2BS_IBRIDGE_TAG);
	if (!p)
		return -ENODEV;
	p += sizeof(T2BS_IBRIDGE_TAG) - 1;

	for (i = 0; i < T2BS_VERSION_FIELDS; i++) {
		p = t2bs_parse_u32(p, &parsed[i]);
		if (!p || *p != after[i])
			return -EINVAL;
		p++;
	}
	/* Nothing may follow the closing parenthesis. */
	if (*p != '\0')
		return -EINVAL;

	memcpy(version, parsed, sizeof(parsed));
	return 0;
}

/**
 * t2bs_build_efmv - build the version record for EFMV
 * @efmv: receives the record
 * @version: the bridgeOS version
 * @epoch_major: epoch major byte
 * @epoch_minor: epoch minor byte
 *
 * Layout: format byte, epoch major, epoch minor, a zero byte, then the six
 * version numbers as little-endian 32-bit values. The rest stays zero.
 */
void t2bs_build_efmv(u8 efmv[T2BS_EFMV_LEN],
		     const u32 version[T2BS_VERSION_FIELDS],
		     u8 epoch_major, u8 epoch_minor)
{
	unsigned int i;

	memset(efmv, 0, T2BS_EFMV_LEN);
	efmv[0] = T2BS_EFMV_FORMAT;
	efmv[1] = epoch_major;
	efmv[2] = epoch_minor;
	for (i = 0; i < T2BS_VERSION_FIELDS; i++) {
		u8 *field = &efmv[T2BS_EFMV_VERSION_AT + i * T2BS_EFMV_FIELD_LEN];

		field[0] = (u8)(version[i] & 0xff);
		field[1] = (u8)((version[i] >> 8) & 0xff);
		field[2] = (u8)((version[i] >> 16) & 0xff);
		field[3] = (u8)((version[i] >> 24) & 0xff);
	}
}

/* A non-macOS boot leaves EFMV at 0xfe followed by zeros. */
static bool t2bs_efmv_is_pristine(const u8 efmv[T2BS_EFMV_LEN])
{
	unsigned int i;

	if (efmv[0] != T2BS_EFMV_PRISTINE)
		return false;
	for (i = 1; i < T2BS_EFMV_LEN; i++) {
		if (efmv[i])
			return false;
	}
	return true;
}

static void t2bs_finish(struct t2bs_result *result, enum t2bs_state state, int error)
{
	result->state = state;
	result->error = error;
}

/**
 * t2bs_publish - run the boot-state transaction once
 * @ops: access to the SMC and the clock
 * @ctx: passed to every @ops call
 * @version: the bridgeOS version
 * @epoch_major: epoch major byte
 * @epoch_minor: epoch minor byte
 * @commit: false checks the preconditions only and writes nothing
 * @result: receives the outcome
 *
 * Nothing is written unless all three keys have the expected size and EFMV
 * holds the sentinel of a non-macOS boot. That check keeps the function from
 * overwriting a record left by Apple's firmware and from running twice in one
 * boot. With @commit false the function stops here, with T2BS_READY.
 *
 * From the first write on, any failure leaves the outcome unknown
 * (T2BS_COMMIT_UNCERTAIN), and the caller must not try again before the next
 * boot.
 */
void t2bs_publish(const struct t2bs_smc_ops *ops, void *ctx,
		  const u32 version[T2BS_VERSION_FIELDS],
		  u8 epoch_major, u8 epoch_minor, bool commit,
		  struct t2bs_result *result)
{
	static const struct {
		u32 key;
		u8 len;
	} keys[] = {
		{ T2BS_KEY_EFMV, T2BS_EFMV_LEN },
		{ T2BS_KEY_EFMS, T2BS_STATE_LEN },
		{ T2BS_KEY_EFBS, T2BS_STATE_LEN },
	};
	const unsigned int max_polls = T2BS_TIMEOUT_MS / T2BS_POLL_MS;
	u8 efmv[T2BS_EFMV_LEN] = { 0 };
	const u8 clear = 0;
	const u8 commit_value = T2BS_EFBS_COMMIT;
	u8 state = 0;
	unsigned int i, polls;
	u64 start;
	int ret;

	result->response = 0;
	result->wrote = false;

	for (i = 0; i < T2BS_ARRAY_SIZE(keys); i++) {
		u8 size = 0;

		ret = ops->key_size(ctx, keys[i].key, &size);
		if (ret) {
			t2bs_finish(result, T2BS_FAILED, ret);
			return;
		}
		if (size != keys[i].len) {
			t2bs_finish(result, T2BS_REJECTED, -EPROTO);
			return;
		}
	}

	ret = ops->read(ctx, T2BS_KEY_EFMV, efmv, sizeof(efmv));
	if (ret) {
		t2bs_finish(result, T2BS_FAILED, ret);
		return;
	}
	if (!t2bs_efmv_is_pristine(efmv)) {
		t2bs_finish(result, T2BS_REJECTED, -EALREADY);
		return;
	}

	if (!commit) {
		t2bs_finish(result, T2BS_READY, 0);
		return;
	}

	t2bs_build_efmv(efmv, version, epoch_major, epoch_minor);

	result->wrote = true;
	ret = ops->write(ctx, T2BS_KEY_EFMV, efmv, sizeof(efmv));
	/* Apple's firmware clears the answer before it commits with EFBS. */
	if (!ret)
		ret = ops->write(ctx, T2BS_KEY_EFMS, &clear, T2BS_STATE_LEN);
	if (!ret)
		ret = ops->write(ctx, T2BS_KEY_EFBS, &commit_value, T2BS_STATE_LEN);
	if (ret) {
		t2bs_finish(result, T2BS_COMMIT_UNCERTAIN, ret);
		return;
	}

	/*
	 * Wait for the answer. The poll count is a second limit, so the loop
	 * ends even if the clock does not move.
	 */
	start = ops->now_ms(ctx);
	for (polls = 0; ; polls++) {
		ret = ops->read(ctx, T2BS_KEY_EFMS, &state, T2BS_STATE_LEN);
		if (ret) {
			t2bs_finish(result, T2BS_COMMIT_UNCERTAIN, ret);
			return;
		}
		if (state)
			break;
		if (polls >= max_polls || ops->now_ms(ctx) - start >= T2BS_TIMEOUT_MS) {
			t2bs_finish(result, T2BS_COMMIT_UNCERTAIN, -ETIMEDOUT);
			return;
		}
		ops->sleep_ms(ctx, T2BS_POLL_MS);
	}

	result->response = state;
	t2bs_finish(result, T2BS_RESPONSE_RECEIVED, 0);
}

/**
 * t2bs_format_result - write the outcome as text
 * @result: the outcome
 * @buf: receives the text and a terminating NUL
 * @len: size of @buf, T2BS_RESULT_TEXT_LEN is always enough
 *
 * The names are the ones the applesmc patch of t2touch used, plus
 * "in-progress" and "ready". Returns the length of the text, or -ENOSPC.
 */
int t2bs_format_result(const struct t2bs_result *result, char *buf, size_t len)
{
	static const char *const names[] = {
		[T2BS_DISABLED]		 = "disabled",
		[T2BS_IN_PROGRESS]	 = "in-progress",
		[T2BS_NOT_APPLICABLE]	 = "not-applicable",
		[T2BS_READY]		 = "ready",
		[T2BS_REJECTED]		 = "rejected",
		[T2BS_FAILED]		 = "failed",
		[T2BS_COMMIT_UNCERTAIN]	 = "commit-uncertain",
		[T2BS_RESPONSE_RECEIVED] = "response-received:",
	};
	const unsigned int state = (unsigned int)result->state;
	const char *name = "invalid";
	char digits[3];
	size_t n, ndigits = 0;

	if (state < T2BS_ARRAY_SIZE(names) && names[state])
		name = names[state];
	n = strlen(name);

	if (state == T2BS_RESPONSE_RECEIVED) {
		unsigned int value = result->response;

		/* Least significant digit first. */
		do {
			digits[ndigits++] = (char)('0' + value % 10);
			value /= 10;
		} while (value && ndigits < sizeof(digits));
	}

	if (len < n + ndigits + 1)
		return -ENOSPC;

	memcpy(buf, name, n);
	while (ndigits)
		buf[n++] = digits[--ndigits];
	buf[n] = '\0';
	return (int)n;
}
