#ifndef NPULIB_API_COMMAND_QUEUE_H_
#define NPULIB_API_COMMAND_QUEUE_H_


#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <sys/mman.h>
#include <fcntl.h>
#include <vta/driver.h>
#include "uop_kernel.h"

#include <memory>
#include <string>
#include <ctime>

namespace vta {

/*!
 * \brief The command queue object that handles the request.
 */
class CommandQueue {
 public:
  CommandQueue(int index=0) {
    this->InitSpace(index);
  }
  void InitSpace(int index) {
    uop_queue_.InitSpace();
    insn_queue_.InitSpace();
    device_ = VTADeviceAlloc(index);
    CHECK(device_ != nullptr);
    evta_index_ = index;
  }

  ~CommandQueue() {
    VTADeviceFree(device_);
  }

  uint32_t GetElemBytes(uint32_t memory_id) {
    uint32_t elem_bytes = 0;
    switch (memory_id) {
      case VTA_MEM_ID_UOP:
          elem_bytes = VTA_UOP_ELEM_BYTES;
          break;
      case VTA_MEM_ID_INP:
          elem_bytes = VTA_INP_ELEM_BYTES;
          break;
      case VTA_MEM_ID_WGT:
          elem_bytes = VTA_WGT_ELEM_BYTES;
          break;
      case VTA_MEM_ID_ACC:
          elem_bytes = VTA_ACC_ELEM_BYTES;
          break;
      case VTA_MEM_ID_OUT:
          elem_bytes = VTA_INP_ELEM_BYTES;
          break;
      case VTA_MEM_ID_MOVE:
          elem_bytes = VTA_INP_ELEM_BYTES;
      default:
          LOG(FATAL) << "Memory id not recognized:" << memory_id;
          break;
    }
    /*
     * elements size should not larger than VTA_PAGE_BYTES.
     *
     */
    CHECK_GE(VTA_PAGE_BYTES, elem_bytes);
    return elem_bytes;
  }

  void LoadBuffer2D(void* src_dram_addr,
                    uint32_t src_elem_offset,
                    uint32_t x_size,
                    uint32_t y_size,
                    uint32_t x_stride,
                    uint32_t x_pad_before,
                    uint32_t y_pad_before,
                    uint32_t x_pad_after,
                    uint32_t y_pad_after,
                    uint32_t dst_sram_index,
                    uint32_t dst_memory_type) {
    VTAMemInsn* insn = insn_queue_.CreateMemInsn(dst_memory_type);
    insn->opcode = VTA_OPCODE_LOAD;
    insn->memory_type = dst_memory_type;
    insn->sram_base = dst_sram_index;
    DataBuffer* src = DataBuffer::FromHandle(src_dram_addr);
    if(dst_memory_type!=VTA_MEM_ID_MOVE) {
      insn->dram_base = src->phy_addr() / GetElemBytes(dst_memory_type) + src_elem_offset;
    }
    else {
      insn->dram_base = *(uint32_t *) src_dram_addr;
    }
    insn->y_size = y_size;
    insn->x_size = x_size;
    insn->x_stride = x_stride;
    insn->y_pad_0 = y_pad_before;
    insn->y_pad_1 = y_pad_after;
    insn->x_pad_0 = x_pad_before;
    insn->x_pad_1 = x_pad_after;
    this->CheckInsnOverFlow();
  }

  void StoreBuffer2D(uint32_t src_sram_index,
                     uint32_t src_memory_type,
                     void* dst_dram_addr,
                     uint32_t dst_elem_offset,
                     uint32_t x_size,
                     uint32_t y_size,
                     uint32_t x_stride) {
    VTAMemInsn* insn = insn_queue_.CreateStoreInsn();
    insn->opcode = VTA_OPCODE_STORE;
    insn->memory_type = src_memory_type;
    insn->sram_base = src_sram_index;
    DataBuffer* dst = DataBuffer::FromHandle(dst_dram_addr);
    insn->dram_base = dst->phy_addr() / GetElemBytes(src_memory_type) + dst_elem_offset;
    insn->y_size = y_size;
    insn->x_size = x_size;
    insn->x_stride = x_stride;
    insn->y_pad_0 = 0;
    insn->y_pad_1 = 0;
    insn->x_pad_0 = 0;
    insn->x_pad_1 = 0;
    this->CheckInsnOverFlow();

    //printf("store2d dram_addr = %p, dst->phy_addr=%p (drambase=%p)\n",dst_dram_addr, (void*)dst->phy_addr(), (void*)insn->dram_base );

  }

