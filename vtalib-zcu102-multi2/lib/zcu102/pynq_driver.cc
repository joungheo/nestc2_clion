/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 * \file pynq_driver.c
 * \brief VTA driver for Zynq SoC boards with Pynq support (see pynq.io).
 */

#include <vta/driver.h>
#include <thread>
#include "pynq_driver.h"
#include <sys/time.h>


void* VTAMemAlloc(size_t size, int cached) {
  assert(size <= VTA_MAX_XFER);
  // Rely on the pynq-specific cma library
  return cma_alloc(size, cached);
}

void VTAMemFree(void* buf) {
  // Rely on the pynq-specific cma library
  cma_free(buf);
}

vta_phy_addr_t VTAMemGetPhyAddr(void* buf) {
  return cma_get_phy_addr(buf);
}

void VTAMemCopyFromHost(void* dst, const void* src, size_t size) {
  // For SoC-based FPGAs that used shared memory with the CPU, use memcopy()
  memcpy(dst, src, size);
}

void VTAMemCopyToHost(void* dst, const void* src, size_t size) {
  // For SoC-based FPGAs that used shared memory with the CPU, use memcopy()
  memcpy(dst, src, size);
}

void VTAFlushCache(void* vir_addr, vta_phy_addr_t phy_addr, int size) {
  // Call the cma_flush_cache on the CMA buffer
  // so that the FPGA can read the buffer data.
  cma_flush_cache(vir_addr, phy_addr, size);
}

void VTAInvalidateCache(void* vir_addr, vta_phy_addr_t phy_addr, int size) {
  // Call the cma_invalidate_cache on the CMA buffer
  // so that the host needs to read the buffer data.
  cma_invalidate_cache(vir_addr, phy_addr, size);
}

void *VTAMapRegister(uint32_t addr) {
  // Align the base address with the pages
  uint32_t virt_base = addr & ~(getpagesize() - 1);
  // Calculate base address offset w.r.t the base address
  uint32_t virt_offset = addr - virt_base;
  // Open file and mmap
  uint32_t mmap_file = open("/dev/mem", O_RDWR|O_SYNC);
  void* rtn =  mmap(NULL,
              (VTA_IP_REG_MAP_RANGE + virt_offset),
              PROT_READ|PROT_WRITE,
              MAP_SHARED,
              mmap_file,
              virt_base);
  close(mmap_file);
  return rtn;
}

void VTAUnmapRegister(void *vta) {
  // Unmap memory
  int status = munmap(vta, VTA_IP_REG_MAP_RANGE);
  assert(status == 0);
}

void VTAWriteMappedReg(void* base_addr, uint32_t offset, uint32_t val) {
  *((volatile uint32_t *) (reinterpret_cast<char *>(base_addr) + offset)) = val;
}

uint32_t VTAReadMappedReg(void* base_addr, uint32_t offset) {
  return *((volatile uint32_t *) (reinterpret_cast<char *>(base_addr) + offset));
}


struct ProfileEvent {
  enum class EventType {
    DEFAULT = 0,
    VTA_SYNC = 1
  };

  uint64_t begin_timestamp_us;
  uint64_t end_timestamp_us;
  EventType event_type;
  uint32_t event_num;
  uint32_t ops;
  uint32_t read_write_bits;
  uint32_t read_bytes;
  uint32_t read_latency;
  uint32_t read_transactions;
  uint32_t write_bytes;
  uint32_t write_latency;
  uint32_t write_transactions;
  uint32_t gemm_count;
  uint32_t alu_count;
  uint32_t load_count;
  double exe_time;
};


