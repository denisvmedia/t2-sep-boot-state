// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Copyright (C) 2026 Denis Vojtuk
 *
 * t2-sep-boot-state: tell the Secure Enclave of an Apple T2 chip that the
 * system has booted, on kernels where the macsmc drivers own the SMC.
 *
 * The transaction itself lives in boot_state.c. This file finds the SMC,
 * runs the transaction once when the module loads with publish=1, and shows
 * the outcome in /sys/module/t2_sep_boot_state/parameters/result. With probe=1
 * it only checks that the transaction could run, and writes nothing.
 *
 * The module takes no data from user space apart from its parameters.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/build_bug.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/ktime.h>
#include <linux/limits.h>
#include <linux/mfd/macsmc.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include "boot_state.h"

/* The SMC is the ACPI device APP0001, bound to the macsmc ACPI backend. */
#define T2BS_SMC_DEVICE_PREFIX	"APP0001:"
#define T2BS_SMC_DRIVER		"macsmc-acpi"

static bool publish;
module_param(publish, bool, 0444);
MODULE_PARM_DESC(publish, "Run the boot-state transaction once when the module loads");

static bool probe;
module_param(probe, bool, 0444);
MODULE_PARM_DESC(probe, "Check that the transaction could run, and write nothing");

static unsigned int epoch_major = 1;
module_param(epoch_major, uint, 0444);
MODULE_PARM_DESC(epoch_major, "Epoch major byte of the version record (0-255)");

static unsigned int epoch_minor;
module_param(epoch_minor, uint, 0444);
MODULE_PARM_DESC(epoch_minor, "Epoch minor byte of the version record (0-255)");

/* The result file can be read while the module is still loading. */
static DEFINE_MUTEX(t2bs_result_lock);
static struct t2bs_result t2bs_result = { .state = T2BS_DISABLED };

static void t2bs_store_result(const struct t2bs_result *result)
{
	mutex_lock(&t2bs_result_lock);
	t2bs_result = *result;
	mutex_unlock(&t2bs_result_lock);
}

static int t2bs_result_get(char *buffer, const struct kernel_param *kp)
{
	char text[T2BS_RESULT_TEXT_LEN];
	struct t2bs_result snapshot;
	int len;

	mutex_lock(&t2bs_result_lock);
	snapshot = t2bs_result;
	mutex_unlock(&t2bs_result_lock);

	len = t2bs_format_result(&snapshot, text, sizeof(text));
	if (len < 0)
		return len;
	return sysfs_emit(buffer, "%s\n", text);
}

static const struct kernel_param_ops t2bs_result_ops = {
	.get = t2bs_result_get,
};
module_param_cb(result, &t2bs_result_ops, NULL, 0444);
MODULE_PARM_DESC(result, "Outcome of the transaction (read-only)");

/*
 * The macsmc backends return the byte count on success, while the header
 * documents zero. Accept both, and treat any other count as an I/O error.
 */
static int t2bs_smc_status(int ret, size_t len)
{
	if (ret < 0)
		return ret;
	if (ret != 0 && (size_t)ret != len)
		return -EIO;
	return 0;
}

static int t2bs_smc_key_size(void *ctx, u32 key, u8 *size)
{
	struct apple_smc_key_info info = { };
	int ret;

	ret = apple_smc_get_key_info(ctx, key, &info);
	if (ret < 0)
		return ret;

	*size = info.size;
	return 0;
}

static int t2bs_smc_read(void *ctx, u32 key, u8 *buf, size_t len)
{
	return t2bs_smc_status(apple_smc_read(ctx, key, buf, len), len);
}

static int t2bs_smc_write(void *ctx, u32 key, const u8 *buf, size_t len)
{
	return t2bs_smc_status(apple_smc_write(ctx, key, buf, len), len);
}

static u64 t2bs_now_ms(void *ctx)
{
	return (u64)ktime_to_ms(ktime_get());
}

static void t2bs_sleep_ms(void *ctx, unsigned int ms)
{
	msleep(ms);
}

static const struct t2bs_smc_ops t2bs_kernel_ops = {
	.key_size = t2bs_smc_key_size,
	.read = t2bs_smc_read,
	.write = t2bs_smc_write,
	.now_ms = t2bs_now_ms,
	.sleep_ms = t2bs_sleep_ms,
};

