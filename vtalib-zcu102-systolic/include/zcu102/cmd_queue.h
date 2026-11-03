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
#include <unordered_map>
#include <vector>

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
      if (rec_) RecordReloc(src, insn->dram_base, GetElemBytes(dst_memory_type));
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
    if (rec_) RecordReloc(dst, insn->dram_base, GetElemBytes(src_memory_type));
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
    //#### JO added (2026-11-02): EVTA_PRE_RUN_AT=1: the pre-run wait before the copy and cache flush of the queues
    //#### instead of after them (hang hunt: time since the last run vs time since the flush)
    if (PreRunAt() == 1) PreRunWait();
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
    if (rec_) RecordSegment();

    //#### JO added (2026-11-01): EVTA_STORE_SENTINEL=1 fills the DRAM the STOREs of this run write with 0xA5 first
    StoreSentinelFill();
    if (PreRunAt() == 0) PreRunWait();
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
    //#### JO added (2026-11-02): EVTA_DOUBLE_QUEUE=1: the next run's uops and instructions in the other buffers
    //#### (the uop LOADs take the buffer's address when they are made, so swap before the next run is built)
    if (DoubleQueue()) {
      uop_queue_.SwapBuffer();
      insn_queue_.SwapBuffer();
    }
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

  //#### JO added (2026-11-02): record / replay of an EVTA call (AOT stage 0, docs/evta_runtime_aot.md 5.2).
  //#### The bundle entry (conv, maxpool, fc) calls ReplayBegin with a key (its scalar arguments) and its VTA buffers.
  //#### The first call with a key records: the uop SRAM is marked empty (every uop of the call is LOADed inside it),
  //#### each run (Synchronize) of the call is copied after its queues are built (instructions, uops; the uop LOADs
  //#### point at the copy) and each LOAD / STORE of one of the buffers notes its offset from that buffer; ReplayEnd
  //#### keeps it. A later call with the key runs the copies (the buffers' addresses patched in if they moved) and
  //#### generates nothing. The relocation table is what a compile-time instruction blob needs as well (stage 1).
  //#### EVTA_REPLAY=0 turns it off; EVTA_REPLAY_MB (default 16) limits the copies per EVTA; EVTA_REPLAY_LOG=1 prints them.
  int ReplayBegin(const void *key, int key_bytes, void *const *bufs, int nbufs) {
    if (!ReplayOn()) return 0;
    if (rec_) DropRecording();
    if (insn_queue_.count() != 0) return 0;
    std::string k(static_cast<const char *>(key), key_bytes);
    auto it = replays_.find(k);
    if (it != replays_.end()) {
      RunReplay(*it->second, bufs, nbufs);
      return 1;
    }
    if (replay_bytes_ >= ReplayLimit()) return 0;
    rec_ = new Replay;
    rec_key_ = k;
    for (int i = 0; i < nbufs; i++) {
      rec_->bufs.push_back(bufs[i] ? DataBuffer::FromHandle(bufs[i])->phy_addr() : 0);
      rec_bufs_.push_back(bufs[i] ? DataBuffer::FromHandle(bufs[i]) : nullptr);
    }
    uop_queue_.UopReset();
    return 0;
  }
  void ReplayEnd(bool ok) {
    if (!rec_) return;
    if (!ok || !rec_ok_ || rec_->segs.empty() || insn_queue_.count() != 0) {
      DropRecording();
      return;
    }
    replay_bytes_ += rec_->bytes;
    rec_->id = (uint32_t)replays_.size();
    if (ReplayDumpDir())
      for (size_t i = 0; i < rec_->segs.size(); i++) DumpSegment(ReplayDumpDir(), rec_->id, i, rec_->segs[i]);
    MemExperiment(*rec_);
    if (ReplayLog())
      fprintf(stderr, "EVTA %d replay %zu: %zu runs, %u instructions, %zu relocations, %zu bytes (%zu in all)\n",
              evta_index_, replays_.size(), rec_->segs.size(), rec_->insns, rec_->relocs.size(), rec_->bytes,
              replay_bytes_);
    replays_[rec_key_] = rec_;
    rec_ = nullptr;
    rec_bufs_.clear();
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
  //#### JO added (2026-11-02): record / replay, see ReplayBegin
  struct ReplaySeg {
    void *insn{nullptr}, *uop{nullptr};
    vta_phy_addr_t insn_phy{0}, uop_phy{0};
    uint32_t count{0}, uop_bytes{0};
    // what the run does (EVTA_REPLAY_LOG=2): GEMM / ALU inner iterations, DRAM bytes of the LOADs (uop, wgt, inp, acc)
    // and of the STOREs (out)
    uint64_t gemm{0}, alu{0}, bytes[5]{};
  };
  struct ReplayReloc {
    uint32_t seg, idx, buf, elem_bytes, elem_offset;
  };
  struct Replay {
    std::vector<ReplaySeg> segs;
    std::vector<ReplayReloc> relocs;
    std::vector<vta_phy_addr_t> bufs;  // the buffers' addresses in the copies
    size_t bytes{0};
    uint32_t insns{0}, id{0};
  };
  static int ReplayEnv(const char *name, int def) {
    const char *e = getenv(name);
    return e ? atoi(e) : def;
  }
  static bool ReplayOn() {
    static const bool on = ReplayEnv("EVTA_REPLAY", 1) > 0;
    return on;
  }
  static bool ReplayLog() {
    static const bool on = ReplayEnv("EVTA_REPLAY_LOG", 0) > 0;
    return on;
  }
  static int ReplayLogLevel() {
    static const int level = ReplayEnv("EVTA_REPLAY_LOG", 0);
    return level;
  }
  //#### JO added (2026-11-02): EVTA_REPLAY_DUMP=<dir> writes each recorded segment as text, rec_<id>_<seg>.txt: one line per
  //#### instruction (opcode, dependency flags, the fields) and the uops (dst src wgt)
  static const char *ReplayDumpDir() {
    static const char *d = getenv("EVTA_REPLAY_DUMP");
    return d;
  }
  static void DumpSegment(const char *dir, uint32_t id, size_t seg, const ReplaySeg &g) {
    char name[512];
    snprintf(name, sizeof(name), "%s/rec_%u_%zu.txt", dir, id, seg);
    FILE *fp = fopen(name, "w");
    if (!fp) return;
    static const char *op[] = {"LOAD", "STORE", "GEMM", "FINISH", "ALU", "XPGEMM", "?6", "?7"};
    for (uint32_t i = 0; i < g.count; i++) {
      VTAGenericInsn *p = static_cast<VTAGenericInsn *>(g.insn) + i;
      fprintf(fp, "%u %s %d%d%d%d", i, op[p->opcode & 7], (int)p->pop_prev_dep, (int)p->pop_next_dep, (int)p->push_prev_dep,
              (int)p->push_next_dep);
      if (p->opcode == VTA_OPCODE_LOAD || p->opcode == VTA_OPCODE_STORE) {
        VTAMemInsn *m = reinterpret_cast<VTAMemInsn *>(p);
        fprintf(fp, " mem %d sram %u dram %u y %u x %u stride %u pad %u %u %u %u", (int)m->memory_type, (unsigned)m->sram_base,
                (unsigned)m->dram_base, (unsigned)m->y_size, (unsigned)m->x_size, (unsigned)m->x_stride, (unsigned)m->y_pad_0,
                (unsigned)m->y_pad_1, (unsigned)m->x_pad_0, (unsigned)m->x_pad_1);
      } else if (p->opcode == VTA_OPCODE_GEMM) {
        VTAGemInsn *x = reinterpret_cast<VTAGemInsn *>(p);
        //#### JO changed (2026-11-04): reset 2 = bias reset
        fprintf(fp, " reset %d uop %u %u iter %u %u dst %u %u src %u %u wgt %u %u", (int)x->reset_reg + (int)x->bias_reset, (unsigned)x->uop_bgn,
                (unsigned)x->uop_end, (unsigned)x->iter_out, (unsigned)x->iter_in, (unsigned)x->dst_factor_out,
                (unsigned)x->dst_factor_in, (unsigned)x->src_factor_out, (unsigned)x->src_factor_in,
                (unsigned)x->wgt_factor_out, (unsigned)x->wgt_factor_in);
      } else if (p->opcode == VTA_OPCODE_ALU) {
        VTAAluInsn *x = reinterpret_cast<VTAAluInsn *>(p);
        fprintf(fp, " reset %d uop %u %u iter %u %u dst %u %u src %u %u alu %d imm %d %d", (int)x->reset_reg,
                (unsigned)x->uop_bgn, (unsigned)x->uop_end, (unsigned)x->iter_out, (unsigned)x->iter_in,
                (unsigned)x->dst_factor_out, (unsigned)x->dst_factor_in, (unsigned)x->src_factor_out,
                (unsigned)x->src_factor_in, (int)x->alu_opcode, (int)x->use_imm, (int)x->imm);
      }
      fputc('\n', fp);
    }
    const VTAUop *u = static_cast<const VTAUop *>(g.uop);
    for (uint32_t i = 0; u && i < g.uop_bytes / sizeof(VTAUop); i++)
      fprintf(fp, "uop %u %u %u %u\n", i, (unsigned)u[i].dst_idx, (unsigned)u[i].src_idx, (unsigned)u[i].wgt_idx);
    fclose(fp);
  }
  //#### JO added (2026-11-02): DRAM traffic experiment (docs/evta_runtime_compare.md 7.11), results are garbage:
  //#### EVTA_MEMEXP bit 0 / 1 / 2 / 3 shrinks the inp / wgt / acc LOADs / the STOREs of the replayed copies to one element
  //#### (pads kept), so a replay times the run without that traffic
  static int MemExpMask() {
    static const int mask = ReplayEnv("EVTA_MEMEXP", 0);
    return mask;
  }
  static void MemExperiment(Replay &r) {
    const int mask = MemExpMask();
    if (!mask) return;
    for (auto &g : r.segs) {
      for (uint32_t i = 0; i < g.count; i++) {
        VTAMemInsn *m = InsnAt(g.insn, i);
        int bit = -1;
        if (m->opcode == VTA_OPCODE_STORE) bit = 3;
        else if (m->opcode == VTA_OPCODE_LOAD && m->memory_type == VTA_MEM_ID_INP) bit = 0;
        else if (m->opcode == VTA_OPCODE_LOAD && m->memory_type == VTA_MEM_ID_WGT) bit = 1;
        else if (m->opcode == VTA_OPCODE_LOAD && m->memory_type == VTA_MEM_ID_ACC) bit = 2;
        if (bit >= 0 && (mask >> bit & 1) && m->x_size) {
          m->x_size = 1;
          m->y_size = 1;
        }
      }
      VTAFlushCache(g.insn, g.insn_phy, g.count * sizeof(VTAGenericInsn));
    }
  }
  static void CountSegment(ReplaySeg &g) {
    for (uint32_t i = 0; i < g.count; i++) {
      VTAGenericInsn *p = static_cast<VTAGenericInsn *>(g.insn) + i;
      if (p->opcode == VTA_OPCODE_GEMM) {
        VTAGemInsn *x = reinterpret_cast<VTAGemInsn *>(p);
        g.gemm += (uint64_t)x->iter_out * x->iter_in * (x->uop_end - x->uop_bgn);
      } else if (p->opcode == VTA_OPCODE_ALU) {
        VTAAluInsn *x = reinterpret_cast<VTAAluInsn *>(p);
        g.alu += (uint64_t)x->iter_out * x->iter_in * (x->uop_end - x->uop_bgn);
      } else if (p->opcode == VTA_OPCODE_LOAD || p->opcode == VTA_OPCODE_STORE) {
        VTAMemInsn *m = reinterpret_cast<VTAMemInsn *>(p);
        static const uint32_t eb[4] = {VTA_UOP_ELEM_BYTES, VTA_WGT_ELEM_BYTES, VTA_INP_ELEM_BYTES, VTA_ACC_ELEM_BYTES};
        uint64_t n = (uint64_t)m->x_size * m->y_size;
        if (p->opcode == VTA_OPCODE_STORE) g.bytes[4] += n * VTA_OUT_ELEM_BYTES;
        else g.bytes[m->memory_type] += n * eb[m->memory_type];
      }
    }
  }
  static size_t ReplayLimit() {
    static const size_t mb = ReplayEnv("EVTA_REPLAY_MB", 16);
    return mb << 20;
  }
  static void FreeReplay(Replay *r) {
    for (auto &g : r->segs) {
      if (g.insn) VTAMemFree(g.insn);
      if (g.uop) VTAMemFree(g.uop);
    }
    delete r;
  }
  static VTAMemInsn *InsnAt(void *insns, uint32_t i) {
    return reinterpret_cast<VTAMemInsn *>(static_cast<VTAGenericInsn *>(insns) + i);
  }
  void DropRecording() {
    FreeReplay(rec_);
    rec_ = nullptr;
    rec_bufs_.clear();
    rec_ok_ = true;
  }
  // LOAD / STORE just made (the last instruction) addresses buffer b: note which argument and the offset
  void RecordReloc(DataBuffer *b, uint32_t dram_base, uint32_t elem_bytes) {
    for (size_t i = 0; i < rec_bufs_.size(); i++)
      if (rec_bufs_[i] == b) {
        rec_->relocs.push_back({(uint32_t)rec_->segs.size(), insn_queue_.count() - 1, (uint32_t)i, elem_bytes,
                                dram_base - (uint32_t)(b->phy_addr() / elem_bytes)});
        return;
      }
    // another buffer (identity weights, fc staging): kept for the program's life, its address stays
  }
  // a run of the recorded call, its queues built and flushed: copy them
  void RecordSegment() {
    ReplaySeg g;
    g.count = insn_queue_.count();
    size_t ib = g.count * sizeof(VTAGenericInsn);
    g.insn = VTAMemAlloc(ib, 1);
    g.uop_bytes = uop_queue_.flushed_bytes();
    if (g.uop_bytes) g.uop = VTAMemAlloc(g.uop_bytes, 1);
    if (!g.insn || (g.uop_bytes && !g.uop)) {
      if (g.insn) VTAMemFree(g.insn);
      rec_ok_ = false;
      return;
    }
    g.insn_phy = VTAMemGetPhyAddr(g.insn);
    memcpy(g.insn, insn_queue_.data(), ib);
    if (g.uop_bytes) {
      g.uop_phy = VTAMemGetPhyAddr(g.uop);
      memcpy(g.uop, uop_queue_.fpga_data(), g.uop_bytes);
      VTAFlushCache(g.uop, g.uop_phy, g.uop_bytes);
    }
    // the uop LOADs read the uop queue's buffer: point them at the copy
    const uint32_t eb = VTA_UOP_ELEM_BYTES, q = uop_queue_.dram_phy_addr() / eb;
    for (uint32_t i = 0; i < g.count; i++) {
      VTAMemInsn *m = InsnAt(g.insn, i);
      if (m->opcode == VTA_OPCODE_LOAD && m->memory_type == VTA_MEM_ID_UOP && m->x_size) {
        if (!g.uop || m->dram_base < q || (m->dram_base - q) * eb >= g.uop_bytes) rec_ok_ = false;
        m->dram_base = m->dram_base - q + g.uop_phy / eb;
      }
    }
    VTAFlushCache(g.insn, g.insn_phy, ib);
    CountSegment(g);
    rec_->segs.push_back(g);
    rec_->bytes += ib + g.uop_bytes;
    rec_->insns += g.count;
  }
  void RunReplay(Replay &r, void *const *bufs, int nbufs) {
    CHECK_EQ((size_t)nbufs, r.bufs.size());
    bool moved = false;
    for (int i = 0; i < nbufs; i++) {
      vta_phy_addr_t a = bufs[i] ? DataBuffer::FromHandle(bufs[i])->phy_addr() : 0;
      if (a != r.bufs[i]) moved = true;
      r.bufs[i] = a;
    }
    if (moved) {
      for (auto &c : r.relocs)
        InsnAt(r.segs[c.seg].insn, c.idx)->dram_base = (uint32_t)(r.bufs[c.buf] / c.elem_bytes) + c.elem_offset;
      for (auto &g : r.segs) VTAFlushCache(g.insn, g.insn_phy, g.count * sizeof(VTAGenericInsn));
    }
    for (size_t s = 0; s < r.segs.size(); s++) {
      ReplaySeg &g = r.segs[s];
      PreRunWait();
      struct timespec t0, t1;
      if (ReplayLogLevel() >= 2) clock_gettime(CLOCK_MONOTONIC, &t0);
      int timeout = VTADeviceRun(device_, g.insn_phy, g.count, 1u << 31);
      if (ReplayLogLevel() >= 2) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        fprintf(stderr, "EVTA_RUN %d %u %zu %u %.1f %llu %llu %llu %llu %llu %llu %llu\n", evta_index_, r.id, s, g.count,
                (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) * 1e-3, (unsigned long long)g.gemm,
                (unsigned long long)g.alu, (unsigned long long)g.bytes[0], (unsigned long long)g.bytes[1],
                (unsigned long long)g.bytes[2], (unsigned long long)g.bytes[3], (unsigned long long)g.bytes[4]);
      }
      if (timeout) {
        char name[128];
        snprintf(name, sizeof(name), "/tmp/evta_hang_%d_%08x.insn", (int)getpid(), (unsigned)g.insn_phy);
        FILE *fp = fopen(name, "wb");
        if (fp) {
          fwrite(g.insn, sizeof(VTAGenericInsn), g.count, fp);
          fclose(fp);
          fprintf(stderr, "instructions of the replayed run saved in %s (%u x %zu bytes)\n", name, g.count,
                  sizeof(VTAGenericInsn));
        }
        fflush(stdout);
        fflush(stderr);
        _exit(5);
      }
    }
    // the uop SRAM now holds the copies' uops
    uop_queue_.UopReset();
  }
  std::unordered_map<std::string, Replay *> replays_;
  Replay *rec_{nullptr};
  bool rec_ok_{true};
  std::string rec_key_;
  std::vector<DataBuffer *> rec_bufs_;
  size_t replay_bytes_{0};

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
    //#### JO changed (2026-11-04, bias reset): VTAUopPush reset_out 2 = reset to acc[src] (bias_reset, bitstreams with
    //#### "bias" in the name; VTABiasReset() in pynq_driver.cc)
    //insn->reset_reg = kernel->reset_out_;
    insn->reset_reg = kernel->reset_out_ == 2 ? 1 : kernel->reset_out_;
    insn->bias_reset = kernel->reset_out_ == 2;
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
  //#### JO changed (2026-11-02): default 5 us (multi4 runs hung about once per 400 inferences without it,
  //#### docs/evta_runtime_compare.md 7.7); EVTA_PRE_RUN_US=0 turns it off
  //#### JO changed (2026-11-02): default 0 again (the hang was the missing dsb after the cache flush, pynq_driver.cc)
  static int PreRunAt() {
    static int at = -1;
    if (at < 0) {
      const char *e = getenv("EVTA_PRE_RUN_AT");
      at = e ? atoi(e) : 0;
    }
    return at;
  }
  static bool DoubleQueue() {
    static int on = -1;
    if (on < 0) {
      const char *e = getenv("EVTA_DOUBLE_QUEUE");
      on = e ? atoi(e) : 0;
    }
    return on;
  }
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