static int log_count = 0;
#define AXI_PERF_MONITOR 0xA0050000
//#### JO added
#define AXI_PERF_MONITOR_STRIDE 0x10000
#define CONTROL_OFFSET 0x300
#define SLOT0_OFFSET 0x100
#define SLOT1_OFFSET 0x160
#define SLOT2_OFFSET 0x500
#define WRITE_BYTE_COUNT 0x0
#define WRITE_TRANSACTION_COUNT 0x10
#define WRITE_LATENCY_COUNT 0x20
#define READ_BYTE_COUNT 0x30
#define READ_TRANSACTION_COUNT 0x40
#define READ_LATENCY_COUNT 0x50


#define COMPUTE_LOAD_COUNT_OFFSET 0x50
#define COMPUTE_GEMM_COUNT_OFFSET 0x60
#define COMPUTE_ALU_COUNT_OFFSET 0x70

//#define PROFILE_ENABLE 0

//#### JO added: the zcu102_1x16_i8w8a32_16_16_19_18 bitstream has no AXI perf monitor at
//#### AXI_PERF_MONITOR (reading it -> SIGBUS), so PROFILE_ENABLE alone records only timing and
//#### the compute-module counters. Build with -DPROFILE_AXI_MONITOR=1 for a design that has one
//#### (vta-hw-zcu102-single `make bin PROFILE=1`, cmake -DNESTC_EVTA_PROFILE_AXI_MONITOR=ON).
#ifndef PROFILE_AXI_MONITOR
#define PROFILE_AXI_MONITOR 0
#endif
#define PROFILE_MAX_EVENTS 1024

// device handle
static VTADeviceHandle createdVTADeviceHandle[VTA_DEVICE_NUM] = {};


class VTADevice {
 public:
  VTADevice(int index=0) {      

    assert(index < VTA_DEVICE_NUM);

    uint32_t fetch_addr;
    uint32_t load_addr;
    uint32_t compute_addr;
    uint32_t store_addr;

    vta_index_ = index;

    //defautl init
    fetch_addr=VTA_FETCH_ADDR;
    load_addr=VTA_LOAD_ADDR;
    compute_addr=VTA_COMPUTE_ADDR;
    store_addr=VTA_STORE_ADDR;      

    switch(index) {
      case 0:
        fetch_addr=VTA_FETCH_ADDR;
        load_addr=VTA_LOAD_ADDR;
        compute_addr=VTA_COMPUTE_ADDR;
        store_addr=VTA_STORE_ADDR;
        break;
      case 1:
        fetch_addr=VTA_FETCH_ADDR_1;
        load_addr=VTA_LOAD_ADDR_1;
        compute_addr=VTA_COMPUTE_ADDR_1;
        store_addr=VTA_STORE_ADDR_1;      
      break;
      case 2:
        fetch_addr=VTA_FETCH_ADDR_2;
        load_addr=VTA_LOAD_ADDR_2;
        compute_addr=VTA_COMPUTE_ADDR_2;
        store_addr=VTA_STORE_ADDR_2;      

      break;
      case 3:
        fetch_addr=VTA_FETCH_ADDR_3;
        load_addr=VTA_LOAD_ADDR_3;
        compute_addr=VTA_COMPUTE_ADDR_3;
        store_addr=VTA_STORE_ADDR_3;      

      break;
      default:
        printf("ERROR. no index.");

      break;

    }

    // VTA stage handles
    vta_fetch_handle_ = VTAMapRegister(fetch_addr);
    vta_load_handle_ = VTAMapRegister(load_addr);
    vta_compute_handle_ = VTAMapRegister(compute_addr);
    vta_store_handle_ = VTAMapRegister(store_addr);
    //#### JO changed: map the AXI perf monitor only when the design has one
    //if(PROFILE_ENABLE) {
    if(PROFILE_ENABLE && PROFILE_AXI_MONITOR) {
      //#### JO changed: one monitor per EVTA (vta-hw-zcu102-multi* PROFILE=1), EVTA i at + i * 0x10000
      //vta_axiperfmonitor_handle_ = VTAMapRegister(AXI_PERF_MONITOR);
      vta_axiperfmonitor_handle_ = VTAMapRegister(AXI_PERF_MONITOR + index * AXI_PERF_MONITOR_STRIDE);
    }

#ifdef VTA_DEBUG_MODE
    printf("VTA Device created idx=%d\n", index);
    printf("fetch(%p) = %p\n", vta_fetch_handle_,fetch_addr);
    printf("load(%p) = %p\n", vta_load_handle_,load_addr);
    printf("compute(%p) = %p\n", vta_compute_handle_,compute_addr);
    printf("store(%p) = %p\n", vta_store_handle_,store_addr);
#endif	

    current_event_index_ = 0;
  }