static int t2bs_match_smc_device(struct device *dev, const void *data)
{
	return str_has_prefix(dev_name(dev), T2BS_SMC_DEVICE_PREFIX) != 0;
}

static void t2bs_run(u32 version[T2BS_VERSION_FIELDS], struct t2bs_result *result)
{
	struct apple_smc *smc = NULL;
	struct device *dev;
	int ret;

	if (epoch_major > U8_MAX || epoch_minor > U8_MAX) {
		result->state = T2BS_REJECTED;
		result->error = -ERANGE;
		return;
	}

	ret = t2bs_parse_bridge_version(dmi_get_system_info(DMI_BIOS_VERSION), version);
	if (ret) {
		/* -ENODEV: the firmware names no iBridge, so there is no T2 chip. */
		result->state = ret == -ENODEV ? T2BS_NOT_APPLICABLE : T2BS_REJECTED;
		result->error = ret;
		return;
	}

	dev = bus_find_device(&platform_bus_type, NULL, NULL, t2bs_match_smc_device);
	if (!dev) {
		result->state = T2BS_NOT_APPLICABLE;
		result->error = -ENODEV;
		return;
	}

	/*
	 * macsmc-acpi stores its struct apple_smc as the driver data of this
	 * device. The device lock keeps the driver bound, and the structure
	 * alive, until the transaction is over.
	 */
	device_lock(dev);
	if (device_is_bound(dev) && dev->driver &&
	    !strcmp(dev->driver->name, T2BS_SMC_DRIVER))
		smc = dev_get_drvdata(dev);

	if (smc && smc->dev == dev) {
		t2bs_publish(&t2bs_kernel_ops, smc, version,
			     (u8)epoch_major, (u8)epoch_minor, publish, result);
	} else {
		result->state = T2BS_NOT_APPLICABLE;
		result->error = -ENODEV;
	}
	device_unlock(dev);
	put_device(dev);
}

static int __init t2bs_init(void)
{
	struct t2bs_result result = { .state = T2BS_IN_PROGRESS };
	u32 version[T2BS_VERSION_FIELDS] = { 0 };
	char text[T2BS_RESULT_TEXT_LEN];

	BUILD_BUG_ON(T2BS_KEY_EFMV != SMC_KEY(EFMV));
	BUILD_BUG_ON(T2BS_KEY_EFMS != SMC_KEY(EFMS));
	BUILD_BUG_ON(T2BS_KEY_EFBS != SMC_KEY(EFBS));

	if (!publish && !probe) {
		pr_info("loaded without publish=1, nothing was written\n");
		return 0;
	}

	t2bs_store_result(&result);
	t2bs_run(version, &result);
	t2bs_store_result(&result);

	/*
	 * After a write the outcome must stay readable until the next boot,
	 * and a reload must not look like a fresh start. Keep the module.
	 */
	if (result.wrote)
		__module_get(THIS_MODULE);

	if (t2bs_format_result(&result, text, sizeof(text)) < 0)
		strscpy(text, "invalid", sizeof(text));

	switch (result.state) {
	case T2BS_RESPONSE_RECEIVED:
		pr_info("received T2 SEP boot-policy response %u for bridgeOS %u.%u.%u.%u.%u,%u epoch %u.%u\n",
			result.response, version[0], version[1], version[2],
			version[3], version[4], version[5], epoch_major, epoch_minor);
		break;
	case T2BS_READY:
		pr_info("the transaction could run for bridgeOS %u.%u.%u.%u.%u,%u; nothing was written\n",
			version[0], version[1], version[2], version[3], version[4], version[5]);
		break;
	case T2BS_COMMIT_UNCERTAIN:
		pr_err("the outcome of the transaction is uncertain (%d); do not retry before the next boot\n",
		       result.error);
		break;
	default:
		pr_warn("nothing was written: %s (%d)\n", text, result.error);
		break;
	}

	return 0;
}
module_init(t2bs_init);

static void __exit t2bs_exit(void)
{
}
module_exit(t2bs_exit);

MODULE_SOFTDEP("pre: macsmc-acpi");
MODULE_DESCRIPTION("Publish the Apple T2 SEP boot state through macsmc");
MODULE_AUTHOR("Denis Vojtuk");
MODULE_LICENSE("Dual MIT/GPL");
MODULE_VERSION("0.1.0");
