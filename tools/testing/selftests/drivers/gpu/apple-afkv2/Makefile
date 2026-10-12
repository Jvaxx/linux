# SPDX-License-Identifier: GPL-2.0-only
HOSTCC ?= cc
export HOSTCC
export AFKV2_SOURCE_ROOT := $(abspath ../../../../../..)

TEST_PROGS := run-afkv2-tests.sh
TEST_FILES := codec.c owned-prefix.c owned-suffix.c extract-owned.py host-include

include ../../../lib.mk