  ~VTADevice() {
    // Close VTA stage handle
    VTAUnmapRegister(vta_fetch_handle_);
    VTAUnmapRegister(vta_load_handle_);
    VTAUnmapRegister(vta_compute_handle_);
    VTAUnmapRegister(vta_store_handle_);
    //#### JO changed: nothing recorded -> event_buffer_[-1] below; also report dropped events
    //if (PROFILE_ENABLE) {
    if (PROFILE_ENABLE && current_event_index_ > 0) {
      if (dropped_events_)
        printf("[profile] VTA%u: event buffer full, %u later events not recorded\n", vta_index_, dropped_events_);
      PrintProfilingInfo();

      double total_time = 0.0;
      double total_vta_time = 0.0;
      for (int i = 0; i < current_event_index_; i++)
        total_vta_time += event_buffer_[i].exe_time;

      total_time = double(event_buffer_[current_event_index_ - 1].end_timestamp_us - event_buffer_[0].begin_timestamp_us) / 1000.0;

      printf("total_time : %.3f ms, total_vta_time : %.3f ms\n", total_time, total_vta_time);


    }
  }

  void PrintProfilingInfo() {
    printf("event_type event_num GOPS Gbits read_latency read_bytes read_transactions write_latency write_bytes write_transactions gemm_count alu_count load_count exe_time begin_timestamp_us end_timestamp_us\n");
    for (int i = 0; i < current_event_index_; i++) {
      struct ProfileEvent *e = &event_buffer_[i];
      printf("%s %d %.3f %.3f %d %d %d %d %d %d %d %d %d %.3f %lld %lld\n",
             e->event_type == ProfileEvent::EventType::VTA_SYNC ? "VTA_SYNC" : "Unknown",
             e->event_num,
             (double)e->ops / e->exe_time / 1000000,
             (double)e->read_write_bits / e->exe_time / 1000000,
             e->read_latency,
             e->read_bytes,
             e->read_transactions,
             e->write_latency,
             e->write_bytes,
             e->write_transactions,
             e->gemm_count,
             e->alu_count,
             e->load_count,
             e->exe_time,
             e->begin_timestamp_us,
             e->end_timestamp_us
      );
    }
  }

