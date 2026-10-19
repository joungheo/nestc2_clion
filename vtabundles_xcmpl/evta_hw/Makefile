# #### JO added (2026-10-17): build EVTA bitstreams for the ZCU102 with Vivado / Vivado HLS 2020.1.
# The HLS IP depends only on the buffer config (CFG); the bitstream also on NUM_EVTA.
#   make ip                                   # HLS: fetch/load/compute/store IP (once per CFG)
#   make bit NUM_EVTA=1                       # Vivado: build/<cfg>/vivado_1evta/export/<cfg>_1evta.bit (+ .xsa)
#   make bin NUM_EVTA=1                       # .bin for /lib/firmware (also possible on the board: hwtest/bit2bin.py)
#   make bit CFG=evta_zcu102_small.json NUM_EVTA=4
# Variables: CFG (evta_zcu102.json), NUM_EVTA (1), BOARD_REV (3.3), FREQ (333), JOBS (8), CSIM (0)

CFG        ?= evta_zcu102.json
NUM_EVTA   ?= 1
BOARD_REV  ?= 3.3
FREQ       ?= 333
JOBS       ?= 8
CSIM       ?= 0
VIVADO_HLS ?= vivado_hls
VIVADO     ?= vivado
PYTHON     ?= python3

NAME    := $(basename $(notdir $(CFG)))
BUILD   := $(CURDIR)/build/$(NAME)
IP_DIR  := $(BUILD)/hls
IP_CFG  := $(BUILD)/config_hls.tcl
BIT_DIR := $(BUILD)/vivado_$(NUM_EVTA)evta
BIT_CFG := $(BIT_DIR)/config.tcl
BIT     := $(BIT_DIR)/export/$(NAME)_$(NUM_EVTA)evta.bit
HW_SRC  := $(wildcard ../../tvm/3rdparty/vta-hw/hardware/xilinx/src/*) ../../vta/vtalib/include/zcu102/vta/hw_spec.h

.PHONY: all config ip bit bin clean
all: bit
config: $(IP_CFG)
ip: $(IP_DIR)/.done
bit: $(BIT)
bin: $(BIT:.bit=.bin)

$(IP_CFG): $(CFG) gen_config.py
	mkdir -p $(BUILD)
	$(PYTHON) gen_config.py $(CFG) $@ --board-rev $(BOARD_REV) --freq $(FREQ)

$(IP_DIR)/.done: $(IP_CFG) hls.tcl $(HW_SRC)
	mkdir -p $(IP_DIR)
	cd $(IP_DIR) && $(VIVADO_HLS) -f $(CURDIR)/hls.tcl -tclargs $(IP_CFG) $(CSIM)
	touch $@

$(BIT): $(IP_DIR)/.done vivado.tcl gen_config.py
	mkdir -p $(BIT_DIR)
	$(PYTHON) gen_config.py $(CFG) $(BIT_CFG) --num-evta $(NUM_EVTA) --board-rev $(BOARD_REV) --freq $(FREQ)
	cd $(BIT_DIR) && $(VIVADO) -mode batch -source $(CURDIR)/vivado.tcl -tclargs $(IP_DIR) $(BIT_CFG) $(JOBS)

$(BIT:.bit=.bin): $(BIT)
	$(PYTHON) ../hwtest/bit2bin.py $< $@

clean:
	rm -rf build