  void DepPush(int from_qid, int to_qid) {
   // printf("dep_push(%d %d)\n",from_qid, to_qid);
    insn_queue_.DepPush(from_qid, to_qid);
  }

  void DepPop(int from_qid, int to_qid) {
  //  printf("dep_pop(%d %d)\n",from_qid, to_qid);    
    insn_queue_.DepPop(from_qid, to_qid);
  }

  void ReadBarrier(void* buffer, uint32_t elem_bits, uint32_t start, uint32_t extent) {
    if (!(debug_flag_ & VTA_DEBUG_SKIP_READ_BARRIER)) {
      //printf("CmdQ ReadBarrier\n");
      uint32_t elem_bytes = (elem_bits + 8 - 1) / 8;
      DataBuffer::FromHandle(buffer)->FlushCache(
          elem_bytes * start, elem_bytes * extent);
    }
  }

  void WriteBarrier(void* buffer, uint32_t elem_bits, uint32_t start, uint32_t extent) {
    if (!(debug_flag_ & VTA_DEBUG_SKIP_WRITE_BARRIER)) {
      //printf("CmdQ WriteBarrier\n");
      uint32_t elem_bytes = (elem_bits + 8 - 1) / 8;
      DataBuffer::FromHandle(buffer)->InvalidateCache(
          elem_bytes * start, elem_bytes * extent);
    }
  }

  void Synchronize(uint32_t wait_cycles) {
    //printf("sync\n");    
    // Insert dependences to force serialization         
    if (debug_flag_ & VTA_DEBUG_FORCE_SERIAL) {
      insn_queue_.RewriteForceSerial();
    } else {
      // This will issue finish after last store finishes
      insn_queue_.DepPush(kStoreStage, kComputeStage);
      insn_queue_.DepPush(kLoadStage, kComputeStage);
      insn_queue_.DepPop(kStoreStage, kComputeStage);
      insn_queue_.DepPop(kLoadStage, kComputeStage);
      insn_queue_.CommitPendingPop(kComputeStage);
    }
    // NOTE: FINISH cannot contain pop
    VTAGemInsn* insn = insn_queue_.CreateGemInsn();
    insn->opcode = VTA_OPCODE_FINISH;
    CHECK(!insn_queue_.PendingPop());
    // Check if there are no instruction to execute at all
    if (insn_queue_.count() == 0) return;
    // Synchronization for the queues
    uop_queue_.AutoReadBarrier();
    insn_queue_.AutoReadBarrier();
    // Dump instructions if debug enabled

    if (debug_flag_ & VTA_DEBUG_DUMP_INSN) {
      insn_queue_.DumpInsn();
    }

    //printf("[Synchronize] check last instruction FINISH");
    // Make sure that the last instruction is a finish instruction
    CHECK(reinterpret_cast<VTAMemInsn*>(
    insn_queue_.data())[insn_queue_.count()-1].opcode == VTA_OPCODE_FINISH);
    //printf(" -- DONE\n");

    //printf("[Synchronize] check exceed physical memory limit");
    // Make sure that we don't exceed contiguous physical memory limits
    CHECK(insn_queue_.count() * sizeof(VTAGenericInsn) < VTA_MAX_XFER);
    //printf(" -- DONE\n");

    //#### JO added (2026-11-01): EVTA_STORE_SENTINEL=1 fills the DRAM the STOREs of this run write with 0xA5 first
    StoreSentinelFill();
    PreRunWait();
    //printf("[Synchronize] device run. device=%p", device_);
    int timeout = VTADeviceRun(
        device_,
        insn_queue_.dram_phy_addr(),
        insn_queue_.count(),
        wait_cycles);
    //printf("[Synchronize] device run. -- DONE\n");

    //printf("[Synchronize] check timeout");
    //#### JO added (2026-11-01): a timed-out run (EVTA_RUN_TIMEOUT_MS, pynq_driver.cc) saves its instructions
    if (timeout) {
      char name[128];
      snprintf(name, sizeof(name), "/tmp/evta_hang_%d_%08x.insn", (int)getpid(), (unsigned)insn_queue_.dram_phy_addr());
      FILE *fp = fopen(name, "wb");
      if (fp) {
        fwrite(insn_queue_.data(), sizeof(VTAGenericInsn), insn_queue_.count(), fp);
        fclose(fp);
        fprintf(stderr, "instructions of the run saved in %s (%u x %zu bytes)\n", name, (unsigned)insn_queue_.count(),
                sizeof(VTAGenericInsn));
      }
      insn_queue_.CompareDram("insn");
      uop_queue_.CompareDram("uop");
      StoreSentinelReport();
      //#### exit at once (not the CHECK's abort: apport takes the core dump of the process for minutes, and the CMA
      //#### stays allocated until it is done, so the next bitstream load fails with ENOMEM)
      fflush(stdout);
      fflush(stderr);
      _exit(5);
    }
    CHECK_EQ(timeout, 0);
    //printf(" -- DONE\n");
    
    //printf("[Synchronize] reset uop queue");
    // Reset buffers
    uop_queue_.Reset();
    //printf(" -- DONE\n");

    //printf("[Synchronize] reset uop queue");
    insn_queue_.Reset();
    //printf(" -- DONE\n");    
  }

