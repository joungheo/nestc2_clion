# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

# Directories
ROOTDIR = $(CURDIR)
VTA_HW_DIR = $(CURDIR)/../..
BUILD_DIR = $(VTA_HW_DIR)/build/hardware/xilinx
SCRIPT_DIR = $(CURDIR)/scripts
SRC_DIR = $(CURDIR)/src

# Executables
VIVADO_HLS = vivado_hls
VIVADO = vivado
#### JO added: python3 (the Vivado docker may have no `python`), CSIM=1 runs the HLS C simulation first,
#### JOBS = Vivado implementation threads
PYTHON ?= python3
CSIM ?= 0
JOBS ?= 8
#### JO added: PROFILE=1 adds the compute profiling counters (HLS) and the AXI perf monitor (Vivado);
#### built in separate <CONF>_prof dirs and exported as vtahw_<CONF>_prof
PROFILE ?= 0
#### JO added: number of EVTA cores in the bitstream (1-4, one PS HP port each)
NUM_EVTA ?= 4

# Process VTA JSON config
VTA_CONFIG := $(VTA_HW_DIR)/config/vta_config.py
#### JO added: everything the config tcl is derived from
VTA_CONFIG_DEPS := $(VTA_CONFIG) $(VTA_HW_DIR)/config/pkg_config.py $(VTA_HW_DIR)/config/vta_config.json

