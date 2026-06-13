# SPDX-License-Identifier: GPL-2.0
#
# (C) COPYRIGHT 2010-2018, 2022 ARM Limited. All rights reserved.
#
# This program is free software and is provided to you under the terms of the
# GNU General Public License version 2 as published by the Free Software
# Foundation, and any use by you of this program is subject to the terms
# of such GNU license.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, you can access it online at
# http://www.gnu.org/licenses/gpl-2.0.html.
#
#

KDIR ?= /lib/modules/$(shell uname -r)/build

#
# Default configuration values
#
# Dependency resolution is done through statements as Kconfig
# is not supported for out-of-tree builds.
#

CONFIG_MALI_MIDGARD ?= m
ifeq ($(CONFIG_MALI_MIDGARD), m)
	CONFIG_MALI_PLATFORM_NAME ?= "devicetree"
	CONFIG_MALI_GATOR_SUPPORT ?= y
	CONFIG_MALI_DEVFREQ ?= y
	CONFIG_MALI_EXPERT ?= y
else
	CONFIG_MALI_PLATFORM_NAME = "devicetree"
	CONFIG_MALI_GATOR_SUPPORT = n
	CONFIG_MALI_DEVFREQ = n
	CONFIG_MALI_EXPERT = n
endif

#
# Expert/Debug released configurations
#
ifeq ($(CONFIG_MALI_EXPERT), y)
	CONFIG_MALI_CORESTACK ?= n
	CONFIG_MALI_DEBUG ?= n
	ifeq ($(CONFIG_MALI_DEBUG), y)
		CONFIG_MALI_FENCE_DEBUG ?= y
	else
		CONFIG_MALI_FENCE_DEBUG = n
	endif
	CONFIG_MALI_NO_MALI ?= n
	ifeq ($(CONFIG_MALI_NO_MALI), y)
		CONFIG_MALI_ERROR_INJECT ?= n
	else
		CONFIG_MALI_ERROR_INJECT = n
	endif
	CONFIG_MALI_SYSTEM_TRACE ?= n
	CONFIG_MALI_2MB_ALLOC ?= n
	CONFIG_MALI_PWRSOFT_765 ?= n
	CONFIG_MALI_JOB_DUMP ?= n
	CONFIG_MALI_PRFCNT_SET_SECONDARY ?= n
else
	CONFIG_MALI_CORESTACK = n
	CONFIG_MALI_DEBUG = n
	CONFIG_MALI_FENCE_DEBUG = n
	CONFIG_MALI_NO_MALI = n
	CONFIG_MALI_ERROR_INJECT = n
	CONFIG_MALI_SYSTEM_TRACE = n
	CONFIG_MALI_2MB_ALLOC = n
	CONFIG_MALI_PWRSOFT_765 = n
	CONFIG_MALI_JOB_DUMP = n
	CONFIG_MALI_PRFCNT_SET_SECONDARY = n
endif

#
# Test released configurations
#
ifeq ($(CONFIG_MALI_DEBUG), y)
	CONFIG_MALI_KUTF ?= m
else
	CONFIG_MALI_KUTF = n
endif

ifneq ($(CONFIG_MALI_KUTF), n)
	CONFIG_MALI_IRQ_LATENCY ?= m
else
	CONFIG_MALI_IRQ_LATENCY = n
endif

CONFIGS = \
	CONFIG_MALI_MIDGARD \
	CONFIG_MALI_GATOR_SUPPORT \
	CONFIG_MALI_DEVFREQ \
	CONFIG_MALI_DMA_FENCE \
	CONFIG_MALI_EXPERT \
	CONFIG_MALI_CORESTACK \
	CONFIG_MALI_DEBUG \
	CONFIG_MALI_FENCE_DEBUG \
	CONFIG_MALI_NO_MALI \
	CONFIG_MALI_ERROR_INJECT \
	CONFIG_MALI_SYSTEM_TRACE \
	CONFIG_MALI_2MB_ALLOC \
	CONFIG_MALI_PWRSOFT_765 \
	CONFIG_MALI_JOB_DUMP \
	CONFIG_MALI_PRFCNT_SET_SECONDARY \
	CONFIG_MALI_KUTF \
	CONFIG_MALI_IRQ_LATENCY

#
# MAKE_ARGS to pass the custom CONFIGs on out-of-tree build
#
# Generate the list of CONFIGs and values.
# $(value config) is the name of the CONFIG option.
# $(value $(value config)) is its value (y, m).
# When the CONFIG is not set to y or m, it defaults to n.
MAKE_ARGS := $(foreach config,$(CONFIGS), \
			$(if $(filter y m,$(value $(value config))), \
				$(value config)=$(value $(value config)), \
				$(value config)=n))

MAKE_ARGS += \
	CONFIG_MALI_PLATFORM_NAME=$(CONFIG_MALI_PLATFORM_NAME) \

#
# EXTRA_CFLAGS to define the custom CONFIGs on out-of-tree build
#
# Generate the list of CONFIGs defines with values from CONFIGS.
# $(value config) is the name of the CONFIG option.
# When set to y or m, the CONFIG gets defined to 1.
EXTRA_CFLAGS := $(foreach config, $(CONFIGS), \
			$(if $(filter y m,$(value $(value config))),\
				-D$(value config)=1))

EXTRA_CFLAGS += -DCONFIG_MALI_PLATFORM_NAME=$(CONFIG_MALI_PLATFORM_NAME)

# we get the symbols from modules using KBUILD_EXTRA_SYMBOLS to prevent warnings about unknown functions
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) EXTRA_CFLAGS="$(EXTRA_CFLAGS)" KBUILD_EXTRA_SYMBOLS="$(EXTRA_SYMBOLS)" $(MAKE_ARGS) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
