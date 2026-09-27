# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_OPTEE) += optee.o
optee-objs += core.o
optee-objs += call.o
optee-objs += notif.o
optee-objs += rpc.o
optee-objs += protmem.o
optee-objs += supp.o
optee-objs += device.o
optee-objs += smc_abi.o
optee-objs += ffa_abi.o
optee-$(CONFIG_OPTEE_TEELOG) += teelog.o
optee-$(CONFIG_OPTEE_TEELOG) += teelog_dev.o
optee-$(CONFIG_OPTEE_TEELOG) += teelog_ring_read.o

# ring_read.o is linked only via optee.o (exported). Do not also put it in
# teelog_kunit.o — built-in y+y would multiple-define the same symbols.
obj-$(CONFIG_OPTEE_TEELOG_KUNIT_TEST) += teelog_kunit.o
teelog_kunit-objs := teelog_test.o

# for tracing framework to find optee_trace.h
CFLAGS_smc_abi.o := -I$(src)