  int Synchronize_CNU(uint32_t wait_cycles, char* rec_cache = NULL, int buf_size = 0, int num_cache = 0, int record = -1) {
    //printf("sync\n");    
    // Insert dependences to force serialization
    int counter = 0;
    if(record != 0){                
      if (debug_flag_ & VTA_DEBUG_FORCE_SERIAL) {
        insn_queue_.RewriteForceSerial();
      } else {
        // This will issue finish after last store finishes
        insn_queue_.DepPush(kStoreStage, kComputeStage);
        insn_queue_.DepPush(kLoadStage, kComputeStage);
        insn_queue_.DepPop(kStoreStage, kComputeStage);
        insn_queue_.DepPop(kLoadStage, kComputeStage);
        insn_queue_.CommitPendingPop(kComputeStage);
      }
      // NOTE: FINISH cannot contain pop
      VTAGemInsn* insn = insn_queue_.CreateGemInsn();
      insn->opcode = VTA_OPCODE_FINISH;
      CHECK(!insn_queue_.PendingPop());
    }
    // Check if there are no instruction to execute at all
    if (insn_queue_.count() == 0 && record != 0) return 0;
    if(record == 0){
      memcpy(insn_queue_.dram_point(), rec_cache, buf_size);
    }else if(record == 1){
      memcpy(rec_cache, insn_queue_.dram_point(), insn_queue_.count() * sizeof(VTAGenericInsn));
    }
    // Synchronization for the queues
    uop_queue_.AutoReadBarrier();
    if(record == 0){
      insn_queue_.ManualReadBarrier(buf_size);
    }else{                                                                
      insn_queue_.AutoReadBarrier();
    }
    // Dump instructions if debug enabled

    if (debug_flag_ & VTA_DEBUG_DUMP_INSN) {
        insn_queue_.DumpInsn();      
    }

    //printf("[Synchronize] check last instruction FINISH");
    // Make sure that the last instruction is a finish instruction
    if(record != 0){                                                             
      CHECK(reinterpret_cast<VTAMemInsn*>(
      insn_queue_.data())[insn_queue_.count()-1].opcode == VTA_OPCODE_FINISH);
    }                                                                              
    //printf(" -- DONE\n");

    //printf("[Synchronize] check exceed physical memory limit");
    // Make sure that we don't exceed contiguous physical memory limits
    if(record != 0){                                                                   
      CHECK(insn_queue_.count() * sizeof(VTAGenericInsn) < VTA_MAX_XFER);
    }
    //printf(" -- DONE\n");

    counter = insn_queue_.count() * sizeof(VTAGenericInsn);
    //printf("[Synchronize] device run. device=%p", device_);
    int timeout;
    if(record == 0){                                                         
        timeout = VTADeviceRun(
        device_,
        insn_queue_.dram_phy_addr(),
        buf_size/sizeof(VTAGenericInsn),
        wait_cycles);
    }else{                              
        timeout = VTADeviceRun(
        device_,
        insn_queue_.dram_phy_addr(),
        insn_queue_.count(),
        wait_cycles);
    }
    //printf("[Synchronize] device run. -- DONE\n");

    //printf("[Synchronize] check timeout");
    CHECK_EQ(timeout, 0);
    //printf(" -- DONE\n");
    
    //printf("[Synchronize] reset uop queue");
    // Reset buffers
    uop_queue_.Reset();
    //printf(" -- DONE\n");

    //printf("[Synchronize] reset uop queue");
    insn_queue_.Reset();
    //printf(" -- DONE\n");    
    return counter;                           
  }

