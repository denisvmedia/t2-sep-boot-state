/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Copyright (C) 2026 Denis Vojtuk
 *
 * The T2 SEP boot-state transaction.
 *
 * This part has no kernel dependencies. The kernel module and the unit tests
 * build the same file, and each supplies its own way to reach the SMC.
 */
#ifndef T2_SEP_BOOT_STATE_H
#define T2_SEP_BOOT_STATE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

/* SMC keys: four ASCII characters, the first one in the top byte. */
#define T2BS_KEY_EFMV		0x45464d56u
#define T2BS_KEY_EFMS		0x45464d53u
#define T2BS_KEY_EFBS		0x45464253u

#define T2BS_EFMV_LEN		32
#define T2BS_STATE_LEN		1
#define T2BS_EFMV_PRISTINE	0xfe
#define T2BS_EFMV_FORMAT	1
#define T2BS_EFBS_COMMIT	0x11
#define T2BS_TIMEOUT_MS		40000
#define T2BS_POLL_MS		10
#define T2BS_VERSION_FIELDS	6
/* Room for "response-received:255" and the terminating NUL. */
#define T2BS_RESULT_TEXT_LEN	24

enum t2bs_state {
	T2BS_DISABLED,
	T2BS_IN_PROGRESS,
	T2BS_NOT_APPLICABLE,
	T2BS_READY,
	T2BS_REJECTED,
	T2BS_FAILED,
	T2BS_COMMIT_UNCERTAIN,
	T2BS_RESPONSE_RECEIVED,
};

/**
 * struct t2bs_result - outcome of the transaction
 * @state: what happened
 * @response: the EFMS value, valid for T2BS_RESPONSE_RECEIVED
 * @error: zero or a negative errno that explains @state
 * @wrote: a write to the SMC was attempted. Once this is set, the transaction
 *	must not run again before the next boot.
 */
struct t2bs_result {
	enum t2bs_state state;
	u8 response;
	int error;
	bool wrote;
};

/**
 * struct t2bs_smc_ops - how the transaction reaches the SMC and the clock
 * @key_size: store the size of @key in @size
 * @read: read exactly @len bytes of @key into @buf
 * @write: write exactly @len bytes from @buf to @key
 * @now_ms: a monotonic clock in milliseconds
 * @sleep_ms: sleep for about @ms milliseconds
 *
 * The first three return zero on success and a negative errno on failure.
 */
struct t2bs_smc_ops {
	int (*key_size)(void *ctx, u32 key, u8 *size);
	int (*read)(void *ctx, u32 key, u8 *buf, size_t len);
	int (*write)(void *ctx, u32 key, const u8 *buf, size_t len);
	u64 (*now_ms)(void *ctx);
	void (*sleep_ms)(void *ctx, unsigned int ms);
};

u8 t2bs_key_info_size(u8 size, u8 flags);
int t2bs_parse_bridge_version(const char *bios_version,
			      u32 version[T2BS_VERSION_FIELDS]);
void t2bs_build_efmv(u8 efmv[T2BS_EFMV_LEN],
		     const u32 version[T2BS_VERSION_FIELDS],
		     u8 epoch_major, u8 epoch_minor);
void t2bs_publish(const struct t2bs_smc_ops *ops, void *ctx,
		  const u32 version[T2BS_VERSION_FIELDS],
		  u8 epoch_major, u8 epoch_minor, bool commit,
		  struct t2bs_result *result);
int t2bs_format_result(const struct t2bs_result *result, char *buf, size_t len);

#endif /* T2_SEP_BOOT_STATE_H */