  int Run(vta_phy_addr_t insn_phy_addr,
          uint32_t insn_count,
          uint32_t wait_cycles) {

#ifdef VTA_DEBUG_MODE		  
	printf("VTA RUN (IDX=%d)\n", vta_index_);
	printf("insn_phyaddr=%p\n",insn_phy_addr);
	printf("insn_count=%d\n",insn_count);
	printf("wait_cycles=%u\n",wait_cycles);
#endif	

    VTAWriteMappedReg(vta_fetch_handle_, VTA_FETCH_INSN_COUNT_OFFSET, insn_count);
    VTAWriteMappedReg(vta_fetch_handle_, VTA_FETCH_INSN_ADDR_OFFSET, insn_phy_addr);
    VTAWriteMappedReg(vta_load_handle_, VTA_LOAD_INP_ADDR_OFFSET, 0);
    VTAWriteMappedReg(vta_load_handle_, VTA_LOAD_WGT_ADDR_OFFSET, 0);
    VTAWriteMappedReg(vta_compute_handle_, VTA_COMPUTE_UOP_ADDR_OFFSET, 0);
    VTAWriteMappedReg(vta_compute_handle_, VTA_COMPUTE_BIAS_ADDR_OFFSET, 0);
    VTAWriteMappedReg(vta_store_handle_, VTA_STORE_OUT_ADDR_OFFSET, 0);


    //#### JO changed: the monitor's counters are disabled after reset; clear them (bit 1) and enable
    //#### them (bit 0) for every run so each event holds this run's counts
    //if(PROFILE_ENABLE) {
    //  VTAWriteMappedReg(vta_axiperfmonitor_handle_, CONTROL_OFFSET, 2);
    //  VTAWriteMappedReg(vta_axiperfmonitor_handle_, CONTROL_OFFSET, 1);
    //}
    if(PROFILE_ENABLE && PROFILE_AXI_MONITOR) {
      VTAWriteMappedReg(vta_axiperfmonitor_handle_, CONTROL_OFFSET, 2);
      VTAWriteMappedReg(vta_axiperfmonitor_handle_, CONTROL_OFFSET, 1);
    }
    

    //#### JO added (2026-10-23): clear done_o_ap_vld (clear on read) before the start; see the done poll below
    VTAReadMappedReg(vta_compute_handle_, VTA_COMPUTE_DONE_RD_OFFSET + 4);

    // VTA start
    VTAWriteMappedReg(vta_fetch_handle_, 0x0, VTA_START);
    VTAWriteMappedReg(vta_load_handle_, 0x0, VTA_AUTORESTART);
    VTAWriteMappedReg(vta_compute_handle_, 0x0, VTA_AUTORESTART);
    VTAWriteMappedReg(vta_store_handle_, 0x0, VTA_AUTORESTART);

    struct timeval start_time, stop_time;

    gettimeofday(&start_time, nullptr);
    // Loop until the VTA is done
    //#### JO changed (2026-10-23): done race. compute's done_o register (0x18) keeps the 1 of the previous
    //#### run's FINISH: compute (vta.cc) pops the instruction, then blocks on the l2g/s2g dependence pops, and
    //#### only then writes done = 0. So right after the start done_o still reads 1 while compute waits for
    //#### load (e.g. the first GEMM of a conv waits for the weights), and the old loop returned before the
    //#### run had finished (the original upstream loop breaks on the first read; `t > 10` only delayed it a
    //#### few reads). HLS keeps done_o_ap_vld (0x1c, clear on read, cleared before the start above), set by
    //#### every write of done: once it has been seen, done_o holds a value of this run, and 1 can only come
    //#### from this run's FINISH.
    //unsigned t, flag = 0;
    //for (t = 0; t < wait_cycles; ++t) {
    //  flag = VTAReadMappedReg(vta_compute_handle_, VTA_COMPUTE_DONE_RD_OFFSET);
    //  if (t > 10 && flag == VTA_DONE) break;
    //  std::this_thread::yield();
    //}
    unsigned t, flag = 0;
    bool written = false;
    for (t = 0; t < wait_cycles; ++t) {
      if (!written)
        written = VTAReadMappedReg(vta_compute_handle_, VTA_COMPUTE_DONE_RD_OFFSET + 4) & 1;
      if (written) {
        flag = VTAReadMappedReg(vta_compute_handle_, VTA_COMPUTE_DONE_RD_OFFSET);
        if (flag == VTA_DONE) break;
      }
      std::this_thread::yield();
    }
    gettimeofday(&stop_time, nullptr);



    //#### JO changed: event_buffer_ has PROFILE_MAX_EVENTS entries and was written without a bound check
    //if (PROFILE_ENABLE) {
    if (PROFILE_ENABLE && current_event_index_ >= PROFILE_MAX_EVENTS) {
      dropped_events_++;
    } else if (PROFILE_ENABLE) {
      //#### JO added: AXI perf monitor reads only when the design has one (else they read as 0)
#if PROFILE_AXI_MONITOR
      unsigned slot0_wr_bytes = VTAReadMappedReg(vta_axiperfmonitor_handle_, SLOT0_OFFSET + WRITE_BYTE_COUNT);
      unsigned slot0_wr_transactions = VTAReadMappedReg(vta_axiperfmonitor_handle_, SLOT0_OFFSET + WRITE_TRANSACTION_COUNT);
      unsigned slot0_wr_latency = VTAReadMappedReg(vta_axiperfmonitor_handle_, SLOT0_OFFSET + WRITE_LATENCY_COUNT);

      unsigned slot0_rd_bytes = VTAReadMappedReg(vta_axiperfmonitor_handle_, SLOT0_OFFSET + READ_BYTE_COUNT);
      unsigned slot0_rd_transactions = VTAReadMappedReg(vta_axiperfmonitor_handle_, SLOT0_OFFSET + READ_TRANSACTION_COUNT);
      unsigned slot0_rd_latency = VTAReadMappedReg(vta_axiperfmonitor_handle_, SLOT0_OFFSET + READ_LATENCY_COUNT);

      unsigned slot1_wr_bytes = VTAReadMappedReg(vta_axiperfmonitor_handle_, SLOT1_OFFSET + WRITE_BYTE_COUNT);
      unsigned slot1_wr_transactions = VTAReadMappedReg(vta_axiperfmonitor_handle_, SLOT1_OFFSET + WRITE_TRANSACTION_COUNT);
      unsigned slot1_wr_latency = VTAReadMappedReg(vta_axiperfmonitor_handle_, SLOT1_OFFSET + WRITE_LATENCY_COUNT);

      unsigned slot1_rd_bytes = VTAReadMappedReg(vta_axiperfmonitor_handle_, SLOT1_OFFSET + READ_BYTE_COUNT);
      unsigned slot1_rd_transactions = VTAReadMappedReg(vta_axiperfmonitor_handle_, SLOT1_OFFSET + READ_TRANSACTION_COUNT);
      unsigned slot1_rd_latency = VTAReadMappedReg(vta_axiperfmonitor_handle_, SLOT1_OFFSET + READ_LATENCY_COUNT);

      unsigned slot2_wr_bytes = VTAReadMappedReg(vta_axiperfmonitor_handle_, SLOT2_OFFSET + WRITE_BYTE_COUNT);
      unsigned slot2_wr_transactions = VTAReadMappedReg(vta_axiperfmonitor_handle_, SLOT2_OFFSET + WRITE_TRANSACTION_COUNT);
      unsigned slot2_wr_latency = VTAReadMappedReg(vta_axiperfmonitor_handle_, SLOT2_OFFSET + WRITE_LATENCY_COUNT);

      unsigned slot2_rd_bytes = VTAReadMappedReg(vta_axiperfmonitor_handle_, SLOT2_OFFSET + READ_BYTE_COUNT);
      unsigned slot2_rd_transactions = VTAReadMappedReg(vta_axiperfmonitor_handle_, SLOT2_OFFSET + READ_TRANSACTION_COUNT);
      unsigned slot2_rd_latency = VTAReadMappedReg(vta_axiperfmonitor_handle_, SLOT2_OFFSET + READ_LATENCY_COUNT);
#else
      unsigned slot0_wr_bytes = 0, slot0_rd_bytes = 0;
      unsigned slot1_rd_bytes = 0, slot1_rd_transactions = 0, slot1_rd_latency = 0;
      unsigned slot2_wr_bytes = 0, slot2_wr_transactions = 0, slot2_wr_latency = 0;
#endif

      unsigned load_count = VTAReadMappedReg(vta_compute_handle_, COMPUTE_LOAD_COUNT_OFFSET);
      unsigned gemm_count = VTAReadMappedReg(vta_compute_handle_, COMPUTE_GEMM_COUNT_OFFSET);
      unsigned alu_count = VTAReadMappedReg(vta_compute_handle_, COMPUTE_ALU_COUNT_OFFSET);

      unsigned ops = (gemm_count * 2 * 256) + (alu_count / 2 * 16);
      unsigned read_write_bits = (slot0_wr_bytes + slot0_rd_bytes) * 8;


      uint64_t begin_timestamp_us = static_cast<uint64_t>(start_time.tv_sec) * 1000000 + start_time.tv_usec;
      uint64_t end_timestamp_us= static_cast<uint64_t>(stop_time.tv_sec) * 1000000 + stop_time.tv_usec;

      event_buffer_[current_event_index_].begin_timestamp_us = begin_timestamp_us;
      event_buffer_[current_event_index_].end_timestamp_us = end_timestamp_us;
      event_buffer_[current_event_index_].exe_time = double(end_timestamp_us - begin_timestamp_us) / 1000.0;
      event_buffer_[current_event_index_].ops = ops;
      event_buffer_[current_event_index_].read_write_bits = read_write_bits;
      event_buffer_[current_event_index_].read_latency = slot1_rd_latency;
      event_buffer_[current_event_index_].read_transactions = slot1_rd_transactions;
      event_buffer_[current_event_index_].write_latency = slot2_wr_latency;
      event_buffer_[current_event_index_].read_bytes = slot1_rd_bytes;
      event_buffer_[current_event_index_].write_bytes = slot2_wr_bytes;
      event_buffer_[current_event_index_].write_transactions = slot2_wr_transactions;

      event_buffer_[current_event_index_].event_type = ProfileEvent::EventType::VTA_SYNC;
      event_buffer_[current_event_index_].event_num = current_event_index_;
      event_buffer_[current_event_index_].gemm_count = gemm_count;
      event_buffer_[current_event_index_].load_count = load_count;
      event_buffer_[current_event_index_].alu_count = alu_count;

      current_event_index_++;
    }

    // Report error if timeout
    return t < wait_cycles ? 0 : 1;
  }