  // Get record kernel
  UopKernel* record_kernel() const {
    //printf("rk\n");
    CHECK(record_kernel_ != nullptr);
    return record_kernel_;
  }

  // Set debug flag
  void SetDebugFlag(int debug_flag) {
    debug_flag_ = debug_flag;
  }

  void PushGEMMOp(void** uop_handle,
                  int (*finit)(void*,VTACommandHandle),
                  void* signature,
                  int nbytes,
                  bool from_outmem=false) {
    UopKernelMap** uptr = reinterpret_cast<UopKernelMap**>(uop_handle);
    
    if (uptr[0] == nullptr) {
      uptr[0] = new UopKernelMap();
    }
    UopKernel** kptr = uptr[0]->Get(signature, nbytes);
    if (kptr[0] == nullptr) {
      record_kernel_ = new UopKernel(static_cast<char*>(signature), nbytes);
      CHECK_EQ((*finit)(signature,this), 0);
      kptr[0] = static_cast<UopKernel*>(record_kernel_);

      if (debug_flag_ & VTA_DEBUG_DUMP_UOP) {
          printf("dump kernel\n");
        record_kernel_->Dump();
      }
      record_kernel_ = nullptr;
    }
    this->PushGEMMOp(static_cast<UopKernel*>(kptr[0]), from_outmem);
    this->CheckInsnOverFlow();
  }

  void PushXPGEMMOp(void** uop_handle,
                  int (*finit)(void*,VTACommandHandle),
                  void* signature,
                  int nbytes,
                  bool from_outmem=false) {
    UopKernelMap** uptr = reinterpret_cast<UopKernelMap**>(uop_handle);
    
    if (uptr[0] == nullptr) {
      uptr[0] = new UopKernelMap();
    }
    UopKernel** kptr = uptr[0]->Get(signature, nbytes);
    if (kptr[0] == nullptr) {
      record_kernel_ = new UopKernel(static_cast<char*>(signature), nbytes);
      CHECK_EQ((*finit)(signature,this), 0);
      kptr[0] = static_cast<UopKernel*>(record_kernel_);

      if (debug_flag_ & VTA_DEBUG_DUMP_UOP) {
          printf("dump kernel\n");
        record_kernel_->Dump();
      }
      record_kernel_ = nullptr;
    }
    this->PushXPGEMMOp(static_cast<UopKernel*>(kptr[0]), from_outmem);
    this->CheckInsnOverFlow();
  }

