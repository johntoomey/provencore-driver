
obj-$(CONFIG_PROVENCORE_SHARED_DEVICES) += pnc_shdev.o
pnc_shdev-y := shdev.o
pnc_shdev-$(CONFIG_PROVENCORE_SHARED_MMC) += mmcblk.o
pnc_shdev-$(CONFIG_PROVENCORE_SHARED_ENET) += enetdev.o
pnc_shdev-$(CONFIG_PROVENCORE_SHARED_SPI) += spidev.o

ccflags-y := \
	-Wno-declaration-after-statement \
	-Wno-unused-function \
	-Wno-error=date-time

CC_VERSION := $(shell $(CC) -dumpversion)
IS_VER_GE = $(shell printf "$2\n$1" | sort --version-sort --check=quiet && echo YES)

ifeq ($(ARCH), arm64)
ifeq ($(call IS_VER_GE,$(CC_VERSION),10.1), YES)
ccflags-y += -mno-outline-atomics
endif
endif

#ccflags-y := -DDEBUG
