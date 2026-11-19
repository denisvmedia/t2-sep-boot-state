# t2-sep-boot-state

A small Linux kernel module for Intel Macs with the Apple T2 chip. It tells the Secure Enclave (SEP) inside the T2 chip that a system has booted, the way Apple's firmware does for macOS. Touch ID on Linux needs this: without it, the SEP does not serve fingerprint requests.

The module is for kernels where the `macsmc` drivers own the System Management Controller (SMC). On t2linux kernels that is Linux 7.3 and later.

**Status: experimental.** It writes to SMC keys that the Secure Enclave reads. Read [Risks](#risks) first.

| Check | Result |
|---|---|
| Unit tests of the transaction, with memory sanitizers | pass |
| Build for t2linux 7.3.4-3 and 7.3.5-1 | pass |
| `probe=1` on a MacBookAir9,1, bridgeOS 24.16.6005 | `ready` |
| `publish=1` on real hardware | not run yet |

## Why a module

The write goes to three SMC keys: `EFMV`, `EFMS` and `EFBS`.

- Up to Linux 7.2, the `applesmc` driver owns the SMC. [t2touch](https://github.com/macintog/t2touch) replaces it with a patched copy that does the write.
- Linux 7.3 removed the kernel interface that `applesmc` is built on. On t2linux kernels the `macsmc` drivers took over, and `applesmc` is gone.
- `macsmc` offers `/dev/smc` to programs. In the t2linux 7.3.5-1 kernel, writes through it are limited to a short list of keys, and these three are not on it.

So this module does the write inside the kernel, through the functions that `macsmc` exports to other drivers.

## What it does

When the module loads with `publish=1`, it runs one transaction:

1. Read the bridgeOS version from the firmware version string, for example `(iBridge: 24.16.6005.0.0,0)`.
2. Check that `EFMV` is 32 bytes and that `EFMS` and `EFBS` are 1 byte each.
3. Check that `EFMV` holds `fe` followed by zeros. A boot that is not macOS leaves it like that. Anything else means a record is already there, and the module stops.
4. Write the version record to `EFMV`, write `0` to `EFMS`, then write `0x11` to `EFBS`.
5. Read `EFMS` every 10 ms, for up to 40 seconds, until the T2 chip answers.

Nothing is written unless steps 1 to 3 pass. After a write, the module stays loaded until the next boot, so the transaction cannot run twice.

The sequence, the values and the result names come from the `applesmc` patch of t2touch.

## Risks

- The transaction changes state in the Secure Enclave. Nobody outside Apple has documented these keys.
- When a write or the wait for the answer fails, the outcome is unknown (`commit-uncertain`). Do not try again in the same boot. Reboot first.
- t2touch warns against using it on a Mac that also runs macOS, see its [safety notice](https://github.com/macintog/t2touch/issues/16). The same caution applies here.
- The module is tested on one machine so far, see the table above.

There is no warranty. You use it at your own risk.

## Requirements

- An Intel Mac with a T2 chip.
- A kernel with the `macsmc` ACPI backend (`CONFIG_MACSMC_ACPI`), such as a [t2linux](https://t2linux.org) kernel 7.3 or later.
- The headers of that kernel, `gcc`, `make` and `dkms`.

## Install

```sh
git clone https://github.com/denisvmedia/t2-sep-boot-state.git
cd t2-sep-boot-state
make test                # optional: the unit tests
sudo make dkms-install   # build and install through DKMS
```

DKMS rebuilds the module for new kernels by itself. `dkms.conf` limits it to kernel series that were tested (`BUILD_EXCLUSIVE_KERNEL`) and to kernels that have `macsmc`. DKMS skips every other kernel, and a skipped build cannot stop a kernel update. To use the module on a newer kernel series, add the series to `BUILD_EXCLUSIVE_KERNEL` after a test build.

To remove it: `sudo make dkms-remove`.

## Use

Check that the transaction could run. This writes nothing, and the module can be unloaded afterwards:

```sh
sudo modprobe t2_sep_boot_state probe=1
cat /sys/module/t2_sep_boot_state/parameters/result    # ready
sudo modprobe -r t2_sep_boot_state
```

Run the transaction once in this boot:

```sh
sudo modprobe t2_sep_boot_state publish=1
cat /sys/module/t2_sep_boot_state/parameters/result    # response-received:1
```

To run it at every boot, put the option into `/etc/modprobe.d/t2-sep-boot-state.conf` and load the module before whatever talks to the SEP. For t2touch that is the `t2_sep_transport` module:

```
options t2_sep_boot_state publish=1
softdep t2_sep_transport pre: t2_sep_boot_state
```

### Parameters

| Parameter | Default | Meaning |
|---|---|---|
| `publish` | off | Run the transaction once when the module loads |
| `probe` | off | Check that the transaction could run, and write nothing |
| `epoch_major` | 1 | Epoch major byte of the version record |
| `epoch_minor` | 0 | Epoch minor byte of the version record |

### Results

`/sys/module/t2_sep_boot_state/parameters/result` holds one of these:

| Result | Meaning | Written? |
|---|---|---|
| `disabled` | Loaded without `publish=1` or `probe=1` | no |
| `in-progress` | The transaction is running | maybe |
| `not-applicable` | No T2 chip, or `macsmc` does not own the SMC | no |
| `ready` | `probe=1`: all checks passed | no |
| `rejected` | A check failed: a key has the wrong size, `EFMV` already holds a record, or a parameter is out of range | no |
| `failed` | The SMC returned an error during the checks | no |
| `commit-uncertain` | A write or the wait for the answer failed. Reboot before another try | yes |
| `response-received:N` | The T2 chip answered with `N`. t2touch treats `1` as the normal answer and `3` as a request to reboot | yes |

The kernel log (`dmesg`) has one line with the same outcome and the error code.

## How the code guards against memory errors

A kernel module in Rust is not possible here: the t2linux kernels are built without Rust support. The C code is small, and it is written to leave little room for mistakes:

- **No data from user space.** The module runs one fixed transaction. Its only inputs are the parameters above.
- **No dynamic memory.** It uses two buffers of fixed size, 32 bytes and 1 byte.
- **The transaction is separate from the kernel.** `src/boot_state.c` has no kernel dependencies. The unit tests build the same file as a normal program, with the address and undefined-behavior sanitizers, and run it against a fake SMC. They cover every way out of the transaction: each failed check, each failed write, a read error and a timeout while waiting, and the answers. They also feed the version parser 200,000 valid and damaged strings.
- **The SMC handle is checked.** `src/main.c` takes it only from the `APP0001` device while `macsmc-acpi` is bound to it, and holds the device lock for the whole transaction.

```sh
make test       # unit tests with sanitizers
make analyze    # gcc's static analyzer on the transaction
make W=1        # the module, with extra kernel warnings
```

## License and credits

Your choice of GPL-2.0-only ([LICENSE-GPL-2.0](LICENSE-GPL-2.0)) or MIT ([LICENSE-MIT](LICENSE-MIT)). The `macsmc` drivers use the same pair.

The code in this repository was written for it. The transaction it performs (the order of the writes, the values and the result names) was learned from the `applesmc` patch of t2touch, which is GPL-2.0-only.

- [t2touch](https://github.com/macintog/t2touch) worked out the transaction and has run it through `applesmc`.
- [t2linux](https://t2linux.org) brought the `macsmc` drivers to T2 Macs.
- The [Asahi Linux](https://asahilinux.org) project wrote `macsmc`.