  void PushALUUop(void** uop_handle,
                  int (*finit)(void*,VTACommandHandle),
                  void* signature,
                  int nbytes) {
    UopKernelMap** uptr = reinterpret_cast<UopKernelMap**>(uop_handle);
    //printf("UKM:%p\n",uptr[0]);
    if (uptr[0] == nullptr) {
      uptr[0] = new UopKernelMap();
    }
    UopKernel** kptr = uptr[0]->Get(signature, nbytes);
    if (kptr[0] == nullptr) {
      record_kernel_ = new UopKernel(static_cast<char*>(signature), nbytes);
      CHECK_EQ((*finit)(signature,this), 0);
      kptr[0] = static_cast<UopKernel*>(record_kernel_);
      if (debug_flag_ & VTA_DEBUG_DUMP_UOP) {
        record_kernel_->Dump();
      }
      record_kernel_ = nullptr;
    }
    this->PushALUUop(static_cast<UopKernel*>(kptr[0]));
    this->CheckInsnOverFlow();
  }

  static std::shared_ptr<CommandQueue>& ThreadLocal(uint32_t index=0) {
    
/*   
    static std::vector<std::shared_ptr<CommandQueue>> instPtr;

    // init
    if(instPtr.size()==0) {
     // printf("----------- vector <cmdqueue> inited -------------- \n");
      for(int i=0;i<VTA_DEVICE_NUM;i++) {
        auto k = std::make_shared<CommandQueue>(i);
        instPtr.push_back(k);
      }
    }

    if(index >= VTA_DEVICE_NUM) {
      index=0;
    }
    
    return instPtr[index];
    
*/

    static std::shared_ptr<CommandQueue> inst0;
    static std::shared_ptr<CommandQueue> inst1;
    static std::shared_ptr<CommandQueue> inst2;
    static std::shared_ptr<CommandQueue> inst3;


    if (inst0 == nullptr && index==0) {
      inst0 = std::make_shared<CommandQueue>(0);
    }
    if (inst1 == nullptr && index==1) {
      inst1 = std::make_shared<CommandQueue>(1);
    }
    if (inst2 == nullptr && index==2) {
      inst2 = std::make_shared<CommandQueue>(2);
    }
    if (inst3 == nullptr && index==3) {
      inst3 = std::make_shared<CommandQueue>(3);
    }

    if(index==0) return inst0;
    if(index==1) return inst1;
    if(index==2) return inst2;
    if(index==3) return inst3;
    return inst0;
  }

  static void Shutdown(int index=0) {
    ThreadLocal(index).reset();
  }

  void ResetUopQueue(){
    uop_queue_.UopReset();
  }

