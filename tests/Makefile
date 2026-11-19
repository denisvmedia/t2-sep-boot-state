# SPDX-License-Identifier: GPL-2.0-only
# Unit tests for the transaction. They build the same boot_state.c as the
# kernel module, with the address and undefined-behavior sanitizers.
CC ?= cc
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wconversion -Wshadow \
	  -fno-omit-frame-pointer -fsanitize=address,undefined \
	  -fno-sanitize-recover=all -I../src

.PHONY: all clean
all: test_boot_state
	./test_boot_state

test_boot_state: test_boot_state.c ../src/boot_state.c ../src/boot_state.h
	$(CC) $(CFLAGS) -o $@ test_boot_state.c ../src/boot_state.c

clean:
	$(RM) test_boot_state