  uint32_t GetIndex() {
    return vta_index_;
  }

 private:
  // VTA handles (register maps)
  void* vta_fetch_handle_{nullptr};
  void* vta_load_handle_{nullptr};
  void* vta_compute_handle_{nullptr};
  void* vta_store_handle_{nullptr};
  void* vta_axiperfmonitor_handle_{nullptr};

  //#### JO changed: size named so Run() can bound-check it
  //struct ProfileEvent event_buffer_[1024];
  struct ProfileEvent event_buffer_[PROFILE_MAX_EVENTS];
  uint32_t current_event_index_;
  //#### JO added
  uint32_t dropped_events_{0};
  uint32_t vta_index_;
};

VTADeviceHandle VTADeviceAlloc(int index) {
#ifdef VTA_DEBUG_MODE
  printf("devAlloc idx=%d\n",index);
#endif
  if (createdVTADeviceHandle[index] == nullptr) createdVTADeviceHandle[index] = new VTADevice(index);
#ifdef VTA_DEBUG_MODE
  printf("addr = %p\n",createdVTADeviceHandle[index]);
#endif
  return createdVTADeviceHandle[index];
}

void VTADeviceFree(VTADeviceHandle handle) {
  int index = ((VTADevice*)handle)->GetIndex();
  if(index >= VTA_DEVICE_NUM) {
    printf("[ERROR] invalid vta device handle: %p. index: %d\n", handle, index);
    return;

  }
  createdVTADeviceHandle[index]=nullptr;  
  delete static_cast<VTADevice*>(handle);
}

int VTADeviceRun(VTADeviceHandle handle,
                 vta_phy_addr_t insn_phy_addr,
                 uint32_t insn_count,
                 uint32_t wait_cycles) {
  return static_cast<VTADevice*>(handle)->Run(
      insn_phy_addr, insn_count, wait_cycles);
}