 private:
  // Push GEMM uop to the command buffer
  void PushGEMMOp(UopKernel* kernel, bool from_outmem=false) {
    uop_queue_.Push(kernel,
                    [this]() { this->AutoSync(); });
    if (uop_queue_.pending()) {
      VTAMemInsn* insn = insn_queue_.CreateMemInsn(VTA_MEM_ID_UOP);
      insn->opcode = VTA_OPCODE_LOAD;
      uop_queue_.FlushUopLoad(insn);
    }
    VTAGemInsn* insn = insn_queue_.CreateGemInsn();
    insn->opcode = VTA_OPCODE_GEMM;
    insn->reset_reg = kernel->reset_out_;
    insn->uop_bgn = kernel->sram_begin_;
    insn->uop_end = kernel->sram_end_;
    insn->gemm_from_output = from_outmem;
    const std::vector<UopKernel::LoopEntry> &loop = kernel->loop();
    if (loop.size() > 0) {
      insn->iter_out = loop[0].extent;
      insn->wgt_factor_out = loop[0].wgt_factor;
      insn->src_factor_out = loop[0].src_factor;
      insn->dst_factor_out = loop[0].dst_factor;
    } else {
      insn->iter_out = 1;
      insn->wgt_factor_out = 0;
      insn->src_factor_out = 0;
      insn->dst_factor_out = 0;
    }
    if (loop.size() > 1) {
      insn->iter_in = loop[1].extent;
      insn->wgt_factor_in = loop[1].wgt_factor;
      insn->src_factor_in = loop[1].src_factor;
      insn->dst_factor_in = loop[1].dst_factor;
    } else {
      insn->iter_in = 1;
      insn->wgt_factor_in = 0;
      insn->src_factor_in = 0;
      insn->dst_factor_in = 0;
    }
  }
  // Push XP GEMM uop to the command buffer
  void PushXPGEMMOp(UopKernel* kernel, bool from_outmem=false) {
    uop_queue_.Push(kernel,
                    [this]() { this->AutoSync(); });
    if (uop_queue_.pending()) {
      VTAMemInsn* insn = insn_queue_.CreateMemInsn(VTA_MEM_ID_UOP);
      insn->opcode = VTA_OPCODE_LOAD;
      uop_queue_.FlushUopLoad(insn);
    }
    VTAGemInsn* insn = insn_queue_.CreateGemInsn();
    insn->opcode = VTA_OPCODE_XP_GEMM;
    insn->reset_reg = kernel->reset_out_;
    insn->uop_bgn = kernel->sram_begin_;
    insn->uop_end = kernel->sram_end_;
    insn->gemm_from_output = from_outmem;
    const std::vector<UopKernel::LoopEntry> &loop = kernel->loop();
    if (loop.size() > 0) {
      insn->iter_out = loop[0].extent;
      insn->wgt_factor_out = loop[0].wgt_factor;
      insn->src_factor_out = loop[0].src_factor;
      insn->dst_factor_out = loop[0].dst_factor;
    } else {
      insn->iter_out = 1;
      insn->wgt_factor_out = 0;
      insn->src_factor_out = 0;
      insn->dst_factor_out = 0;
    }
    if (loop.size() > 1) {
      insn->iter_in = loop[1].extent;
      insn->wgt_factor_in = loop[1].wgt_factor;
      insn->src_factor_in = loop[1].src_factor;
      insn->dst_factor_in = loop[1].dst_factor;
    } else {
      insn->iter_in = 1;
      insn->wgt_factor_in = 0;
      insn->src_factor_in = 0;
      insn->dst_factor_in = 0;
    }
  }
  // Push ALU uop to the command buffer
  void PushALUUop(UopKernel* kernel) {
    uop_queue_.Push(kernel,
                    [this]() { this->AutoSync(); });
    if (uop_queue_.pending()) {
      VTAMemInsn* insn = insn_queue_.CreateMemInsn(VTA_MEM_ID_UOP);
      insn->opcode = VTA_OPCODE_LOAD;
      uop_queue_.FlushUopLoad(insn);
    }
    VTAAluInsn* insn = insn_queue_.CreateAluInsn();
    insn->opcode = VTA_OPCODE_ALU;
    insn->reset_reg = kernel->reset_out_;
    insn->uop_bgn = kernel->sram_begin_;
    insn->uop_end = kernel->sram_end_;
    insn->alu_opcode = kernel->opcode_;
    insn->use_imm = kernel->use_imm_;
    insn->imm = kernel->imm_val_;
    const std::vector<UopKernel::LoopEntry> &loop = kernel->loop();
    if (loop.size() == 0) {
      insn->iter_out = 1;
      insn->dst_factor_out = 0;
      insn->src_factor_out = 0;
      insn->iter_in = 1;
      insn->dst_factor_in = 0;
      insn->src_factor_in = 0;
    } else if (loop.size() == 1) {
      insn->iter_out = 1;
      insn->dst_factor_out = 0;
      insn->src_factor_out = 0;
      insn->iter_in = loop[0].extent;
      insn->dst_factor_in = loop[0].dst_factor;
      insn->src_factor_in = loop[0].src_factor;
    } else {
      insn->iter_out = loop[0].extent;
      insn->dst_factor_out = loop[0].dst_factor;
      insn->src_factor_out = loop[0].src_factor;
      insn->iter_in = loop[1].extent;
      insn->dst_factor_in = loop[1].dst_factor;
      insn->src_factor_in = loop[1].src_factor;
    }
  }