# Derive config name
#CONF := $(shell python ${VTA_CONFIG} --cfg-str)
CONF := $(shell $(PYTHON) ${VTA_CONFIG} --cfg-str)
#### JO changed: _prof suffix with PROFILE=1
#IP_BUILD_PATH := $(BUILD_DIR)/hls/$(CONF)
#HW_BUILD_PATH := $(BUILD_DIR)/vivado/$(CONF)
#### JO changed: + _<N>evta
#BUILD_NAME := $(CONF)$(if $(filter 1,$(PROFILE)),_prof)
#BUILD_NAME := $(CONF)_$(NUM_EVTA)evta$(if $(filter 1,$(PROFILE)),_prof)
#IP_BUILD_PATH := $(BUILD_DIR)/hls/$(BUILD_NAME)
#### JO changed (2026-10-27): FREQ=<MHz> overrides the PLL frequency of the config (same IP, _<FREQ>mhz vivado dir)
FREQ ?=
#### JO changed (2026-10-28, systolic Phase D, docs/evta_systolic_design.md): SYSTOLIC=1 -> GEMM in the RTL unit gemm_sa per EVTA
#### (RTL from vta-hw-zcu102-systolic/hardware/xilinx/rtl, _sa dirs), IMPL_STRATEGY / BUILD_TAG as in that tree
SYSTOLIC ?= 0
IMPL_STRATEGY ?=
BUILD_TAG ?=
#IP_NAME := $(CONF)_$(NUM_EVTA)evta$(if $(filter 1,$(PROFILE)),_prof)
#BUILD_NAME := $(IP_NAME)$(if $(FREQ),_$(FREQ)mhz)
IP_NAME := $(CONF)_$(NUM_EVTA)evta$(if $(filter 1,$(PROFILE)),_prof)$(if $(filter 1,$(SYSTOLIC)),_sa)
BUILD_NAME := $(IP_NAME)$(if $(FREQ),_$(FREQ)mhz)$(if $(BUILD_TAG),_$(BUILD_TAG))
export VTA_SYSTOLIC := $(SYSTOLIC)
export VTA_IMPL_STRATEGY := $(IMPL_STRATEGY)
SA_RTL_DIR := $(abspath $(CURDIR)/../../../vta-hw-zcu102-systolic/hardware/xilinx/rtl)
export VTA_SA_RTL := $(SA_RTL_DIR)
SA_RTL := $(if $(filter 1,$(SYSTOLIC)),$(wildcard $(SA_RTL_DIR)/*.v))
IP_BUILD_PATH := $(BUILD_DIR)/hls/$(IP_NAME)
export VTA_FREQ := $(FREQ)
HW_BUILD_PATH := $(BUILD_DIR)/vivado/$(BUILD_NAME)

# Config files
#### JO changed: one config tcl per CONF, so a different vta_config.json does not reuse a stale one
#CONFIG_TCL = $(BUILD_DIR)/include/vta_config.tcl
CONFIG_TCL = $(BUILD_DIR)/include/$(CONF)/vta_config.tcl

# IP file path
#### JO changed: a stamp written after all four stages are exported (was the compute zip only)
#IP_PATH := $(BUILD_DIR)/hls/$(CONF)/vta_compute/soln/impl/ip/xilinx_com_hls_compute_1_0.zip
IP_PATH := $(IP_BUILD_PATH)/.done
IP_ZIPS := $(foreach s,fetch load compute store,$(IP_BUILD_PATH)/vta_$(s)/soln/impl/ip/xilinx_com_hls_$(s)_1_0.zip)

# Bitstream file path
#### JO changed: export name vtahw_<CONF>, so the .bin never overwrites the bitstream in use on the board
#### (/lib/firmware/zcu102_1x16_i8w8a32_16_16_19_18.bin has the same CONF string)
#BIT_PATH := $(BUILD_DIR)/vivado/$(CONF)/export/$(CONF).bit
EXPORT_NAME ?= vtahw_$(BUILD_NAME)
BIT_PATH := $(HW_BUILD_PATH)/export/$(EXPORT_NAME).bit
#### JO added: .bin for /lib/firmware (fpga_manager)
BIN_PATH := $(BIT_PATH:.bit=.bin)

#.PHONY: all ip bit clean clean_all
.PHONY: all ip bit bin clean cleanall

all: bit
ip: $(IP_PATH)
bit: $(BIT_PATH)
bin: $(BIN_PATH)

#### JO changed: order-only dependency on the dir, config tcl rebuilt when its inputs change
#$(BUILD_DIR)/include:
#	mkdir -p $@
#
#$(CONFIG_TCL): | $(BUILD_DIR)/include
#	python $(VTA_CONFIG) --export-tcl $@
$(CONFIG_TCL): $(VTA_CONFIG_DEPS) $(VTA_HW_DIR)/include/vta/hw_spec.h
	mkdir -p $(dir $@)
	$(PYTHON) $(VTA_CONFIG) --export-tcl $@

#### JO changed: also depends on hw_spec.h and hls.tcl; fails unless all four IPs exist
#$(IP_PATH): $(SRC_DIR)/* $(CONFIG_TCL)
$(IP_PATH): $(SRC_DIR)/* $(VTA_HW_DIR)/include/vta/hw_spec.h $(SCRIPT_DIR)/hls.tcl $(CONFIG_TCL)
	mkdir -p $(IP_BUILD_PATH)
	cd $(IP_BUILD_PATH) && \
		$(VIVADO_HLS) \
		-f $(SCRIPT_DIR)/hls.tcl \
		-tclargs \
			$(VTA_HW_DIR) \
			$(CONFIG_TCL) \
			$(CSIM) \
			$(PROFILE)
	ls $(IP_ZIPS)
	touch $@

#### JO changed: batch mode, export name = EXPORT_NAME, JOBS threads; rebuilt when vivado.tcl changes
#$(BIT_PATH): $(IP_PATH)
$(BIT_PATH): $(IP_PATH) $(SCRIPT_DIR)/vivado.tcl $(SA_RTL)
	mkdir -p $(HW_BUILD_PATH)
	cd $(HW_BUILD_PATH) && \
		$(VIVADO) \
		-mode batch \
		-source $(SCRIPT_DIR)/vivado.tcl \
		-tclargs \
			$(IP_BUILD_PATH) \
			$(CONFIG_TCL) \
			$(NUM_EVTA) \
			$(EXPORT_NAME) \
			$(JOBS) \
			$(PROFILE)

#### JO added
$(BIN_PATH): $(BIT_PATH)
	$(PYTHON) $(SCRIPT_DIR)/bit2bin.py $< $@

clean:
	rm -rf *.out *.log

cleanall: clean
	rm -rf $(BUILD_DIR)