  void CheckInsnOverFlow() {
    // At each API call, we can at most commit:
    // one pending store, one pending load, and one uop
    if ((insn_queue_.count() + 4) * sizeof(VTAGenericInsn) >= VTA_MAX_XFER) {
      this->AutoSync();
    }
  }
  // Auto sync when instruction overflow
  void AutoSync() {
    this->Synchronize(1 << 31);
  }

  // Internal debug flag
  int debug_flag_{ 0};//VTA_DEBUG_DUMP_INSN }; // org = 0
  // The kernel we are currently recording
  UopKernel* record_kernel_{nullptr};
  // Micro op queue
  UopQueue<VTA_MAX_XFER, kBufferCoherent, kAlwaysCache> uop_queue_;
  // instruction queue
  InsnQueue<VTA_MAX_XFER, kBufferCoherent, kAlwaysCache> insn_queue_;
  // Device handle
  VTADeviceHandle device_{nullptr};

  //#### JO added (2026-11-01): EVTA_STORE_SENTINEL=1 (hang hunt): before each run, the DRAM its STOREs write is filled with
  //#### 0xA5 through /dev/mem (uncached); on a timeout every STORE reports how many of its 16-byte vectors are no longer all
  //#### 0xA5, i.e. how far the stores got. Skipped for a run whose STORE span overlaps one of its LOADs. Costs time per run.
  static int SentinelOn() {
    static int on = -1;
    if (on < 0) {
      const char *e = getenv("EVTA_STORE_SENTINEL");
      on = e ? atoi(e) : 0;
    }
    return on;
  }
  // DRAM bytes [first, last) of a LOAD / STORE
  void MemSpan(const VTAMemInsn &m, uint64_t *first, uint64_t *last) {
    uint64_t eb = m.opcode == VTA_OPCODE_STORE ? GetElemBytes(VTA_MEM_ID_OUT) : GetElemBytes(m.memory_type);
    *first = (uint64_t)m.dram_base * eb;
    *last = *first + ((uint64_t)(m.y_size ? m.y_size - 1 : 0) * m.x_stride + m.x_size) * eb;
  }
  void StoreSentinelFill() {
    if (sent_map_) munmap(const_cast<uint64_t*>(sent_map_), sent_len_);
    sent_map_ = nullptr;
    if (!SentinelOn()) return;
    const VTAMemInsn *in = reinterpret_cast<const VTAMemInsn*>(insn_queue_.data());
    uint32_t n = insn_queue_.count();
    uint64_t lo = ~0ull, hi = 0, f, l;
    for (uint32_t i = 0; i < n; i++)
      if (in[i].opcode == VTA_OPCODE_STORE && in[i].x_size && in[i].y_size) {
        MemSpan(in[i], &f, &l);
        lo = std::min(lo, f);
        hi = std::max(hi, l);
      }
    if (hi <= lo || hi - lo > (64u << 20)) return;
    for (uint32_t i = 0; i < n; i++)
      if (in[i].opcode == VTA_OPCODE_LOAD && in[i].memory_type != VTA_MEM_ID_UOP && in[i].x_size && in[i].y_size) {
        MemSpan(in[i], &f, &l);
        if (f < hi && l > lo) return;
      }
    if (sent_fd_ < 0) sent_fd_ = open("/dev/mem", O_RDWR | O_SYNC);
    if (sent_fd_ < 0) return;
    uint64_t pg = getpagesize(), base = lo & ~(pg - 1);
    sent_len_ = hi - base;
    void *m = mmap(NULL, sent_len_, PROT_READ | PROT_WRITE, MAP_SHARED, sent_fd_, base);
    if (m == MAP_FAILED) return;
    sent_map_ = static_cast<volatile uint64_t*>(m);
    sent_base_ = base;
    //#### EVTA_STORE_SENTINEL=2: map only, no fill (which part of the fill changes the hangs)
    if (SentinelOn() == 2) {
      munmap(const_cast<uint64_t*>(sent_map_), sent_len_);
      sent_map_ = nullptr;
      return;
    }
    for (uint32_t i = 0; i < n; i++)
      if (in[i].opcode == VTA_OPCODE_STORE) {
        MemSpan(in[i], &f, &l);
        for (uint32_t y = 0; y < in[i].y_size; y++)
          for (uint32_t x = 0; x < in[i].x_size; x++) {
            uint64_t a = (f + ((uint64_t)y * in[i].x_stride + x) * VTA_INP_ELEM_BYTES - base) / 8;
            for (uint32_t k = 0; k < VTA_INP_ELEM_BYTES / 8; k++) sent_map_[a + k] = 0xA5A5A5A5A5A5A5A5ull;
          }
      }
    // the mapping stays until the next run (StoreSentinelReport reads it on a timeout)
    __sync_synchronize();
  }
  void StoreSentinelReport() {
    unsigned q = (unsigned)insn_queue_.dram_phy_addr();
    if (!sent_map_) {
      if (SentinelOn()) fprintf(stderr, "store sentinel (insn 0x%08x): not set for this run\n", q);
      return;
    }
    // one block per EVTA (the threads of a multi-EVTA executable print at the same time): the counts, then the STOREs
    // that are not complete (at most 12)
    const VTAMemInsn *in = reinterpret_cast<const VTAMemInsn*>(insn_queue_.data());
    uint32_t n = insn_queue_.count(), k = 0, full = 0, part = 0, none = 0, shown = 0;
    uint64_t f, l;
    char line[256];
    std::string det;
    for (uint32_t i = 0; i < n; i++)
      if (in[i].opcode == VTA_OPCODE_STORE) {
        MemSpan(in[i], &f, &l);
        uint32_t tot = in[i].y_size * in[i].x_size, wr = 0, firstun = tot;
        for (uint32_t y = 0; y < in[i].y_size; y++)
          for (uint32_t x = 0; x < in[i].x_size; x++) {
            uint64_t a = (f + ((uint64_t)y * in[i].x_stride + x) * VTA_INP_ELEM_BYTES - sent_base_) / 8;
            bool s = true;
            for (uint32_t j = 0; j < VTA_INP_ELEM_BYTES / 8; j++) s = s && sent_map_[a + j] == 0xA5A5A5A5A5A5A5A5ull;
            if (!s) wr++;
            else if (firstun == tot) firstun = y * in[i].x_size + x;
          }
        if (wr == tot) full++;
        else {
          wr ? part++ : none++;
          if (shown++ < 12) {
            snprintf(line, sizeof(line), "  STORE %u (insn %u, sram %u, dram 0x%08llx, %ux%u stride %u): %u of %u vectors written,"
                     " first unwritten %u\n", k, i, (unsigned)in[i].sram_base, (unsigned long long)f, (unsigned)in[i].y_size,
                     (unsigned)in[i].x_size, (unsigned)in[i].x_stride, wr, tot, firstun);
            det += line;
          }
        }
        k++;
      }
    fprintf(stderr, "store sentinel (insn 0x%08x): %u STOREs, %u written, %u partly, %u not at all\n%s", q, k, full, part,
            none, det.c_str());
  }
  //#### JO added (2026-11-01): EVTA_PRE_RUN_US=<n>: busy wait n us before each run (hang hunt: start timing only);
  //#### EVTA_PRE_RUN_MASK=<bits> (default all): only on the EVTAs whose bit is set (2026-11-02)
  void PreRunWait() {
    static long us = -1, mask = -1;
    if (us < 0) {
      const char *e = getenv("EVTA_PRE_RUN_US");
      us = e ? atol(e) : 0;
      e = getenv("EVTA_PRE_RUN_MASK");
      mask = e ? strtol(e, nullptr, 0) : -1;
      if (mask < 0) mask = 0x7fffffff;
    }
    if (us <= 0 || !((mask >> evta_index_) & 1)) return;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do clock_gettime(CLOCK_MONOTONIC, &t1);
    while ((t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000 < us);
  }
  int evta_index_{0};
  int sent_fd_{-1};
  volatile uint64_t *sent_map_{nullptr};
  uint64_t sent_base_{0}, sent_len_{0};
};

}

#endif
