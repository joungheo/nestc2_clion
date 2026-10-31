#include <iostream>
#include <ctime>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <vector>
#include <algorithm>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cassert>
#include "llvm/ADT/ArrayRef.h"

#include <pthread.h>
#include <sched.h>

#include <arm_neon.h>
#include <omp.h>

#if(!VTALIB_ONLY)
#include "vta/vtalib/include/Bundle/VTABundle.h"
#else
#include "Bundle/VTABundle.h"
#endif


#include <arm_compute/core/Types.h>
#include <arm_compute/core/ITensor.h>
#include <arm_compute/runtime/NEON/functions/NEConvolutionLayer.h>
#include <arm_compute/runtime/NEON/functions/NEPoolingLayer.h>
#include <arm_compute/runtime/NEON/functions/NEReshapeLayer.h>
#include <arm_compute/runtime/NEON/functions/NEFullyConnectedLayer.h>


#include <linux/perf_event.h>
#include <asm/unistd.h>
#define DEBUG_INFO  0

using namespace std;
using namespace arm_compute;

struct PerfEvent {
  perf_event_attr pe;
  int fd = -1;

  PerfEvent(std::uint32_t type, std::uint64_t config) {
    memset(&pe, 0, sizeof(pe));
    pe.size = sizeof(pe);
    pe.type = type;
    pe.config = config;
    pe.disabled = 1;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    fd = syscall(__NR_perf_event_open, &pe, 0, -1, -1, 0);
    if (fd == -1) {
      fprintf(stderr, "perf_event_open failed for config 0x%lx\n", config);
      abort();
    }
  }

  void Start() {
    ioctl(fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
  }

  std::int64_t Stop() {
    ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
    std::int64_t count = 0;
    read(fd, &count, sizeof(count));
    return count;
  }

  ~PerfEvent() { close(fd); }
};

struct ArmPmuEvent : PerfEvent {
  static constexpr std::uint16_t L1I_CACHE_REFILL = 0x01;
  static constexpr std::uint16_t L1I_TLB_REFILL = 0x02;
  static constexpr std::uint16_t L1D_CACHE_REFILL = 0x03;
  static constexpr std::uint16_t L1D_CACHE = 0x04;
  static constexpr std::uint16_t L1D_TLB_REFILL = 0x05;
  static constexpr std::uint16_t LD_RETIRED = 0x06;
  static constexpr std::uint16_t ST_RETIRED = 0x07;
  static constexpr std::uint16_t INST_RETIRED = 0x08;
  static constexpr std::uint16_t EXC_TAKEN = 0x09;
  static constexpr std::uint16_t EXC_RETURN = 0x0A;
  static constexpr std::uint16_t CID_WRITE_RETIRED = 0x0B;
  static constexpr std::uint16_t PC_WRITE_RETIRED = 0x0C;
  static constexpr std::uint16_t BR_IMMED_RETIRED = 0x0D;
  static constexpr std::uint16_t BR_RETURN_RETIRED = 0x0E;
  static constexpr std::uint16_t UNALIGNED_LDST_RETIRED = 0x0F;
  static constexpr std::uint16_t BR_MIS_PRED = 0x10;
  static constexpr std::uint16_t CPU_CYCLES = 0x11;
  static constexpr std::uint16_t BR_PRED = 0x12;
  static constexpr std::uint16_t MEM_ACCESS = 0x13;
  static constexpr std::uint16_t L1I_CACHE = 0x14;
  static constexpr std::uint16_t L1D_CACHE_WB = 0x15;
  static constexpr std::uint16_t L2D_CACHE = 0x16;
  static constexpr std::uint16_t L2D_CACHE_REFILL = 0x17;
  static constexpr std::uint16_t L2D_CACHE_WB = 0x18;
  static constexpr std::uint16_t BUS_ACCESS = 0x19;
  static constexpr std::uint16_t MEMORY_ERROR = 0x1A;
  static constexpr std::uint16_t INST_SPEC = 0x1B;
  static constexpr std::uint16_t TTBR_WRITE_RETIRED = 0x1C;
  static constexpr std::uint16_t BUS_CYCLES = 0x1D;
  static constexpr std::uint16_t CHAIN = 0x1E;
  static constexpr std::uint16_t L1D_CACHE_ALLOCATE = 0x1F;
  static constexpr std::uint16_t L2D_CACHE_ALLOCATE = 0x20;
  static constexpr std::uint16_t BR_RETIRED = 0x21;
  static constexpr std::uint16_t BR_MIS_PRED_RETIRED = 0x22;
  static constexpr std::uint16_t STALL_FRONTEND = 0x23;
  static constexpr std::uint16_t STALL_BACKEND = 0x24;
  static constexpr std::uint16_t L1D_TLB = 0x25;
  static constexpr std::uint16_t L1I_TLB = 0x26;
  static constexpr std::uint16_t L2I_CACHE = 0x27;
  static constexpr std::uint16_t L2I_CACHE_REFILL = 0x28;
  static constexpr std::uint16_t L3D_CACHE_ALLOCATE = 0x29;
  static constexpr std::uint16_t L3D_CACHE_REFILL = 0x2A;
  static constexpr std::uint16_t L3D_CACHE = 0x2B;
  static constexpr std::uint16_t L3D_CACHE_WB = 0x2C;
  static constexpr std::uint16_t L2D_TLB_REFILL = 0x2D;
  static constexpr std::uint16_t L2I_TLB_REFILL = 0x2E;
  static constexpr std::uint16_t L2D_TLB = 0x2F;
  static constexpr std::uint16_t L2I_TLB = 0x30;
  static constexpr std::uint16_t LL_CACHE = 0x32;
  static constexpr std::uint16_t LL_CACHE_MISS = 0x33;
  static constexpr std::uint16_t DTLB_WALK = 0x34;
  static constexpr std::uint16_t LL_CACHE_RD = 0x36;
  static constexpr std::uint16_t LL_CACHE_MISS_RD = 0x37;
  static constexpr std::uint16_t L1D_CACHE_RD = 0x40;
  static constexpr std::uint16_t L1D_CACHE_REFILL_RD = 0x42;
  static constexpr std::uint16_t L1D_TLB_REFILL_RD = 0x4C;
  static constexpr std::uint16_t L1D_TLB_RD = 0x4E;
  static constexpr std::uint16_t L2D_CACHE_RD = 0x50;
  static constexpr std::uint16_t L2D_CACHE_REFILL_RD = 0x52;
  static constexpr std::uint16_t BUS_ACCESS_RD = 0x60;
  static constexpr std::uint16_t MEM_ACCESS_RD = 0x66;
  static constexpr std::uint16_t L3D_CACHE_RD = 0xA0;
  static constexpr std::uint16_t L3D_CACHE_REFILL_RD = 0xA2;
  ArmPmuEvent(std::uint16_t number) : PerfEvent(PERF_TYPE_RAW, number) {}
};

struct CacheCounts {
  int st_retired = 0;
  int ld_retired = 0;
  int mem_access = 0;
  int ll_cache = 0;
  int ll_cache_miss = 0;
  int l1d_tlb = 0;
  int l1d_tlb_refill = 0;
  int l1d_cache = 0;
  int l1d_cache_refill = 0;
  int l2d_cache = 0;
  int l2d_cache_refill = 0;
  int l3d_cache = 0;
  int l3d_cache_refill = 0;
  int cpu_cycle = 0;
};

void PrintCacheCounts(const CacheCounts& cache_counts) {
  printf("st_retired = %d\n", cache_counts.st_retired);
  printf("ld_retired = %d\n", cache_counts.ld_retired);
  printf("mem_access = %d\n", cache_counts.mem_access);
  printf("ll_cache = %d\n", cache_counts.ll_cache);
  printf("ll_cache_miss = %d\n", cache_counts.ll_cache_miss);
  printf("l1d_tlb = %d\n", cache_counts.l1d_tlb);
  printf("l1d_tlb_refill = %d\n", cache_counts.l1d_tlb_refill);
  printf("l1d_cache = %d\n", cache_counts.l1d_cache);
  printf("l1d_cache_refill = %d\n", cache_counts.l1d_cache_refill);
  printf("l2d_cache = %d\n", cache_counts.l2d_cache);
  printf("l2d_cache_refill = %d\n", cache_counts.l2d_cache_refill);
  printf("l3d_cache = %d\n", cache_counts.l3d_cache);
  printf("l3d_cache_refill = %d\n", cache_counts.l3d_cache_refill);
  printf("cpu cycle= %d\n", cache_counts.cpu_cycle);

}

void PrintCacheCountsRow(const CacheCounts& cache_counts) {
 printf("%d,", cache_counts.st_retired);  
  printf("%d,", cache_counts.ld_retired);
  printf("%d,", cache_counts.mem_access);
  printf("%d,", cache_counts.ll_cache);
  printf("%d,", cache_counts.ll_cache_miss);
  printf("%d,", cache_counts.l1d_tlb);
  printf("%d,", cache_counts.l1d_tlb_refill);  
  printf("%d,", cache_counts.l1d_cache);
  printf("%d,", cache_counts.l1d_cache_refill);
  printf("%d,", cache_counts.l2d_cache);
  printf("%d,", cache_counts.l2d_cache_refill);
  printf("%d,", cache_counts.l3d_cache);
  printf("%d,", cache_counts.l3d_cache_refill);
  printf("%d\n", cache_counts.cpu_cycle);

}

const bool only_reads = false;
ArmPmuEvent st_retired(ArmPmuEvent::ST_RETIRED);
ArmPmuEvent ld_retired(ArmPmuEvent::LD_RETIRED);
ArmPmuEvent mem_access(only_reads ? ArmPmuEvent::MEM_ACCESS_RD
                                  : ArmPmuEvent::MEM_ACCESS);
ArmPmuEvent ll_cache(only_reads ? ArmPmuEvent::LL_CACHE_RD
                                : ArmPmuEvent::LL_CACHE);
ArmPmuEvent ll_cache_miss(only_reads ? ArmPmuEvent::LL_CACHE_MISS_RD
                                     : ArmPmuEvent::LL_CACHE_MISS);
ArmPmuEvent l1d_cache(only_reads ? ArmPmuEvent::L1D_CACHE_RD
                                 : ArmPmuEvent::L1D_CACHE);
ArmPmuEvent l1d_cache_refill(only_reads ? ArmPmuEvent::L1D_CACHE_REFILL_RD
                                        : ArmPmuEvent::L1D_CACHE_REFILL);
ArmPmuEvent l1d_tlb(only_reads ? ArmPmuEvent::L1D_TLB_RD
                               : ArmPmuEvent::L1D_TLB);
ArmPmuEvent l1d_tlb_refill(only_reads ? ArmPmuEvent::L1D_TLB_REFILL_RD
                                      : ArmPmuEvent::L1D_TLB_REFILL);
ArmPmuEvent l2d_cache(only_reads ? ArmPmuEvent::L2D_CACHE_RD
                                 : ArmPmuEvent::L2D_CACHE);
ArmPmuEvent l2d_cache_refill(only_reads ? ArmPmuEvent::L2D_CACHE_REFILL_RD
                                        : ArmPmuEvent::L2D_CACHE_REFILL);
ArmPmuEvent l3d_cache(only_reads ? ArmPmuEvent::L3D_CACHE_RD
                                 : ArmPmuEvent::L3D_CACHE);
ArmPmuEvent l3d_cache_refill(only_reads ? ArmPmuEvent::L3D_CACHE_REFILL_RD
                                        : ArmPmuEvent::L3D_CACHE_REFILL);
ArmPmuEvent cpu_l3d_cache_refill(only_reads ? ArmPmuEvent::L3D_CACHE_REFILL_RD
                                            : ArmPmuEvent::L3D_CACHE_REFILL);
ArmPmuEvent cpu_cycle(ArmPmuEvent::CPU_CYCLES);

void StartCacheCounts() {
  st_retired.Start();
  ld_retired.Start();
  mem_access.Start();
  ll_cache.Start();
  ll_cache_miss.Start();
  l1d_cache.Start();
  l1d_cache_refill.Start();
  l1d_tlb.Start();
  l1d_tlb_refill.Start();

  l2d_cache.Start();
  l2d_cache_refill.Start();
  l3d_cache.Start();
  l3d_cache_refill.Start();
  cpu_cycle.Start();
}

void StopCacheCounts(CacheCounts* cache_counts)
{
  cache_counts->st_retired = st_retired.Stop();
  cache_counts->ld_retired = ld_retired.Stop();
  cache_counts->mem_access = mem_access.Stop();
  cache_counts->ll_cache = ll_cache.Stop();
  cache_counts->ll_cache_miss = ll_cache_miss.Stop();
  cache_counts->l1d_cache = l1d_cache.Stop();
  cache_counts->l1d_cache_refill = l1d_cache_refill.Stop();
  cache_counts->l1d_tlb = l1d_tlb.Stop();
  cache_counts->l1d_tlb_refill = l1d_tlb_refill.Stop();
  cache_counts->l2d_cache = l2d_cache.Stop();
  cache_counts->l2d_cache_refill = l2d_cache_refill.Stop();
  cache_counts->l3d_cache = l3d_cache.Stop();
  cache_counts->l3d_cache_refill = l3d_cache_refill.Stop();
  cache_counts->cpu_cycle = cpu_cycle.Stop();
}


inline void nhwc_to_nchw_s8(
    const int8_t* const in,  // Input data in NHWC form
    int8_t* const out,       // Output data in NCHW form
    const int n_batches,
    const int n_rows,
    const int n_cols,
    const int n_channels
)
{
  // Fill in stride values
  int in_col_stride = n_channels;
  int in_row_stride = n_cols * in_col_stride;
  int in_batch_stride = n_rows * in_row_stride;

  int out_row_stride = n_cols;
  int out_channel_stride = n_rows * out_row_stride;
  int out_batch_stride = n_channels * out_channel_stride;

  // Perform the re-ordering
  // For every batch
  for (int n = 0; n < n_batches; n++)
  {
    const int8_t* const in_batch = in + n*in_batch_stride;
    int8_t* const out_batch = out + n*out_batch_stride;

    // For every row
    for (int i = 0; i < n_rows; i++)
    {
      const int8_t* const in_i = in_batch + i*in_row_stride;
      int8_t* const out_i = out_batch + i*out_row_stride;

      // For every column
      for (int j = 0; j < n_cols; j++)
      {
        const int8_t* const in_j = in_i + j*in_col_stride;
        int8_t* const out_j = out_i + j;

        // For every channel
        for (int c = 0; c < n_channels; c++)
        {
          const int8_t* const in_channel = in_j + c;
          int8_t* const out_channel = out_j + c*out_channel_stride;
          *(out_channel) = *(in_channel);
        }
      }
    }
  }
}
// only channel 3. width 16���
inline void nhwc_to_nchw_s8_c3_w16n(
    const int8_t* const in,  // Input data in NHWC form
    int8_t* const out,       // Output data in NCHW form
    const int n_batches,
    const int n_rows,
    const int n_cols,
    const int n_channels
)
{
  // Fill in stride values
  int in_col_stride = n_channels;
  int in_row_stride = n_cols * in_col_stride;
  int in_batch_stride = n_rows * in_row_stride;

  int out_row_stride = n_cols;
  int out_channel_stride = n_rows * out_row_stride;
  int out_batch_stride = n_channels * out_channel_stride;

  // Perform the re-ordering
  // For every batch
  for (int n = 0; n < n_batches; n++)
  {
    const int8_t* const in_batch = in + n*in_batch_stride;
    int8_t* const out_batch = out + n*out_batch_stride;

    // For every row
    for (int i = 0; i < n_rows; i++)
    {
      const int8_t* const in_i = in_batch + i*in_row_stride;
      int8_t* const out_i = out_batch + i*out_row_stride;

      // For every column
      //assert(n_cols/16)
      int num8x16 = n_cols/16;
      int8x16x3_t intlv_rgb;
      for (int j = 0; j < num8x16; j++)
      {
        const int8_t* const in_j = in_i + j*16*in_col_stride;
        int8_t* const out_j = out_i + j*16;

        intlv_rgb=vld3q_s8(in_j);
        vst1q_s8(out_j, intlv_rgb.val[0]);
        vst1q_s8(out_j+out_channel_stride, intlv_rgb.val[1]);
        vst1q_s8(out_j+out_channel_stride*2, intlv_rgb.val[2]);


      }
    }
  }
}



// 8bit nchw_to_nhwc
inline void nchw_to_nhwc_s8(
    const int8_t* const in,
    int8_t* const out,
    const int n_batches,
    const int n_channels,
    const int n_rows,
    const int n_cols
) {

  int in_row_stride =  n_cols;
  int in_channel_stride = n_rows * in_row_stride;
  int in_batch_stride = n_channels * in_channel_stride;

  int out_col_stride = n_channels;
  int out_row_stride = n_cols * out_col_stride;
  int out_batch_stride = n_rows * out_row_stride;

  // Perform the re-ordering
  for (int n = 0; n < n_batches; n++)
  {
    const int8_t* const in_batch = in + n*in_batch_stride;
    int8_t* const out_batch = out + n*out_batch_stride;

    for (int i = 0; i < n_rows; i++)
    {
      const int8_t* const in_row = in_batch + i*in_row_stride;
      int8_t* const out_row = out_batch + i*out_row_stride;

      for (int j = 0; j < n_cols; j++)
      {
        const int8_t* const in_col = in_row + j;
        int8_t* const out_col = out_row + j*out_col_stride;

        for (int c = 0; c < n_channels; c++)
        {
          const int8_t* const in_channel = in_col + c*in_channel_stride;
          out_col[c] = *(in_channel);
        }
      }
    }
  }
}

/*
// 8bit nchw_to_nhwc
inline void nchw_to_nhwc_s8(
  const int8_t* const in,
  int8_t* const out,
  const int n_batches,
  const int n_channels,
  const int n_rows,
  const int n_cols
)
{

  int in_row_stride =  n_cols;
  int in_channel_stride = n_rows * in_row_stride;
  int in_batch_stride = n_channels * in_channel_stride;

  int out_col_stride = n_channels;
  int out_row_stride = n_cols * out_col_stride;
  int out_batch_stride = n_rows * out_row_stride;

  // Perform the re-ordering
  for (int n = 0; n < n_batches; n++)
  {
    const int8_t* const in_batch = in + n*in_batch_stride;
    int8_t* const out_batch = out + n*out_batch_stride;

    for (int i = 0; i < n_rows; i++)
    {
      const int8_t* const in_row = in_batch + i*in_row_stride;
      int8_t* const out_row = out_batch + i*out_row_stride;

      int j = 0, j_remaining = n_cols;

      //16x16 transpose�� ã������.
      //�ϴ� uint16���� ��ȯ. 8x8�� ����. �ٽ� uint8�� ��ȯ.
      for (; j_remaining >= 8; j += 8, j_remaining -= 8)
      {
        int c = 0, c_remaining = n_channels;
        for (; c_remaining >= 8; c += 8, c_remaining -= 8)
        {
          // Read 16 channels worth of 16 columns, then zip to produce 16 columns
          // worth of 16 channels.
          int8x8_t channel_pixels[8];

          int8_t* in_addr0 = in_row + (c + 0)*in_channel_stride + j; // +=in_channel_stride
          int8_t* out_addr0 = out_row + (j + 0)*out_col_stride + c; // += out_col_Stride

          asm volatile(
            "vld1.32 {x0}, [%[in_channel_stride]]!\n"
            "vld1.32 {x1}, [%[in_addr0]]\n"
            "vld1.8 {q0}, [x1]!\n"  // 16�� 8bit load
            "add x1,x1,x0\n"
            "vshll.8 q4,d0,#8\n"
            "vshll.8 q5,d1,#8\n"
            "vld1.8 {q0}, [x1]!\n"  // 16�� 8bit load
            "add x1,x1,x0\n"
            "vshll.8 q6,d0,#8\n"
            "vshll.8 q7,d1,#8\n"
            "vld1.8 {q0}, [x1]!\n"  // 16�� 8bit load
            "add x1,x1,x0\n"
            "vshll.8 q8,d0,#8\n"
            "vshll.8 q9,d1,#8\n"
            "vld1.8 {q0}, [x1]!\n"  // 16�� 8bit load
            "vshll.8 q10,d0,#8\n"
            "vshll.8 q11,d1,#8\n"

            // swap antidiagnoal elem within 2x2 blocks
            "vtrn.16 q8,q9\n"
            "vtrn.16 q10,q11\n"
            "vtrn.16 q4,q5\n"
            "vtrn.16 q6,q7\n"

            // swap antidiagnoal 2x2 block within 4x4 blocks
            "vtrn.32 q8,q10\n"
            "vtrn.32 q9,q11\n"
            "vtrn.32 q4,q6\n"
            "vtrn.32 q5,q7\n"

            // swap antidiagonal 4x4 blocks within 8x8 matrix
            "vswp d9,d16\n"
            "vswp d11,d18\n"
            "vswp d13,d20\n"
            "vswp d15,d22\n"

            //store

            :
            // Outputs.

            :
            // Inputs.
            [in_channel_stride] "r"(in_channel_stride), [in_addr0] "r"(in_addr0),
            [out_col_stride] "r"(out_col_stride), [out_addr0] "r"(out_addr0)
            :
            // Clobbers.
            "cc", "memory",
            // We use these NEON registers
            "x0", "x1",
            "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10",
            "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20",
            "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30",
            "v31");

          vst1q_s32(out_row + (j + 0)*out_col_stride + c, out_0.val[0]);
          vst1q_s32(out_row + (j + 1)*out_col_stride + c, out_0.val[1]);
          vst1q_s32(out_row + (j + 2)*out_col_stride + c, out_1.val[0]);
          vst1q_s32(out_row + (j + 3)*out_col_stride + c, out_1.val[1]);
        }
        for (; c_remaining; c++, c_remaining--)
        {
          for (int _j = 0; _j < 4; _j++)
          {
            const T* const in_col = in_row + j + _j;
            T* const out_col = out_row + (j + _j)*out_col_stride;
            const T* const in_channel = in_col + c*in_channel_stride;
            out_col[c] = *(in_channel);
          }
        }
      }
      for (; j_remaining >= 2; j += 2, j_remaining -= 2)
      {
        int c = 0, c_remaining = n_channels;
        for (; c_remaining >= 2; c += 2, c_remaining -= 2)
        {
          // Read 2 channels worth of 2 columns, then zip to produce 2 columns
          // worth of 2 channels.
          int32x2_t channel_pixels[2];
          channel_pixels[0] = vld1_s32(in_row + (c + 0)*in_channel_stride + j);
          channel_pixels[1] = vld1_s32(in_row + (c + 1)*in_channel_stride + j);

          const auto output = vzip_s32(channel_pixels[0], channel_pixels[1]);

          vst1_s32(out_row + (j + 0)*out_col_stride + c, output.val[0]);
          vst1_s32(out_row + (j + 1)*out_col_stride + c, output.val[1]);
        }
        for (; c_remaining; c++, c_remaining--)
        {
          for (int _j = 0; _j < 2; _j++)
          {
            const T* const in_col = in_row + j + _j;
            T* const out_col = out_row + (j + _j)*out_col_stride;
            const T* const in_channel = in_col + c*in_channel_stride;
            out_col[c] = *(in_channel);
          }
        }
      }

      for (; j_remaining; j++, j_remaining--)
      {
        const T* const in_col = in_row + j;
        T* const out_col = out_row + j*out_col_stride;

        for (int c = 0; c < n_channels; c++)
        {
          const T* const in_channel = in_col + c*in_channel_stride;
          out_col[c] = *(in_channel);
        }
      }
    }
  }
}
*/


// from google gemmlowp
// lhr 4x16n, rhr 16nx4. cell size = 4x(16xN). depth = 16n
void mm_s8_4x16n(const int8_t *lhs_ptr, const int8_t *rhs_ptr, int32_t *accum_ptr, int depth)
{
  int32x4_t acc[4][4];
  for (int i = 0; i < 4; i++)
  {
    for (int j = 0; j < 4; j++)
    {
      acc[i][j] = vdupq_n_s32(0);
    }
  }
  for (int d = 0; d < depth; d += 16)
  {
    int8x16_t lhs[4];
    for (int i = 0; i < 4; i++)
    {
      lhs[i] = vld1q_s8(lhs_ptr + 16 * i);
    }
    int8x16_t rhs[4];
    for (int i = 0; i < 4; i++)
    {
      rhs[i] = vld1q_s8(rhs_ptr + 16 * i);
    }
    for (int i = 0; i < 4; i++)
    {
      for (int j = 0; j < 4; j++)
      {
        int16x8_t local_acc =
            vmull_s8(vget_low_s8(lhs[i]), vget_low_s8(rhs[j]));
        local_acc =
            vmlal_s8(local_acc, vget_high_s8(lhs[i]), vget_high_s8(rhs[j]));
        acc[i][j] = vpadalq_s16(acc[i][j], local_acc);
      }
    }
    lhs_ptr += 64;
    rhs_ptr += 16 * 4;
  }
  for (int i = 0; i < 4; i++)
  {
    int32x4_t acc_2x_0 = vpaddq_s32(acc[0][i], acc[1][i]);
    int32x4_t acc_2x_1 = vpaddq_s32(acc[2][i], acc[3][i]);
    int32x4_t acc_4x = vpaddq_s32(acc_2x_0, acc_2x_1);
    int32x4_t dst_val = vld1q_s32(accum_ptr + 4 * i);
    dst_val = vaddq_s32(dst_val, acc_4x);
    vst1q_s32(accum_ptr + 4 * i, dst_val);
  }
}


int cpuvtaconvolution_f32(float* input, float inputScale, int32_t inputOffset, float *kernel, float kernelScale, int32_t kernelOffset,
                          float *bias, float biasScale, int32_t biasOffset, float *output, float outputScale, int32_t outputOffset, dim_t N, dim_t H, dim_t W, dim_t C, dim_t KN, dim_t KH, dim_t KW, int pad_size,
                          int stride_size, size_t group, size_t dilation, bool doRelu, bool doBias, int out_h, int out_w) {
  dim_t inCperG = C / group;
  dim_t outCperG = KN / group;
  float matMulScale = inputScale * kernelScale;
  for (dim_t n = 0; n < N; n++) {
    // For each group of input channels:
    for (dim_t g = 0; g < group; g++) {

      // For each output channel in the group:
      for (dim_t d = g * outCperG; d < (g + 1) * outCperG; d++) {

        // For each convolution 'jump' in the input tensor:
        ssize_t x = -ssize_t(pad_size);
        for (dim_t ax = 0; ax < out_h; x += stride_size, ax++) {
          ssize_t y = -ssize_t(pad_size);
          for (dim_t ay = 0; ay < out_w; y += stride_size, ay++) {

            // For each element in the convolution-filter:
            float sum = 0.0;
            for (dim_t fx = 0; fx < KH; fx++) {
              for (dim_t fy = 0; fy < KW; fy++) {
                int64_t ox = x + fx * dilation;
                int64_t oy = y + fy * dilation;

                // Ignore index access below zero (this is due to padding).
                if (ox < 0 || oy < 0 || ox >= ssize_t(H) ||
                    oy >= int64_t(W)) {
                  continue;
                }
                for (dim_t fd = 0; fd < inCperG; fd++) {

                  float F = *(kernel+d*KH*KW*inCperG + fx*KW*inCperG + fy*inCperG + fd);
                  float I = *(input+n*H*W*C + ox*W*C + oy*C+ (g*inCperG +fd));
                  // We represent the element multiplication with offset as
                  // (value - offset).
                  sum += (F - kernelOffset) * (I - inputOffset);
                }
              }
            }

            float B = *(bias+d);

            // Add the bias.
            sum += B;

            if(doRelu && sum<0.0){
              sum = 0.0;
            }

            *(output + n*out_h*out_w*KN + ax*out_w*KN + ay*KN + d) = sum;
          } // W
        }   // H
      }     // C
    }       // G
  }         // NS
  return 0;

}
void fwdConvolution_nhwc_acl_f32(float32_t *input, float32_t *kernel, float32_t *bias, float32_t *output, 
                      float inS, float kS, float bS, float outS,
                      int32_t inOffset, int32_t filterOffset, int32_t biasOffset, int32_t outOffset,
                      dim_t N, dim_t C, dim_t H, dim_t W, dim_t KN, dim_t KH, dim_t KW, int pad_size, int stride_size, size_t group,
                      size_t dilation, bool doRelu, bool doBias, 
				 int out_h, int out_w);


int convolutionFloat(int8_t* input, int8_t *kernel, int8_t *bias, int8_t *output, dim_t N, dim_t H, dim_t W, dim_t C, dim_t KN, dim_t KH, dim_t KW, int pad_size,
                     int stride_size, size_t group, size_t dilation, bool doRelu, bool doBias, int out_h, int out_w) {
  float inputScale = 0.0;
  int32_t inputOffset = 0;
  float kernelScale = 0.0;
  int32_t kernelOffset = 0;
  float biasScale = 0.0;
  int32_t biasOffset = 0;
  float outputScale = 0.0;
  int32_t outputOffset = 0;

  
  nonvtaconvolution_f32((float *)input, inputScale, inputOffset, kernel, kernelScale, kernelOffset,
                        (float *)bias, biasScale, biasOffset, (float *)output, outputScale,
                        outputOffset, N, H, W, C, KN, KH, KW, pad_size,
                            stride_size, group, dilation, doRelu, doBias, out_h, out_w);
  
  /*
  cpuvtaconvolution_f32((float *)input, inputScale, inputOffset, (float *)kernel, kernelScale, kernelOffset,
                        (float *)bias, biasScale, biasOffset, (float *)output, outputScale,
                        outputOffset, N, H, W, C, KN, KH, KW, pad_size,
                            stride_size, group, dilation, doRelu, doBias, out_h, out_w);
  */
  /*
  fwdConvolution_nhwc_acl_f32((float *)input, (float *)kernel, 
			      (float *)bias, (float *)output, inputScale, kernelScale, biasScale, outputScale, 0,0,0,0,
			      N, C,H, W, KN, KH, KW, pad_size,
                            stride_size, group, dilation, doRelu, doBias, out_h, out_w);
  */

  return 0;
}


// not all row
void mm_matrix_multiply_s8_v3(int8_t *lhs_ptr, int8_t *rhs_ptr,
                              int32_t *accum_ptr, int width_a, int col_b,
                              size_t stride_b, size_t stride_c,
                              int32_t filterOffset) {

  // block = 1x16 * 16*8

  // TODO. temp
  // check overflow
  // upscale to s32 if s16 datatype overflow because of filterOffset
  assert(filterOffset < 32700);
  assert(filterOffset > -32700);

  const int16x4_t vfilterOff = vdup_n_s16(filterOffset);

    int8_t *a_ptr = lhs_ptr;
  int8_t *b_ptr = rhs_ptr + col_b * stride_b;

    int left = width_a % 16;

    int32x4_t acc[4][4];
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
        acc[i][j] = vdupq_n_s32(0);
      }
    }
    // 16개 단위만 먼저 처리
  for (int d = 0; d < width_a - left; d += 16) {
      int8x16_t lhs[4];
    for (int i = 0; i < 4; i++) {
        lhs[i] = vld1q_s8(a_ptr + width_a * i);
      }
      int8x16_t rhs[4];
    for (int i = 0; i < 4; i++) {
        rhs[i] = vld1q_s8(b_ptr + stride_b * i);
      }
    for (int i = 0; i < 4; i++) {
      for (int j = 0; j < 4; j++) {
          int16x8_t local_acc =
              vmull_s8(vget_low_s8(lhs[i]), vget_low_s8(rhs[j]));
          local_acc =
              vmlal_s8(local_acc, vget_high_s8(lhs[i]), vget_high_s8(rhs[j]));
          acc[i][j] = vpadalq_s16(acc[i][j], local_acc);
        }
      }
      a_ptr += 16;
      b_ptr += 16;
    }

    // 16보다 작은거 처리
    {
      int8x16_t lhs[4];
      for (int i = 0; i < 4; i++)
      {
        lhs[i] = vld1q_s8(a_ptr + width_a * i);
      }
      int8x16_t rhs[4];
      for (int i = 0; i < 4; i++)
      {
        rhs[i] = vld1q_s8(b_ptr + stride_b * i);
      }

      int idx=0;
      if(left >= 8) {
        for (int i = 0; i < 4; i++)
        {
          for (int j = 0; j < 4; j++)
          {
            int16x8_t local_acc =
                vmull_s8(vget_low_s8(lhs[i]), vget_low_s8(rhs[j]));
                    //local_acc = vmlal_s8(local_acc, vget_high_s8(lhs[i]),  vget_high_s8(rhs[j])); 
            acc[i][j] = vpadalq_s16(acc[i][j], local_acc);
          }
        }
        left -= 8;
        idx+=8;
      }

      if(left>=4) {
        for (int i = 0; i < 4; i++)
        {
          for (int j = 0; j < 4; j++)
          {
            int16x8_t local_acc;
            if(idx==0) {
                              local_acc =vmull_s8(vget_low_s8(lhs[i]),   vget_low_s8(rhs[j]));
            }
            else if(idx==8) {
                          local_acc =   vmull_s8(vget_high_s8(lhs[i]),  vget_high_s8(rhs[j]));
            }
            acc[i][j] = vaddq_s32(acc[i][j], vmovl_s16(vget_low_s16(local_acc)));
          }
        }

        idx+=4;
        left-=4;
      }


          if(left>0) {

               // printf("4x%d => a:%p(+%d), b:%p(+%d)\n", left,a_ptr, a_ptr -   lhs_ptr, b_ptr, b_ptr - rhs_ptr); 
    for (int i = 0; i < 4; i++)
    {
                  for (int j = 0; j < 4; j++)
                  {
                    int32x4_t local_acc;
                    if (idx == 0)
                    {
                        local_acc =
                            vmull_s16(vget_low_s16(vmovl_s8(vget_low_s8(lhs[i]))),     vget_low_s16(vmovl_s8(vget_low_s8(rhs[j]))));
                    }
                    else if (idx == 8)
                    {
                        local_acc =
                            vmull_s16(vget_low_s16(vmovl_s8(vget_high_s8(lhs[i]))),     vget_low_s16(vmovl_s8(vget_high_s8(rhs[j]))));
                    }
                    else if (idx == 12)
                    {
                        local_acc =
                            vmull_s16(vget_high_s16(vmovl_s8(vget_high_s8(lhs[i]))),     vget_high_s16(vmovl_s8(vget_high_s8(rhs[j]))));
                    }

                    if(left==1) {
                      local_acc = vsetq_lane_s32(0, local_acc,1);
                      local_acc = vsetq_lane_s32(0, local_acc,2);
                      local_acc = vsetq_lane_s32(0, local_acc,3);
                    }
                    else if(left==2) {
                      local_acc = vsetq_lane_s32(0, local_acc,2);
                      local_acc = vsetq_lane_s32(0, local_acc,3);
                    }
                    else if(left==3) {
                      local_acc = vsetq_lane_s32(0, local_acc,3);
                    }

                    acc[i][j] = vaddq_s32(acc[i][j], local_acc);
                  }
              }
          }


      }
  

  for (int i = 0; i < 4; i++) {
      int32x4_t acc_2x_0 = vpaddq_s32(acc[0][i], acc[1][i]);
      int32x4_t acc_2x_1 = vpaddq_s32(acc[2][i], acc[3][i]);
      int32x4_t acc_4x = vpaddq_s32(acc_2x_0, acc_2x_1);
    // int32x4_t dst_val = vld1q_s32(accum_ptr + 4 * i);
    // dst_val = vaddq_s32(dst_val, acc_4x);
    // vst1q_s32(accum_ptr +  nblock * 4 + stride_c * i, dst_val);
    vst1q_s32(accum_ptr + col_b + stride_c * i, acc_4x);
  }
}


// (16xK)*(K*8n). width_a = K = depth,  width_b =  * elemsize(1).
// best performance if K = 16k
// lhs: row-major, rhs: colomn-major
// 4*(KW*KH) x (KW*KH)*(out_w*out_h)
void mm_matrix_multiply_s8_v2(int8_t *lhs_ptr, int8_t *rhs_ptr, int32_t *accum_ptr, int width_a, int width_b, size_t stride_b, size_t stride_c, int32_t filterOffset)
{

  // block = 1x16 * 16*8

  //TODO. temp
  // check overflow
  // upscale to s32 if s16 datatype overflow because of filterOffset
  assert(filterOffset < 32700 );
  assert(filterOffset > -32700);

  const int16x4_t vfilterOff = vdup_n_s16(filterOffset);





  for (int nblock = 0; nblock < width_b / 4; nblock++)
  {

    int8_t *a_ptr = lhs_ptr;
    int8_t *b_ptr = rhs_ptr + nblock * 4 * stride_b;

    int left = width_a % 16;

    int32x4_t acc[4][4];
    for (int i = 0; i < 4; i++)
    {
      for (int j = 0; j < 4; j++)
      {
        acc[i][j] = vdupq_n_s32(0);
      }
    }
    // 16개 단위만 먼저 처리
    for (int d = 0; d < width_a - left ; d += 16)
    {
      int8x16_t lhs[4];
      for (int i = 0; i < 4; i++)
      {
        lhs[i] = vld1q_s8(a_ptr + width_a * i);
      }
      int8x16_t rhs[4];
      for (int i = 0; i < 4; i++)
      {
        rhs[i] = vld1q_s8(b_ptr + stride_b * i);
      }
      for (int i = 0; i < 4; i++)
      {
        for (int j = 0; j < 4; j++)
        {
          int16x8_t local_acc =
              vmull_s8(vget_low_s8(lhs[i]), vget_low_s8(rhs[j]));
          local_acc =
              vmlal_s8(local_acc, vget_high_s8(lhs[i]), vget_high_s8(rhs[j]));
          acc[i][j] = vpadalq_s16(acc[i][j], local_acc);
        }
      }
      a_ptr += 16;
      b_ptr += 16;
    }

    // 16보다 작은거 처리
    {
      int8x16_t lhs[4];
      for (int i = 0; i < 4; i++)
      {
        lhs[i] = vld1q_s8(a_ptr + width_a * i);
      }
      int8x16_t rhs[4];
      for (int i = 0; i < 4; i++)
      {
        rhs[i] = vld1q_s8(b_ptr + stride_b * i);
      }

      int idx=0;
      if(left >= 8) {
        for (int i = 0; i < 4; i++)
        {
          for (int j = 0; j < 4; j++)
          {
            int16x8_t local_acc =
                vmull_s8(vget_low_s8(lhs[i]), vget_low_s8(rhs[j]));
            //local_acc =
            //    vmlal_s8(local_acc, vget_high_s8(lhs[i]), vget_high_s8(rhs[j]));
            acc[i][j] = vpadalq_s16(acc[i][j], local_acc);
          }
        }
        left -= 8;
        idx+=8;
      }

      if(left>=4) {
        for (int i = 0; i < 4; i++)
        {
          for (int j = 0; j < 4; j++)
          {
            int16x8_t local_acc;
            if(idx==0) {
              local_acc =
                  vmull_s8(vget_low_s8(lhs[i]), vget_low_s8(rhs[j]));
            }
            else if(idx==8) {
              local_acc =
                  vmull_s8(vget_high_s8(lhs[i]), vget_high_s8(rhs[j]));
            }

            acc[i][j] = vaddq_s32(acc[i][j], vmovl_s16(vget_low_s16(local_acc)));
          }
        }

        idx+=4;
        left-=4;
      }

    }

    for (int i = 0; i < 4; i++)
    {
      int32x4_t acc_2x_0 = vpaddq_s32(acc[0][i], acc[1][i]);
      int32x4_t acc_2x_1 = vpaddq_s32(acc[2][i], acc[3][i]);
      int32x4_t acc_4x = vpaddq_s32(acc_2x_0, acc_2x_1);
      int32x4_t dst_val = vld1q_s32(accum_ptr + 4 * i);
      dst_val = vaddq_s32(dst_val, acc_4x);
      vst1q_s32(accum_ptr +  nblock * 4 + stride_c * i, dst_val);
    }

  } // n_block

}


// (1xK)*(K*8n). width_a = K = depth,  width_b =  * elemsize(1).
// best performance if K = 16k
// lhs: row-major, rhs: colomn-major
void vector_matrix_multiply_s8(int8_t *lhs_ptr, int8_t *rhs_ptr, int32_t *accum_ptr, int width_a, int width_b, size_t stride_b, int32_t filterOffset)
{

  // block = 1x16 * 16*8

  //TODO. temp
  // check overflow
  // upscale to s32 if s16 datatype overflow because of filterOffset
  assert(filterOffset < 32700 );
  assert(filterOffset > -32700);

  const int16x4_t vfilterOff = vdup_n_s16(filterOffset);

  const int wb = width_b / 8;

  #pragma omp parallel for num_threads(4)
  for (int nblock = 0; nblock < wb; nblock++)
  {
      
     // printf("%d) => %d /  ",nblock,sched_getcpu());
    // 8 width
    int32x4_t c0[8];

    for (int k = 0; k < 8; k++)
      c0[k] = vdupq_n_s32(0);

    int8_t *a_ptr = lhs_ptr;
    int8_t *b_ptr = rhs_ptr + nblock * 8 * stride_b;
    auto vec_a_end_addr = a_ptr + width_a;

    if( vec_a_end_addr - a_ptr >= 16)
    {
      // This for loop performs 8 accumulations
      // cell layout: 1x16 * 16*8 = 1x8
      for (; a_ptr <= (vec_a_end_addr - 16);)
      {
        const int8x16_t a00_s8 = vld1q_s8(a_ptr);
        const int8x16_t b00_s8 = vld1q_s8(b_ptr + 0 * stride_b);
        const int8x16_t b10_s8 = vld1q_s8(b_ptr + 1 * stride_b);
        const int8x16_t b20_s8 = vld1q_s8(b_ptr + 2 * stride_b);
        const int8x16_t b30_s8 = vld1q_s8(b_ptr + 3 * stride_b);
        const int8x16_t b40_s8 = vld1q_s8(b_ptr + 4 * stride_b);
        const int8x16_t b50_s8 = vld1q_s8(b_ptr + 5 * stride_b);
        const int8x16_t b60_s8 = vld1q_s8(b_ptr + 6 * stride_b);
        const int8x16_t b70_s8 = vld1q_s8(b_ptr + 7 * stride_b);


        // Convert a00_s8 to int16_t and get the lower part
        const int16x4x4_t a00_s16 =
            {
                {vsub_s16(vget_low_s16(vmovl_s8(vget_low_s8(a00_s8))), vfilterOff),
                    vsub_s16(vget_high_s16(vmovl_s8(vget_low_s8(a00_s8))), vfilterOff),
                    vsub_s16(vget_low_s16(vmovl_s8(vget_high_s8(a00_s8))), vfilterOff),
                    vsub_s16(vget_high_s16(vmovl_s8(vget_high_s8(a00_s8))), vfilterOff)}};

        const int16x4x4_t b00_s16 =
            {
                {vget_low_s16(vmovl_s8(vget_low_s8(b00_s8))),
                    vget_high_s16(vmovl_s8(vget_low_s8(b00_s8))),
                    vget_low_s16(vmovl_s8(vget_high_s8(b00_s8))),
                    vget_high_s16(vmovl_s8(vget_high_s8(b00_s8)))}};

        const int16x4x4_t b10_s16 =
            {
                {vget_low_s16(vmovl_s8(vget_low_s8(b10_s8))),
                    vget_high_s16(vmovl_s8(vget_low_s8(b10_s8))),
                    vget_low_s16(vmovl_s8(vget_high_s8(b10_s8))),
                    vget_high_s16(vmovl_s8(vget_high_s8(b10_s8)))}};

        const int16x4x4_t b20_s16 =
            {
                {vget_low_s16(vmovl_s8(vget_low_s8(b20_s8))),
                    vget_high_s16(vmovl_s8(vget_low_s8(b20_s8))),
                    vget_low_s16(vmovl_s8(vget_high_s8(b20_s8))),
                    vget_high_s16(vmovl_s8(vget_high_s8(b20_s8)))}};

        const int16x4x4_t b30_s16 =
            {
                {vget_low_s16(vmovl_s8(vget_low_s8(b30_s8))),
                    vget_high_s16(vmovl_s8(vget_low_s8(b30_s8))),
                    vget_low_s16(vmovl_s8(vget_high_s8(b30_s8))),
                    vget_high_s16(vmovl_s8(vget_high_s8(b30_s8)))}};

        const int16x4x4_t b40_s16 =
            {
                {vget_low_s16(vmovl_s8(vget_low_s8(b40_s8))),
                    vget_high_s16(vmovl_s8(vget_low_s8(b40_s8))),
                    vget_low_s16(vmovl_s8(vget_high_s8(b40_s8))),
                    vget_high_s16(vmovl_s8(vget_high_s8(b40_s8)))}};

        const int16x4x4_t b50_s16 =
            {
                {vget_low_s16(vmovl_s8(vget_low_s8(b50_s8))),
                    vget_high_s16(vmovl_s8(vget_low_s8(b50_s8))),
                    vget_low_s16(vmovl_s8(vget_high_s8(b50_s8))),
                    vget_high_s16(vmovl_s8(vget_high_s8(b50_s8)))}};

        const int16x4x4_t b60_s16 =
            {
                {vget_low_s16(vmovl_s8(vget_low_s8(b60_s8))),
                    vget_high_s16(vmovl_s8(vget_low_s8(b60_s8))),
                    vget_low_s16(vmovl_s8(vget_high_s8(b60_s8))),
                    vget_high_s16(vmovl_s8(vget_high_s8(b60_s8)))}};

        const int16x4x4_t b70_s16 =
            {
                {vget_low_s16(vmovl_s8(vget_low_s8(b70_s8))),
                    vget_high_s16(vmovl_s8(vget_low_s8(b70_s8))),
                    vget_low_s16(vmovl_s8(vget_high_s8(b70_s8))),
                    vget_high_s16(vmovl_s8(vget_high_s8(b70_s8)))}};

        // Accumulate 0:
        c0[0] = vmlal_s16(c0[0], b00_s16.val[0], a00_s16.val[0]);
        c0[0] = vmlal_s16(c0[0], b00_s16.val[1], a00_s16.val[1]);
        c0[0] = vmlal_s16(c0[0], b00_s16.val[2], a00_s16.val[2]);
        c0[0] = vmlal_s16(c0[0], b00_s16.val[3], a00_s16.val[3]);

        // Accumulate 1:
        c0[1] = vmlal_s16(c0[1], b10_s16.val[0], a00_s16.val[0]);
        c0[1] = vmlal_s16(c0[1], b10_s16.val[1], a00_s16.val[1]);
        c0[1] = vmlal_s16(c0[1], b10_s16.val[2], a00_s16.val[2]);
        c0[1] = vmlal_s16(c0[1], b10_s16.val[3], a00_s16.val[3]);

        // Accumulate 2:
        c0[2] = vmlal_s16(c0[2], b20_s16.val[0], a00_s16.val[0]);
        c0[2] = vmlal_s16(c0[2], b20_s16.val[1], a00_s16.val[1]);
        c0[2] = vmlal_s16(c0[2], b20_s16.val[2], a00_s16.val[2]);
        c0[2] = vmlal_s16(c0[2], b20_s16.val[3], a00_s16.val[3]);

        // Accumulate 3:
        c0[3] = vmlal_s16(c0[3], b30_s16.val[0], a00_s16.val[0]);
        c0[3] = vmlal_s16(c0[3], b30_s16.val[1], a00_s16.val[1]);
        c0[3] = vmlal_s16(c0[3], b30_s16.val[2], a00_s16.val[2]);
        c0[3] = vmlal_s16(c0[3], b30_s16.val[3], a00_s16.val[3]);

        // Accumulate 4:
        c0[4] = vmlal_s16(c0[4], b40_s16.val[0], a00_s16.val[0]);
        c0[4] = vmlal_s16(c0[4], b40_s16.val[1], a00_s16.val[1]);
        c0[4] = vmlal_s16(c0[4], b40_s16.val[2], a00_s16.val[2]);
        c0[4] = vmlal_s16(c0[4], b40_s16.val[3], a00_s16.val[3]);

        // Accumulate 5:
        c0[5] = vmlal_s16(c0[5], b50_s16.val[0], a00_s16.val[0]);
        c0[5] = vmlal_s16(c0[5], b50_s16.val[1], a00_s16.val[1]);
        c0[5] = vmlal_s16(c0[5], b50_s16.val[2], a00_s16.val[2]);
        c0[5] = vmlal_s16(c0[5], b50_s16.val[3], a00_s16.val[3]);

        // Accumulate 6:
        c0[6] = vmlal_s16(c0[6], b60_s16.val[0], a00_s16.val[0]);
        c0[6] = vmlal_s16(c0[6], b60_s16.val[1], a00_s16.val[1]);
        c0[6] = vmlal_s16(c0[6], b60_s16.val[2], a00_s16.val[2]);
        c0[6] = vmlal_s16(c0[6], b60_s16.val[3], a00_s16.val[3]);

        // Accumulate 7:
        c0[7] = vmlal_s16(c0[7], b70_s16.val[0], a00_s16.val[0]);
        c0[7] = vmlal_s16(c0[7], b70_s16.val[1], a00_s16.val[1]);
        c0[7] = vmlal_s16(c0[7], b70_s16.val[2], a00_s16.val[2]);
        c0[7] = vmlal_s16(c0[7], b70_s16.val[3], a00_s16.val[3]);

        a_ptr += 16;
        b_ptr += 16;

#if DEBUG_INFO
        if(nblock==0) {

    int32_t sum_temp[4];
    vst1q_s32(sum_temp, c0[0]);

    printf("step:%d - ", a_ptr - lhs_ptr);
    for(int k=0;k<4;k++)
    {
      printf(" %d ", sum_temp[k]);
    }
    printf("\n");

  }
#endif

      }
    }

    // This for loop performs the left-over accumulations
    // left elem is less than 16
    {

      const int8x16_t a00_s8 = vld1q_s8(a_ptr);
      const int8x16_t b00_s8 = vld1q_s8(b_ptr + 0 * stride_b);
      const int8x16_t b10_s8 = vld1q_s8(b_ptr + 1 * stride_b);
      const int8x16_t b20_s8 = vld1q_s8(b_ptr + 2 * stride_b);
      const int8x16_t b30_s8 = vld1q_s8(b_ptr + 3 * stride_b);
      const int8x16_t b40_s8 = vld1q_s8(b_ptr + 4 * stride_b);
      const int8x16_t b50_s8 = vld1q_s8(b_ptr + 5 * stride_b);
      const int8x16_t b60_s8 = vld1q_s8(b_ptr + 6 * stride_b);
      const int8x16_t b70_s8 = vld1q_s8(b_ptr + 7 * stride_b);

      // Convert a00_s8 to int16_t and get the lower part
      const int16x4x4_t a00_s16 =
          {
              {vsub_s16(vget_low_s16(vmovl_s8(vget_low_s8(a00_s8))), vfilterOff),
                  vsub_s16(vget_high_s16(vmovl_s8(vget_low_s8(a00_s8))), vfilterOff),
                  vsub_s16(vget_low_s16(vmovl_s8(vget_high_s8(a00_s8))), vfilterOff),
                  vsub_s16(vget_high_s16(vmovl_s8(vget_high_s8(a00_s8))), vfilterOff)}};

      const int16x4x4_t b00_s16 =
          {
              {vget_low_s16(vmovl_s8(vget_low_s8(b00_s8))),
                  vget_high_s16(vmovl_s8(vget_low_s8(b00_s8))),
                  vget_low_s16(vmovl_s8(vget_high_s8(b00_s8))),
                  vget_high_s16(vmovl_s8(vget_high_s8(b00_s8)))}};

      const int16x4x4_t b10_s16 =
          {
              {vget_low_s16(vmovl_s8(vget_low_s8(b10_s8))),
                  vget_high_s16(vmovl_s8(vget_low_s8(b10_s8))),
                  vget_low_s16(vmovl_s8(vget_high_s8(b10_s8))),
                  vget_high_s16(vmovl_s8(vget_high_s8(b10_s8)))}};

      const int16x4x4_t b20_s16 =
          {
              {vget_low_s16(vmovl_s8(vget_low_s8(b20_s8))),
                  vget_high_s16(vmovl_s8(vget_low_s8(b20_s8))),
                  vget_low_s16(vmovl_s8(vget_high_s8(b20_s8))),
                  vget_high_s16(vmovl_s8(vget_high_s8(b20_s8)))}};

      const int16x4x4_t b30_s16 =
          {
              {vget_low_s16(vmovl_s8(vget_low_s8(b30_s8))),
                  vget_high_s16(vmovl_s8(vget_low_s8(b30_s8))),
                  vget_low_s16(vmovl_s8(vget_high_s8(b30_s8))),
                  vget_high_s16(vmovl_s8(vget_high_s8(b30_s8)))}};

      const int16x4x4_t b40_s16 =
          {
              {vget_low_s16(vmovl_s8(vget_low_s8(b40_s8))),
                  vget_high_s16(vmovl_s8(vget_low_s8(b40_s8))),
                  vget_low_s16(vmovl_s8(vget_high_s8(b40_s8))),
                  vget_high_s16(vmovl_s8(vget_high_s8(b40_s8)))}};

      const int16x4x4_t b50_s16 =
          {
              {vget_low_s16(vmovl_s8(vget_low_s8(b50_s8))),
                  vget_high_s16(vmovl_s8(vget_low_s8(b50_s8))),
                  vget_low_s16(vmovl_s8(vget_high_s8(b50_s8))),
                  vget_high_s16(vmovl_s8(vget_high_s8(b50_s8)))}};

      const int16x4x4_t b60_s16 =
          {
              {vget_low_s16(vmovl_s8(vget_low_s8(b60_s8))),
                  vget_high_s16(vmovl_s8(vget_low_s8(b60_s8))),
                  vget_low_s16(vmovl_s8(vget_high_s8(b60_s8))),
                  vget_high_s16(vmovl_s8(vget_high_s8(b60_s8)))}};

      const int16x4x4_t b70_s16 =
          {
              {vget_low_s16(vmovl_s8(vget_low_s8(b70_s8))),
                  vget_high_s16(vmovl_s8(vget_low_s8(b70_s8))),
                  vget_low_s16(vmovl_s8(vget_high_s8(b70_s8))),
                  vget_high_s16(vmovl_s8(vget_high_s8(b70_s8)))}};

      int ii = 0; // index
      if(vec_a_end_addr - a_ptr  >= 8) {
        // Accumulate 0:

        c0[0] = vmlal_s16(c0[0], b00_s16.val[0+ii], a00_s16.val[0+ii]);
        c0[0] = vmlal_s16(c0[0], b00_s16.val[1+ii], a00_s16.val[1+ii]);

        // Accumulate 1:
        c0[1] = vmlal_s16(c0[1], b10_s16.val[0+ii], a00_s16.val[0+ii]);
        c0[1] = vmlal_s16(c0[1], b10_s16.val[1+ii], a00_s16.val[1+ii]);

        // Accumulate 2:
        c0[2] = vmlal_s16(c0[2], b20_s16.val[0+ii], a00_s16.val[0+ii]);
        c0[2] = vmlal_s16(c0[2], b20_s16.val[1+ii], a00_s16.val[1+ii]);

        // Accumulate 3:
        c0[3] = vmlal_s16(c0[3], b30_s16.val[0+ii], a00_s16.val[0+ii]);
        c0[3] = vmlal_s16(c0[3], b30_s16.val[1+ii], a00_s16.val[1+ii]);

        // Accumulate 4:
        c0[4] = vmlal_s16(c0[4], b40_s16.val[0+ii], a00_s16.val[0+ii]);
        c0[4] = vmlal_s16(c0[4], b40_s16.val[1+ii], a00_s16.val[1+ii]);

        // Accumulate 5:
        c0[5] = vmlal_s16(c0[5], b50_s16.val[0+ii], a00_s16.val[0+ii]);
        c0[5] = vmlal_s16(c0[5], b50_s16.val[1+ii], a00_s16.val[1+ii]);

        // Accumulate 6:
        c0[6] = vmlal_s16(c0[6], b60_s16.val[0+ii], a00_s16.val[0+ii]);
        c0[6] = vmlal_s16(c0[6], b60_s16.val[1+ii], a00_s16.val[1+ii]);

        // Accumulate 7:
        c0[7] = vmlal_s16(c0[7], b70_s16.val[0+ii], a00_s16.val[0+ii]);
        c0[7] = vmlal_s16(c0[7], b70_s16.val[1+ii], a00_s16.val[1+ii]);

        a_ptr+=8;
        ii+= (8/4);
      }

      if(vec_a_end_addr - a_ptr >= 4) {
        // Accumulate 0:
        c0[0] = vmlal_s16(c0[0], b00_s16.val[0+ii], a00_s16.val[0+ii]);

        // Accumulate 1:
        c0[1] = vmlal_s16(c0[1], b10_s16.val[0+ii], a00_s16.val[0+ii]);

        // Accumulate 2:
        c0[2] = vmlal_s16(c0[2], b20_s16.val[0+ii], a00_s16.val[0+ii]);

        // Accumulate 3:
        c0[3] = vmlal_s16(c0[3], b30_s16.val[0+ii], a00_s16.val[0+ii]);

        // Accumulate 4:
        c0[4] = vmlal_s16(c0[4], b40_s16.val[0+ii], a00_s16.val[0+ii]);

        // Accumulate 5:
        c0[5] = vmlal_s16(c0[5], b50_s16.val[0+ii], a00_s16.val[0+ii]);

        // Accumulate 6:
        c0[6] = vmlal_s16(c0[6], b60_s16.val[0+ii], a00_s16.val[0+ii]);

        // Accumulate 7:
        c0[7] = vmlal_s16(c0[7], b70_s16.val[0+ii], a00_s16.val[0+ii]);

        a_ptr+=4;
        ii+= (4/4);
      }

      int left = vec_a_end_addr - a_ptr;
      if(left > 0 )
      {
        int16_t l1_mask[4] = { 1 , 0 ,0 ,0};
        int16_t l2_mask[4] = { 1 , 1 ,0 ,0};
        int16_t l3_mask[4] = { 1 , 1 ,1 ,0};
        int16x4_t left_mask;

        if(left==1) left_mask = vld1_s16(l1_mask);
        else if(left==2) left_mask = vld1_s16(l2_mask);
        else left_mask = vld1_s16(l3_mask);

        // Accumulate 0:
        c0[0] = vmlal_s16(c0[0], b00_s16.val[0+ii], vmul_s16(left_mask, a00_s16.val[0+ii]));

        // Accumulate 1:
        c0[1] = vmlal_s16(c0[1], b10_s16.val[0+ii], vmul_s16(left_mask, a00_s16.val[0+ii]));

        // Accumulate 2:
        c0[2] = vmlal_s16(c0[2], b20_s16.val[0+ii], vmul_s16(left_mask, a00_s16.val[0+ii]));

        // Accumulate 3:
        c0[3] = vmlal_s16(c0[3], b30_s16.val[0+ii], vmul_s16(left_mask, a00_s16.val[0+ii]));

        // Accumulate 4:
        c0[4] = vmlal_s16(c0[4], b40_s16.val[0+ii], vmul_s16(left_mask, a00_s16.val[0+ii]));

        // Accumulate 5:
        c0[5] = vmlal_s16(c0[5], b50_s16.val[0+ii], vmul_s16(left_mask, a00_s16.val[0+ii]));

        // Accumulate 6:
        c0[6] = vmlal_s16(c0[6], b60_s16.val[0+ii], vmul_s16(left_mask, a00_s16.val[0+ii]));

        // Accumulate 7:
        c0[7] = vmlal_s16(c0[7], b70_s16.val[0+ii], vmul_s16(left_mask, a00_s16.val[0+ii]));


        a_ptr += left;
      }
      // b_ptr += stride_b;
    }


    for (int k = 0; k < 8; k++)
    {
      int32x2_t temp = vadd_s32(vget_high_s32(c0[k]), vget_low_s32(c0[k]));
      int32x2_t temp2 = vpadd_s32(temp, temp);
      int32_t sum1 = vget_lane_s32(temp2, 0);

      //vst1q_s32(accum_ptr + nblock * 8 + k, c0[k]);
      accum_ptr[nblock * 8  + k] = sum1;

    }
  }


}

// (16xK)*(K*8n). width_a = K = depth,  width_b =  * elemsize(1).
// best performance if K = 16k
// lhs: row-major, rhs: colomn-major
void mm_matrix_multiply_s8(int8_t *lhs_ptr, int8_t *rhs_ptr, int32_t *accum_ptr, int width_a, int width_b, size_t stride_b, int32_t filterOffset)
{

  // block = 1x16 * 16*8

  //TODO. temp
  // check overflow
  // upscale to s32 if s16 datatype overflow because of filterOffset
  assert(filterOffset < 32700 );
  assert(filterOffset > -32700);

  const int16x4_t vfilterOff = vdup_n_s16(filterOffset);
  const int k_row=8;  // 커널 갯수
  // lhs = (k_row) * 16
  // rhs = 16*8 (8개 width)

  for (int nblock = 0; nblock < width_b / 8; nblock++)
  {
    // 8 width
    int32x4_t c[k_row][8];

    int8x16_t a_s8[k_row] ;
    int16x4x4_t a_s16[k_row];

    for(int k =0;k<k_row;k++)
      for (int o = 0; o < 8; o++)
        c[k][o] = vdupq_n_s32(0);

    int8_t *a_ptr = lhs_ptr;
    int8_t *b_ptr = rhs_ptr + nblock * 8 * stride_b;
    auto vec_a_end_addr = a_ptr + width_a;

    if( vec_a_end_addr - a_ptr >= 16)
    {
      // This for loop performs 8 accumulations
      // cell layout: 1x16 * 16*8 = 1x8
      for (; a_ptr <= (vec_a_end_addr - 16);)
      {


        /*  const int8x16_t a40_s8 = vld1q_s8(a_ptr + 4* width_a);
          const int8x16_t a50_s8 = vld1q_s8(a_ptr + 5* width_a);
          const int8x16_t a60_s8 = vld1q_s8(a_ptr + 6* width_a);
          const int8x16_t a70_s8 = vld1q_s8(a_ptr + 7* width_a);
*/
        const int8x16_t b00_s8 = vld1q_s8(b_ptr + 0 * stride_b);
        const int8x16_t b10_s8 = vld1q_s8(b_ptr + 1 * stride_b);
        const int8x16_t b20_s8 = vld1q_s8(b_ptr + 2 * stride_b);
        const int8x16_t b30_s8 = vld1q_s8(b_ptr + 3 * stride_b);
        const int8x16_t b40_s8 = vld1q_s8(b_ptr + 4 * stride_b);
        const int8x16_t b50_s8 = vld1q_s8(b_ptr + 5 * stride_b);
        const int8x16_t b60_s8 = vld1q_s8(b_ptr + 6 * stride_b);
        const int8x16_t b70_s8 = vld1q_s8(b_ptr + 7 * stride_b);

        const int16x4x4_t b00_s16 =
            {
                {vget_low_s16(vmovl_s8(vget_low_s8(b00_s8))),
                    vget_high_s16(vmovl_s8(vget_low_s8(b00_s8))),
                    vget_low_s16(vmovl_s8(vget_high_s8(b00_s8))),
                    vget_high_s16(vmovl_s8(vget_high_s8(b00_s8)))}};

        const int16x4x4_t b10_s16 =
            {
                {vget_low_s16(vmovl_s8(vget_low_s8(b10_s8))),
                    vget_high_s16(vmovl_s8(vget_low_s8(b10_s8))),
                    vget_low_s16(vmovl_s8(vget_high_s8(b10_s8))),
                    vget_high_s16(vmovl_s8(vget_high_s8(b10_s8)))}};

        const int16x4x4_t b20_s16 =
            {
                {vget_low_s16(vmovl_s8(vget_low_s8(b20_s8))),
                    vget_high_s16(vmovl_s8(vget_low_s8(b20_s8))),
                    vget_low_s16(vmovl_s8(vget_high_s8(b20_s8))),
                    vget_high_s16(vmovl_s8(vget_high_s8(b20_s8)))}};

        const int16x4x4_t b30_s16 =
            {
                {vget_low_s16(vmovl_s8(vget_low_s8(b30_s8))),
                    vget_high_s16(vmovl_s8(vget_low_s8(b30_s8))),
                    vget_low_s16(vmovl_s8(vget_high_s8(b30_s8))),
                    vget_high_s16(vmovl_s8(vget_high_s8(b30_s8)))}};

        const int16x4x4_t b40_s16 =
            {
                {vget_low_s16(vmovl_s8(vget_low_s8(b40_s8))),
                    vget_high_s16(vmovl_s8(vget_low_s8(b40_s8))),
                    vget_low_s16(vmovl_s8(vget_high_s8(b40_s8))),
                    vget_high_s16(vmovl_s8(vget_high_s8(b40_s8)))}};

        const int16x4x4_t b50_s16 =
            {
                {vget_low_s16(vmovl_s8(vget_low_s8(b50_s8))),
                    vget_high_s16(vmovl_s8(vget_low_s8(b50_s8))),
                    vget_low_s16(vmovl_s8(vget_high_s8(b50_s8))),
                    vget_high_s16(vmovl_s8(vget_high_s8(b50_s8)))}};

        const int16x4x4_t b60_s16 =
            {
                {vget_low_s16(vmovl_s8(vget_low_s8(b60_s8))),
                    vget_high_s16(vmovl_s8(vget_low_s8(b60_s8))),
                    vget_low_s16(vmovl_s8(vget_high_s8(b60_s8))),
                    vget_high_s16(vmovl_s8(vget_high_s8(b60_s8)))}};

        const int16x4x4_t b70_s16 =
            {
                {vget_low_s16(vmovl_s8(vget_low_s8(b70_s8))),
                    vget_high_s16(vmovl_s8(vget_low_s8(b70_s8))),
                    vget_low_s16(vmovl_s8(vget_high_s8(b70_s8))),
                    vget_high_s16(vmovl_s8(vget_high_s8(b70_s8)))}};

        for(int k=0;k<k_row;k++)
        {
          a_s8[k] = vld1q_s8(a_ptr + k * width_a);
          a_s16[k] =
              {
                  {vsub_s16(vget_low_s16(vmovl_s8(vget_low_s8(a_s8[k]))), vfilterOff),
                      vsub_s16(vget_high_s16(vmovl_s8(vget_low_s8(a_s8[k]))), vfilterOff),
                      vsub_s16(vget_low_s16(vmovl_s8(vget_high_s8(a_s8[k]))), vfilterOff),
                      vsub_s16(vget_high_s16(vmovl_s8(vget_high_s8(a_s8[k]))), vfilterOff)}};

          // Accumulate 0:
          c[k][0] = vmlal_s16(c[k][0], b00_s16.val[0], a_s16[k].val[0]);
          c[k][0] = vmlal_s16(c[k][0], b00_s16.val[1], a_s16[k].val[1]);
          c[k][0] = vmlal_s16(c[k][0], b00_s16.val[2], a_s16[k].val[2]);
          c[k][0] = vmlal_s16(c[k][0], b00_s16.val[3], a_s16[k].val[3]);

          // Accumulate 1:
          c[k][1] = vmlal_s16(c[k][1], b10_s16.val[0], a_s16[k].val[0]);
          c[k][1] = vmlal_s16(c[k][1], b10_s16.val[1], a_s16[k].val[1]);
          c[k][1] = vmlal_s16(c[k][1], b10_s16.val[2], a_s16[k].val[2]);
          c[k][1] = vmlal_s16(c[k][1], b10_s16.val[3], a_s16[k].val[3]);

          // Accumulate 2:
          c[k][2] = vmlal_s16(c[k][2], b20_s16.val[0], a_s16[k].val[0]);
          c[k][2] = vmlal_s16(c[k][2], b20_s16.val[1], a_s16[k].val[1]);
          c[k][2] = vmlal_s16(c[k][2], b20_s16.val[2], a_s16[k].val[2]);
          c[k][2] = vmlal_s16(c[k][2], b20_s16.val[3], a_s16[k].val[3]);

          // Accumulate 3:
          c[k][3] = vmlal_s16(c[k][3], b30_s16.val[0], a_s16[k].val[0]);
          c[k][3] = vmlal_s16(c[k][3], b30_s16.val[1], a_s16[k].val[1]);
          c[k][3] = vmlal_s16(c[k][3], b30_s16.val[2], a_s16[k].val[2]);
          c[k][3] = vmlal_s16(c[k][3], b30_s16.val[3], a_s16[k].val[3]);

          // Accumulate 4:
          c[k][4] = vmlal_s16(c[k][4], b40_s16.val[0], a_s16[k].val[0]);
          c[k][4] = vmlal_s16(c[k][4], b40_s16.val[1], a_s16[k].val[1]);
          c[k][4] = vmlal_s16(c[k][4], b40_s16.val[2], a_s16[k].val[2]);
          c[k][4] = vmlal_s16(c[k][4], b40_s16.val[3], a_s16[k].val[3]);

          // Accumulate 5:
          c[k][5] = vmlal_s16(c[k][5], b50_s16.val[0], a_s16[k].val[0]);
          c[k][5] = vmlal_s16(c[k][5], b50_s16.val[1], a_s16[k].val[1]);
          c[k][5] = vmlal_s16(c[k][5], b50_s16.val[2], a_s16[k].val[2]);
          c[k][5] = vmlal_s16(c[k][5], b50_s16.val[3], a_s16[k].val[3]);

          // Accumulate 6:
          c[k][6] = vmlal_s16(c[k][6], b60_s16.val[0], a_s16[k].val[0]);
          c[k][6] = vmlal_s16(c[k][6], b60_s16.val[1], a_s16[k].val[1]);
          c[k][6] = vmlal_s16(c[k][6], b60_s16.val[2], a_s16[k].val[2]);
          c[k][6] = vmlal_s16(c[k][6], b60_s16.val[3], a_s16[k].val[3]);

          // Accumulate 7:
          c[k][7] = vmlal_s16(c[k][7], b70_s16.val[0], a_s16[k].val[0]);
          c[k][7] = vmlal_s16(c[k][7], b70_s16.val[1], a_s16[k].val[1]);
          c[k][7] = vmlal_s16(c[k][7], b70_s16.val[2], a_s16[k].val[2]);
          c[k][7] = vmlal_s16(c[k][7], b70_s16.val[3], a_s16[k].val[3]);
        }

        a_ptr += 16;
        b_ptr += 16;

#if DEBUG_INFO
        if(nblock==0) {

    int32_t sum_temp[4];
    vst1q_s32(sum_temp, c0[k][0]);

    printf("step:%d - ", a_ptr - lhs_ptr);
    for(int k=0;k<4;k++)
    {
      printf(" %d ", sum_temp[k]);
    }
    printf("\n");

  }
#endif

      }
    }

    // This for loop performs the left-over accumulations
    // left elem is less than 16
    {


      //const int8x16_t a00_s8 = vld1q_s8(a_ptr);
      const int8x16_t b00_s8 = vld1q_s8(b_ptr + 0 * stride_b);
      const int8x16_t b10_s8 = vld1q_s8(b_ptr + 1 * stride_b);
      const int8x16_t b20_s8 = vld1q_s8(b_ptr + 2 * stride_b);
      const int8x16_t b30_s8 = vld1q_s8(b_ptr + 3 * stride_b);
      const int8x16_t b40_s8 = vld1q_s8(b_ptr + 4 * stride_b);
      const int8x16_t b50_s8 = vld1q_s8(b_ptr + 5 * stride_b);
      const int8x16_t b60_s8 = vld1q_s8(b_ptr + 6 * stride_b);
      const int8x16_t b70_s8 = vld1q_s8(b_ptr + 7 * stride_b);


      const int16x4x4_t b00_s16 =
          {
              {vget_low_s16(vmovl_s8(vget_low_s8(b00_s8))),
                  vget_high_s16(vmovl_s8(vget_low_s8(b00_s8))),
                  vget_low_s16(vmovl_s8(vget_high_s8(b00_s8))),
                  vget_high_s16(vmovl_s8(vget_high_s8(b00_s8)))}};

      const int16x4x4_t b10_s16 =
          {
              {vget_low_s16(vmovl_s8(vget_low_s8(b10_s8))),
                  vget_high_s16(vmovl_s8(vget_low_s8(b10_s8))),
                  vget_low_s16(vmovl_s8(vget_high_s8(b10_s8))),
                  vget_high_s16(vmovl_s8(vget_high_s8(b10_s8)))}};

      const int16x4x4_t b20_s16 =
          {
              {vget_low_s16(vmovl_s8(vget_low_s8(b20_s8))),
                  vget_high_s16(vmovl_s8(vget_low_s8(b20_s8))),
                  vget_low_s16(vmovl_s8(vget_high_s8(b20_s8))),
                  vget_high_s16(vmovl_s8(vget_high_s8(b20_s8)))}};

      const int16x4x4_t b30_s16 =
          {
              {vget_low_s16(vmovl_s8(vget_low_s8(b30_s8))),
                  vget_high_s16(vmovl_s8(vget_low_s8(b30_s8))),
                  vget_low_s16(vmovl_s8(vget_high_s8(b30_s8))),
                  vget_high_s16(vmovl_s8(vget_high_s8(b30_s8)))}};

      const int16x4x4_t b40_s16 =
          {
              {vget_low_s16(vmovl_s8(vget_low_s8(b40_s8))),
                  vget_high_s16(vmovl_s8(vget_low_s8(b40_s8))),
                  vget_low_s16(vmovl_s8(vget_high_s8(b40_s8))),
                  vget_high_s16(vmovl_s8(vget_high_s8(b40_s8)))}};

      const int16x4x4_t b50_s16 =
          {
              {vget_low_s16(vmovl_s8(vget_low_s8(b50_s8))),
                  vget_high_s16(vmovl_s8(vget_low_s8(b50_s8))),
                  vget_low_s16(vmovl_s8(vget_high_s8(b50_s8))),
                  vget_high_s16(vmovl_s8(vget_high_s8(b50_s8)))}};

      const int16x4x4_t b60_s16 =
          {
              {vget_low_s16(vmovl_s8(vget_low_s8(b60_s8))),
                  vget_high_s16(vmovl_s8(vget_low_s8(b60_s8))),
                  vget_low_s16(vmovl_s8(vget_high_s8(b60_s8))),
                  vget_high_s16(vmovl_s8(vget_high_s8(b60_s8)))}};

      const int16x4x4_t b70_s16 =
          {
              {vget_low_s16(vmovl_s8(vget_low_s8(b70_s8))),
                  vget_high_s16(vmovl_s8(vget_low_s8(b70_s8))),
                  vget_low_s16(vmovl_s8(vget_high_s8(b70_s8))),
                  vget_high_s16(vmovl_s8(vget_high_s8(b70_s8)))}};

      // Convert a00_s8 to int16_t and get the lower part
      // Convert a00_s8 to int16_t and get the lower part
      for(int k=0;k<k_row;k++)
      {
        a_s8[k] = vld1q_s8(a_ptr + k * width_a);
        a_s16[k] =
            {
                {vsub_s16(vget_low_s16(vmovl_s8(vget_low_s8(a_s8[k]))), vfilterOff),
                    vsub_s16(vget_high_s16(vmovl_s8(vget_low_s8(a_s8[k]))), vfilterOff),
                    vsub_s16(vget_low_s16(vmovl_s8(vget_high_s8(a_s8[k]))), vfilterOff),
                    vsub_s16(vget_high_s16(vmovl_s8(vget_high_s8(a_s8[k]))), vfilterOff)}};

      }

      int ii = 0; // index
      if(vec_a_end_addr - a_ptr  >= 8) {

        for(int k=0;k<k_row;k++)
        {
          // Accumulate 0:
          c[k][0] = vmlal_s16(c[k][0], b00_s16.val[0+ii], a_s16[k].val[0+ii]);
          c[k][0] = vmlal_s16(c[k][0], b00_s16.val[1+ii], a_s16[k].val[1+ii]);

          // Accumulate 1:
          c[k][1] = vmlal_s16(c[k][1], b10_s16.val[0+ii], a_s16[k].val[0+ii]);
          c[k][1] = vmlal_s16(c[k][1], b10_s16.val[1+ii], a_s16[k].val[1+ii]);

          // Accumulate 2:
          c[k][2] = vmlal_s16(c[k][2], b20_s16.val[0+ii], a_s16[k].val[0+ii]);
          c[k][2] = vmlal_s16(c[k][2], b20_s16.val[1+ii], a_s16[k].val[1+ii]);

          // Accumulate 3:
          c[k][3] = vmlal_s16(c[k][3], b30_s16.val[0+ii], a_s16[k].val[0+ii]);
          c[k][3] = vmlal_s16(c[k][3], b30_s16.val[1+ii], a_s16[k].val[1+ii]);

          // Accumulate 4:
          c[k][4] = vmlal_s16(c[k][4], b40_s16.val[0+ii], a_s16[k].val[0+ii]);
          c[k][4] = vmlal_s16(c[k][4], b40_s16.val[1+ii], a_s16[k].val[1+ii]);

          // Accumulate 5:
          c[k][5] = vmlal_s16(c[k][5], b50_s16.val[0+ii], a_s16[k].val[0+ii]);
          c[k][5] = vmlal_s16(c[k][5], b50_s16.val[1+ii], a_s16[k].val[1+ii]);

          // Accumulate 6:
          c[k][6] = vmlal_s16(c[k][6], b60_s16.val[0+ii], a_s16[k].val[0+ii]);
          c[k][6] = vmlal_s16(c[k][6], b60_s16.val[1+ii], a_s16[k].val[1+ii]);

          // Accumulate 7:
          c[k][7] = vmlal_s16(c[k][7], b70_s16.val[0+ii], a_s16[k].val[0+ii]);
          c[k][7] = vmlal_s16(c[k][7], b70_s16.val[1+ii], a_s16[k].val[1+ii]);
        }
        a_ptr+=8;
        ii+= (8/4);

      }


      if(vec_a_end_addr - a_ptr >= 4) {
        for(int k=0;k<k_row;k++)
        {
          // Accumulate 0:
          c[k][0] = vmlal_s16(c[k][0], b00_s16.val[0+ii], a_s16[k].val[0+ii]);

          // Accumulate 1:
          c[k][1] = vmlal_s16(c[k][1], b10_s16.val[0+ii], a_s16[k].val[0+ii]);

          // Accumulate 2:
          c[k][2] = vmlal_s16(c[k][2], b20_s16.val[0+ii], a_s16[k].val[0+ii]);

          // Accumulate 3:
          c[k][3] = vmlal_s16(c[k][3], b30_s16.val[0+ii], a_s16[k].val[0+ii]);

          // Accumulate 4:
          c[k][4] = vmlal_s16(c[k][4], b40_s16.val[0+ii], a_s16[k].val[0+ii]);

          // Accumulate 5:
          c[k][5] = vmlal_s16(c[k][5], b50_s16.val[0+ii], a_s16[k].val[0+ii]);

          // Accumulate 6:
          c[k][6] = vmlal_s16(c[k][6], b60_s16.val[0+ii], a_s16[k].val[0+ii]);

          // Accumulate 7:
          c[k][7] = vmlal_s16(c[k][7], b70_s16.val[0+ii], a_s16[k].val[0+ii]);
        }

        a_ptr+=4;
        ii+= (4/4);
      }

      int left = vec_a_end_addr - a_ptr;
      if(left > 0 )
      {
        int16_t l1_mask[4] = { 1 , 0 ,0 ,0};
        int16_t l2_mask[4] = { 1 , 1 ,0 ,0};
        int16_t l3_mask[4] = { 1 , 1 ,1 ,0};
        int16x4_t left_mask;

        if(left==1) left_mask = vld1_s16(l1_mask);
        else if(left==2) left_mask = vld1_s16(l2_mask);
        else left_mask = vld1_s16(l3_mask);
        for(int k=0;k<k_row;k++)
        {
          // Accumulate 0:
          c[k][0] = vmlal_s16(c[k][0], b00_s16.val[0+ii], vmul_s16(left_mask, a_s16[k].val[0+ii]));

          // Accumulate 1:
          c[k][1] = vmlal_s16(c[k][1], b10_s16.val[0+ii], vmul_s16(left_mask, a_s16[k].val[0+ii]));

          // Accumulate 2:
          c[k][2] = vmlal_s16(c[k][2], b20_s16.val[0+ii], vmul_s16(left_mask, a_s16[k].val[0+ii]));

          // Accumulate 3:
          c[k][3] = vmlal_s16(c[k][3], b30_s16.val[0+ii], vmul_s16(left_mask, a_s16[k].val[0+ii]));

          // Accumulate 4:
          c[k][4] = vmlal_s16(c[k][4], b40_s16.val[0+ii], vmul_s16(left_mask, a_s16[k].val[0+ii]));

          // Accumulate 5:
          c[k][5] = vmlal_s16(c[k][5], b50_s16.val[0+ii], vmul_s16(left_mask, a_s16[k].val[0+ii]));

          // Accumulate 6:
          c[k][6] = vmlal_s16(c[k][6], b60_s16.val[0+ii], vmul_s16(left_mask, a_s16[k].val[0+ii]));

          // Accumulate 7:
          c[k][7] = vmlal_s16(c[k][7], b70_s16.val[0+ii], vmul_s16(left_mask, a_s16[k].val[0+ii]));

        }
        a_ptr += left;
      }
      // b_ptr += stride_b;
    }


    for (int k = 0; k < k_row; k++)
    {

      for (int o = 0; o < 8; o++)
      {
        int32x2_t temp = vadd_s32(vget_high_s32(c[k][o]), vget_low_s32(c[k][o]));
        int32x2_t temp2 = vpadd_s32(temp, temp);
        int32_t sum1 = vget_lane_s32(temp2, 0);

        //vst1q_s32(accum_ptr + nblock * 8 + k, c0[k]);
        accum_ptr[k*width_b + nblock * 8  + o] = sum1;

      }
    }
  }


}


// add bias offset for output, scaling and clip from s32 to s8.
// out = (int8)((input * matscale) + bias * vscale)
// ! bias is type int32_t. bias are same for a channel(row)
// width = 8n
void vector_scale_bias_offset_clip_s8(int32_t *input, int32_t biasValue, int8_t *output, float32_t scale, float32_t bscale, int width, bool doBias, bool doRelu,int32_t outOff)
{

  const float32x4_t scale_v = vdupq_n_f32(scale);
  const float32x4_t bscale_v = vdupq_n_f32(bscale);
  const int32x4_t zero_v = vdupq_n_s32(0);

  // clip
  const int32x4_t max = vdupq_n_s32(127);
  const int32x4_t min = vdupq_n_s32(-128);

  //out offset
  const int32x4_t voff = vdupq_n_s32(outOff);

#if DEBUG_INFO
  printf("doBias:%d doRelu:%d\n",doBias,doRelu);
printf("s:%f bs:%f -- ",scale, bscale);
printf("o:%p - %p\n",output, output+width);
#endif

  if(!doBias) {
    for (int i=0; i<width;i+=8)
    {
      const int32x4_t a0_s32 = vld1q_s32(input+i);
      const int32x4_t a1_s32 = vld1q_s32(input+i+4);

#if DEBUG_INFO
      int32_t aa[4];
        if(i==0) {
          vst1q_s32(aa,b00);
          printf("%d %d %d %d ---\n",aa[0],aa[1],aa[2],aa[3]);
          vst1q_s32(aa,b01);
          printf("%d %d %d %d ---\n",aa[0],aa[1],aa[2],aa[3]);

        }
#endif
      int16x4_t vres1, vres2;
      if(doRelu)
      {
        vres1   = vmovn_s32(vmaxq_s32(vminq_s32(vaddq_s32(voff,vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmaxq_s32(zero_v,a0_s32)),scale_v))), max),min));
        vres2   = vmovn_s32(vmaxq_s32(vminq_s32(vaddq_s32(voff,vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmaxq_s32(zero_v,a1_s32)),scale_v))), max),min));

      }
      else {
        vres1   = vmovn_s32(vmaxq_s32(vminq_s32(vaddq_s32(voff,vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(a0_s32),scale_v))), max),min));
        vres2   = vmovn_s32(vmaxq_s32(vminq_s32(vaddq_s32(voff,vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(a1_s32),scale_v))), max),min));

      }

      int8x8_t vres = vmovn_s16(vcombine_s16(vres1,vres2));
      vst1_s8(output+i, vres);

    }
  }
  else if(doBias && bscale == 1.0) {
    const int32x4_t v_bias = vdupq_n_s32(biasValue);

    for (int i=0; i<width;i+=8)
    {
      const int32x4_t a0_s32 = vld1q_s32(input+i);
      const int32x4_t a1_s32 = vld1q_s32(input+i+4);


#if DEBUG_INFO
      int32_t aa[4];
        if(i==0) {
          vst1q_s32(aa,a0_s32);
          printf("%d %d %d %d ---\n",aa[0],aa[1],aa[2],aa[3]);
          vst1q_s32(aa,a1_s32);
          printf("%d %d %d %d ---\n",aa[0],aa[1],aa[2],aa[3]);

        }
#endif


      int16x4_t vres1, vres2;

      if(doRelu)
      {
        vres1   = vmovn_s32(vmaxq_s32(vminq_s32(vaddq_s32(voff,vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmaxq_s32(zero_v,vaddq_s32(v_bias,a0_s32))),scale_v))), max),min));
        vres2   = vmovn_s32(vmaxq_s32(vminq_s32(vaddq_s32(voff,vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmaxq_s32(zero_v,vaddq_s32(v_bias,a1_s32))),scale_v))), max),min));

      }
      else {
        vres1   = vmovn_s32(vmaxq_s32(vminq_s32(vaddq_s32(voff,vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vaddq_s32(v_bias,a0_s32)),scale_v))), max),min));
        vres2   = vmovn_s32(vmaxq_s32(vminq_s32(vaddq_s32(voff,vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vaddq_s32(v_bias,a1_s32)),scale_v))), max),min));

      }
#if DEBUG_INFO
      int16_t dd[4];
        if(i==0) {
          vst1_s16(dd,vres1);
          printf("%d %d %d %d ---\n",dd[0],dd[1],dd[2],dd[3]);
          vst1_s16(dd,vres2);
          printf("%d %d %d %d ---\n",dd[0],dd[1],dd[2],dd[3]);

        }
#endif
      int8x8_t vres = vmovn_s16(vcombine_s16(vres1,vres2));

      vst1_s8(output+i, vres);

    }
  }
  else {
    printf("[ERR] bscale = %f\n. not supported bias scale\n", bscale);
    return;
  }

}

// nhwc input matrix to col
/*
void im2col_nhwc(int8_t* in, int8_t* out, int inw, int inh, int inch, int outw, int outh, int kernel_h, int kernel_w, int stride_h, int stride_w )
{
    const int stride = kernel_h * kernel_w * outw * outh;

    for (int p = 0; p < inch; p++)
    {
        const signed char* input_ch_p = in + (p* inw*inh );
        int retID = stride * p;
        for (int u = 0; u < kernel_h; u++)
        {
            for (int v = 0; v < kernel_w; v++)
            {
                for (int i = 0; i < outh; i++)
                {
                    for (int j = 0; j < outw; j++)
                    {
                        int row = u + i * stride_h;
                        int col = v + j * stride_w;
                        int index = row * inw + col;
                        out[retID] = input_ch_p[index];
                        retID++;
                    }
                }
            }
        }
    }
}
*/
inline void linearize_volume_nchw(const int8_t *const in_ptr,
                                  int8_t *out_ptr,
                                  int top_left_x,
                                  int top_left_y,
                                  int kernel_width,
                                  int kernel_height,
                                  int kernel_depth,
                                  int input_w,
                                  int input_h,
                                  int input_stride_x,
                                  int input_stride_y,
                                  int input_stride_z,
                                  int has_pads,
                                  int pad_value,
                                  int dilation_x,
                                  int dilation_y,
                                  int32_t input_offset)
{
  const int kernel_size2 = kernel_width * kernel_height;
  const int x_e = top_left_x + kernel_width * dilation_x;
  const int y_e = top_left_y + kernel_height * dilation_y;

  // Linearize volume
  int d = 0;
  // This for loop linearize a volume with 3 slices. This allows:
  // 1) to reduce the iterations of the outer for loop "d"
  // 2) to have an optimized im2col for the first convolution layer where usually we have 3 IFMs
  for (; d <= (kernel_depth - 3); d += 3)
  {
    for (int y = top_left_y; y < y_e; y += dilation_y)
    {
      if ((y < 0 || y >= input_h) && has_pads)
      {
        // All the values will be the offset (will be zeros when not quantized)
        for (int x = top_left_x; x < x_e; x += dilation_x, ++out_ptr)
        {
          *(out_ptr + 0 * kernel_size2) = pad_value;
          *(out_ptr + 1 * kernel_size2) = pad_value;
          *(out_ptr + 2 * kernel_size2) = pad_value;
        }
      }
      else
      {
        for (int x = top_left_x; x < x_e; x += dilation_x, ++out_ptr)
        {
          if ((x < 0 || x >= input_w) && has_pads)
          {
            *(out_ptr + 0 * kernel_size2) = pad_value;
            *(out_ptr + 1 * kernel_size2) = pad_value;
            *(out_ptr + 2 * kernel_size2) = pad_value;
          }
          else
          {
            *(out_ptr + 0 * kernel_size2) = *((in_ptr + ((d + 0) * input_stride_z + y * input_stride_y + x * input_stride_x))) - input_offset;
            *(out_ptr + 1 * kernel_size2) = *((in_ptr + ((d + 1) * input_stride_z + y * input_stride_y + x * input_stride_x)))  - input_offset;
            *(out_ptr + 2 * kernel_size2) = *((in_ptr + ((d + 2) * input_stride_z + y * input_stride_y + x * input_stride_x)))  - input_offset;
          }
        }
      }
    }
    out_ptr += 2 * kernel_size2;
  }

  // Left over
  for (; d < kernel_depth; d++)
  {
    for (int y = top_left_y; y < y_e; y += dilation_y)
    {
      if ((y < 0 || y >= input_h) && has_pads)
      {
        // All the values will be the offset (will be zeros when not quantized)
        memset(out_ptr, pad_value, kernel_width);
        out_ptr += kernel_width;
      }
      else
      {
        for (int x = top_left_x; x < x_e; x += dilation_x, ++out_ptr)
        {
          if ((x < 0 || x >= input_w) && has_pads)
          {
            *out_ptr = pad_value;
          }
          else
          {
            *out_ptr = *((in_ptr + (d * input_stride_z + y * input_stride_y + x * input_stride_x))) -  - input_offset;
          }
        }
      }
    }
  }
}

// 4*k K*4
void gemm_s8_v2(int8_t *lhs_ptr, int8_t *rhs_ptr, int8_t *accum_ptr, int width_a, int width_b, size_t stride_b, int stride_c, int32_t *biasPtr, float32_t scale, bool doRelu, int32_t filterOffset)
{
  
  // block = 1x16 * 16*8
  
  //TODO. temp
  // check overflow 
  // upscale to s32 if s16 datatype overflow because of filterOffset  
  assert(filterOffset < 32700 );
  assert(filterOffset > -32700);

  const int32x4_t vfilterOff = vdupq_n_s32(filterOffset);

  int32_t s[4][8] = {{0, }};

  int8_t result[8];

/*
  int8_t *ap1 = lhs_ptr + 0*width_a;                         
  int8_t *ap2 = lhs_ptr + 1*width_a;                         
  int8_t *ap3 = lhs_ptr + 2*width_a;                         
  int8_t *ap4 = lhs_ptr + 3*width_a;                         

  int8_t *bp1 = rhs_ptr + 0 * stride_b; 
  int8_t *bp2 = rhs_ptr + 1 * stride_b; 
  int8_t *bp3 = rhs_ptr + 2 * stride_b; 
  int8_t *bp4 = rhs_ptr + 3 * stride_b; 
  int8_t *bp5 = rhs_ptr + 4 * stride_b; 
  int8_t *bp6= rhs_ptr + 5 * stride_b; 
  int8_t *bp7 = rhs_ptr + 6 * stride_b; 
  int8_t *bp8 = rhs_ptr + 7 * stride_b; 
*/

//CacheCounts CA,CB,CC;
//StartCacheCounts();
  for(int k=0;k< width_a;k++) {
    for(int c=0;c<4;c++) {
    
      for(int i=0;i<8;i++) {
            s[c][i] += *(lhs_ptr + c*width_a + k) * *(rhs_ptr + i * stride_b + k);
      }
    }
  }
//StopCacheCounts(&CA);
/*
  for(int c=0;c<4;c++)
  {
    int32_t bias = *(biasPtr+c);
    for(int i=0;i<8;i++) {
      result[i] = (int8_t)((float32_t)(s[c][i]+bias)* scale);
      if(doRelu && result[i]<0) result[i]=0;
    }
    memcpy(accum_ptr + c*stride_c, result, 8) ;
  }
  */

 //StartCacheCounts(); 
  const float32x4_t scale_v = vdupq_n_f32(scale);
  //const float32x4_t bscale_v = vdupq_n_f32(bscale);
  const int32x4_t zero_v = vdupq_n_s32(0);

  // clip
  const int32x4_t max = vdupq_n_s32(127);
  const int32x4_t min = vdupq_n_s32(-128);

#if DEBUG_INFO_V    
printf("s:%f bs:%f -- ",scale, bscale);
printf("o:%p - %p\n",output, output+width);  
#endif  

  if(biasPtr == nullptr) {
    printf("[---------todo ------------]\n");
  }
  else if(biasPtr!=nullptr) { // bscale= 1
//StopCacheCounts(&CB); 
//StartCacheCounts(); 
    //const int32x4_t v_bias = vld1q_s32(bias);
    for(int c=0;c<1;c++)
    {
      const int32x4_t v_bias = vdupq_n_s32(*(biasPtr+c));
       //const int32x4_t v_bias = vdupq_n_s32(0);
        const int32x4_t a0_s32 = vld1q_s32(&s[c][0]);
        const int32x4_t a1_s32 = vld1q_s32(&s[c][4]);

        int16x4_t vres1, vres2;
        
        if(doRelu)
        {
          vres1   = vmovn_s32(vmaxq_s32(vminq_s32(vaddq_s32(vfilterOff,vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmaxq_s32(zero_v,vaddq_s32(v_bias,a0_s32))),scale_v))), max),min));
          vres2   = vmovn_s32(vmaxq_s32(vminq_s32(vaddq_s32(vfilterOff,vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmaxq_s32(zero_v,vaddq_s32(v_bias,a1_s32))),scale_v))), max),min));

        }
        else {
          vres1   = vmovn_s32(vmaxq_s32(vminq_s32(vaddq_s32(vfilterOff,vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vaddq_s32(v_bias,a0_s32)),scale_v))), max),min));
          vres2   = vmovn_s32(vmaxq_s32(vminq_s32(vaddq_s32(vfilterOff,vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vaddq_s32(v_bias,a1_s32)),scale_v))), max),min));

        }


#if DEBUG_INFO_V
        int32_t aa[4];
        if(i==0) {
          vst1q_s32(aa,a0_s32);
          printf("%d %d %d %d ---\n",aa[0],aa[1],aa[2],aa[3]);
          vst1q_s32(aa,a1_s32);
          printf("%d %d %d %d ---\n",aa[0],aa[1],aa[2],aa[3]);
          
        }
      
        int16_t dd[4];
        if(i==0) {
          vst1_s16(dd,vres1);
          printf("%d %d %d %d ---\n",dd[0],dd[1],dd[2],dd[3]);
          vst1_s16(dd,vres2);
          printf("%d %d %d %d ---\n",dd[0],dd[1],dd[2],dd[3]);
          
        }        
#endif  
        int8x8_t vres = vmovn_s16(vcombine_s16(vres1,vres2));

        vst1_s8(accum_ptr + c*stride_c, vres);
    }
//StopCacheCounts(&CC);     
  }

//PrintCacheCountsRow(CA);
//PrintCacheCounts(CB);
//PrintCacheCounts(CC);

}


// input nchw, output nchw
void fwdConvolution_nchw_acl(int8_t *input, int8_t *kernel, int8_t *bias, int8_t *output, 
                      float inS, float kS, float bS, float outS,
                      int32_t inOffset, int32_t filterOffset, int32_t biasOffset, int32_t outOffset,
                      dim_t N, dim_t C, dim_t H, dim_t W, dim_t KN, dim_t KH, dim_t KW, int pad_size, int stride_size, size_t group,
                      size_t dilation, bool doRelu, bool doBias, 
                      int out_h, int out_w) {


  

  #if 0

   for(int c=0;c<3;c++)
   {
       printf("\nCH=%d\n", c);
 
    for(int h=0;h<5;h++) {
      for(int w=0;w<5;w++) {
         printf("%d ",input[c*H*W + h*W + w]);
      }
      printf("\n");
    } 
  }
printf("0----- Kernel N=0\n")  ;
   for(int c=0;c<3;c++)
   {
       printf("\nCH=%d\n", c);
  
    for(int h=0;h<7;h++) {
      for(int w=0;w<7;w++) {
         printf("%d ",kernel[c*KW*KH + h*KW + w+0]);
      }
      printf("\n");
    } 
  }
#endif
    CacheCounts cache_counts1;
    CacheCounts cache_counts2;
  StartCacheCounts();

  
  // make patched input
  // For each convolution 'jump' in the input tensor:
  clock_t t1s = clock();
  
 
    arm_compute::PadStrideInfo pad_stride_info = arm_compute::PadStrideInfo(stride_size,stride_size,pad_size,pad_size);
    arm_compute::Size2D dilation_2d(dilation,dilation);

     arm_compute::ActivationLayerInfo act_info;
    if (doRelu) {
        act_info = arm_compute::ActivationLayerInfo(
            arm_compute::ActivationLayerInfo::ActivationFunction::RELU);
    }


    const arm_compute::DataType            data_type = arm_compute::DataType::QASYMM8_SIGNED;

    // Create tensor
    // input NCHW. 
    const arm_compute::TensorShape         src_shape     =  arm_compute::TensorShape(W, H, C);
    const arm_compute::TensorInfo info(src_shape, 1, data_type);
    //const size_t     required_alignment = 64;
    arm_compute::Tensor           inputT{};
    inputT.allocator()->init(info); //, required_alignment);


    const arm_compute::TensorShape         shape3     =  arm_compute::TensorShape(KW,KH,src_shape.z(), KN); // src_shape z == C
    const arm_compute::TensorInfo info3(shape3, 1, data_type);
    arm_compute::Tensor           kernelT{};
    kernelT.allocator()->init(info3); //, required_alignment);



    const arm_compute::TensorShape         shape2     =  arm_compute::TensorShape(out_h, out_w, KN);
    const arm_compute::TensorInfo info2(shape2, 1, data_type);
    arm_compute::Tensor           outputT{};
    outputT.allocator()->init(info2); //, required_alignment);

printf("\n-------------------------\ninS:%f, kernelS:%f outS:%f\n",inS,kS,outS);
    // quantization
    inputT.info()->set_quantization_info(QuantizationInfo(inS, 0));  // scale, offset
    outputT.info()->set_quantization_info(QuantizationInfo(outS, 0));  // scale, offset
    kernelT.info()->set_quantization_info(QuantizationInfo(kS, 0));  // scale, offset
  /*
    inputT.info()->set_quantization_info(QuantizationInfo(1.0f, 0));  // scale, offset
    outputT.info()->set_quantization_info(QuantizationInfo(1.0f, 0));  // scale, offset
    kernelT.info()->set_quantization_info(QuantizationInfo(1.0f, 0));  // scale, offset   
*/
    arm_compute::Tensor           biasT{};
    if(bias!=nullptr && doBias) {
      const arm_compute::TensorShape         shape4     =  arm_compute::TensorShape(KN);
      const arm_compute::TensorInfo info4(shape4, 1, arm_compute::DataType::S32);
      biasT.allocator()->init(info4); //, required_alignment);
      //biasT.info()->set_quantization_info(QuantizationInfo(bS, 0)); 
    }    

    
    arm_compute::NEConvolutionLayer act_func;

  act_func.configure(&inputT, &kernelT, (bias!=nullptr && doBias) ? &biasT : nullptr,
                        &outputT, 
                        pad_stride_info, arm_compute::WeightsInfo(), dilation_2d, act_info);


    inputT.allocator()->allocate();
    outputT.allocator()->allocate();
    kernelT.allocator()->allocate();                        

    auto *iptr = reinterpret_cast<int8_t *>(inputT.buffer());
    auto *kptr = reinterpret_cast<int8_t *>(kernelT.buffer());
    auto *optr = reinterpret_cast<int8_t *>(outputT.buffer());

    memcpy(iptr,input, N*H*W*C);
    memcpy(kptr,kernel, KN*KW*KH*C);

    if(bias!=nullptr && doBias) {
      biasT.allocator()->allocate();                      
      auto *bptr = reinterpret_cast<int8_t *>(biasT.buffer());
      memcpy(bptr,bias, KN * sizeof(int32_t));
    }

    act_func.run();

    memcpy(output, optr,out_w*out_h*KN);

    StopCacheCounts(&cache_counts1);
  clock_t t1e = clock();
  //cout <<"conv(acl nchw) : "<<(double)(t1e- t1s)/CLOCKS_PER_SEC*1000 << std::endl;
  //PrintCacheCounts(cache_counts1);

 
}


// input nhwc, output nhwc
void fwdConvolution_nhwc_acl(int8_t *input, int8_t *kernel, int8_t *bias, int8_t *output, 
                      float inS, float kS, float bS, float outS,
                      int32_t inOffset, int32_t filterOffset, int32_t biasOffset, int32_t outOffset,
                      dim_t N, dim_t C, dim_t H, dim_t W, dim_t KN, dim_t KH, dim_t KW, int pad_size, int stride_size, size_t group,
                      size_t dilation, bool doRelu, bool doBias, 
                      int out_h, int out_w) {




  // make patched input
  // For each convolution 'jump' in the input tensor:
  clock_t t1s = clock();


    arm_compute::PadStrideInfo pad_stride_info = arm_compute::PadStrideInfo(stride_size,stride_size,pad_size,pad_size);
    arm_compute::Size2D dilation_2d(dilation,dilation);

     arm_compute::ActivationLayerInfo act_info;
    if (doRelu) {
        act_info = arm_compute::ActivationLayerInfo(
            arm_compute::ActivationLayerInfo::ActivationFunction::RELU);
    }
 




    const arm_compute::DataType            data_type = arm_compute::DataType::QASYMM8_SIGNED;

    // Create tensor
    // input NCHW. 
    const arm_compute::TensorShape         src_shape     =  arm_compute::TensorShape(C, W, H);
    const arm_compute::TensorInfo info(src_shape, 1, data_type,arm_compute::DataLayout::NHWC);
    //const size_t     required_alignment = 64;
    arm_compute::Tensor           inputT{};
    inputT.allocator()->init(info); //, required_alignment);


    const arm_compute::TensorShape         shape3     =  arm_compute::TensorShape( C ,KW,KH,KN); // src_shape z == C
    const arm_compute::TensorInfo info3(shape3, 1, data_type,arm_compute::DataLayout::NHWC);
    arm_compute::Tensor           kernelT{};
    kernelT.allocator()->init(info3); //, required_alignment);

    // scale offset??

    const arm_compute::TensorShape         shape2     =  arm_compute::TensorShape(KN,out_w, out_h);
    const arm_compute::TensorInfo info2(shape2, 1, data_type,arm_compute::DataLayout::NHWC);
    arm_compute::Tensor           outputT{};
    outputT.allocator()->init(info2); //, required_alignment);
  
    arm_compute::Tensor           biasT{};
    if(bias!=nullptr && doBias) {
      const arm_compute::TensorShape         shape4     =  arm_compute::TensorShape(KN);
      const arm_compute::TensorInfo info4(shape4, 1, arm_compute::DataType::S32);
      biasT.allocator()->init(info4); //, required_alignment);
  }
  
  //printf("\n-------------------------\ninS:%f, kernelS:%f outS:%f bS:%f\n",inS,kS,outS,bS);
    // quantization
    inputT.info()->set_quantization_info(QuantizationInfo(inS, 0));  // scale, offset
    outputT.info()->set_quantization_info(QuantizationInfo(outS, 0));  // scale, offset
    kernelT.info()->set_quantization_info(QuantizationInfo(kS, 0));  // scale, offset

    arm_compute::NEConvolutionLayer act_func;

    act_func.configure(&inputT, &kernelT, (bias!=nullptr && doBias) ? &biasT : nullptr,
                        &outputT, 
                        pad_stride_info, arm_compute::WeightsInfo(), dilation_2d, act_info);
       
  
    inputT.allocator()->allocate();
    outputT.allocator()->allocate();
    kernelT.allocator()->allocate();                        
    
    auto *iptr = reinterpret_cast<int8_t *>(inputT.buffer());
    auto *kptr = reinterpret_cast<int8_t *>(kernelT.buffer());
    auto *optr = reinterpret_cast<int8_t *>(outputT.buffer());

    memcpy(iptr,input, N*H*W*C);
    memcpy(kptr,kernel, KN*KW*KH*C);

    if(bias!=nullptr && doBias) {
      biasT.allocator()->allocate();                      
      auto *bptr = reinterpret_cast<int8_t *>(biasT.buffer());
      memcpy(bptr,bias, KN * sizeof(int32_t));
    }
    
    act_func.run();

      memcpy(output, optr,out_w*out_h*KN);


  clock_t t1e = clock();
  //cout <<"conv(acl nhwc) : "<<(double)(t1e- t1s)/CLOCKS_PER_SEC*1000 << std::endl;

    

  
}



void fwdConvolution_nhwc_acl_f32(float32_t *input, float32_t *kernel, float32_t *bias, float32_t *output, 
                      float inS, float kS, float bS, float outS,
                      int32_t inOffset, int32_t filterOffset, int32_t biasOffset, int32_t outOffset,
                      dim_t N, dim_t C, dim_t H, dim_t W, dim_t KN, dim_t KH, dim_t KW, int pad_size, int stride_size, size_t group,
                      size_t dilation, bool doRelu, bool doBias, 
                      int out_h, int out_w) {


  
 #if 0

   for(int c=0;c<3;c++)
   {
       printf("\nCH=%d\n", c);

    for(int h=0;h<5;h++) {
      for(int w=0;w<5;w++) {
         printf("%.2f ",input[h*W*C + w*C + c]);
      }
      printf("\n");
    } 
  }
printf("Kernel N=0\n")  ;
   for(int c=0;c<3;c++)
   {
       printf("\nCH=%d\n", c);
 
    for(int h=0;h<7;h++) {
      for(int w=0;w<7;w++) {
         printf("%.2f ",kernel[h*KW*C + w*C+c]);
      }
      printf("\n");
    } 
  }
#endif

    CacheCounts cache_counts1;
    CacheCounts cache_counts2;
  StartCacheCounts();

  
  // make patched input
  // For each convolution 'jump' in the input tensor:
  clock_t t1s = clock();
  
 
    arm_compute::PadStrideInfo pad_stride_info = arm_compute::PadStrideInfo(stride_size,stride_size,pad_size,pad_size);
    arm_compute::Size2D dilation_2d(dilation,dilation);

     arm_compute::ActivationLayerInfo act_info;
    if (doRelu) {
        act_info = arm_compute::ActivationLayerInfo(
            arm_compute::ActivationLayerInfo::ActivationFunction::RELU);
    }



    //layer->outputs.push_back(MakeOutputTensor(node.GetOpShape()[0]));

    const arm_compute::DataType            data_type = arm_compute::DataType::F32;

    // Create tensor
    //const arm_compute::TensorShape         src_shape     =  arm_compute::TensorShape(W, H,C);
    //const arm_compute::TensorInfo info(src_shape, 1, data_type,arm_compute::DataLayout::NCHW);
    
    const arm_compute::TensorShape         src_shape     =  arm_compute::TensorShape(C, W, H);
    const arm_compute::TensorInfo info(src_shape, 1, data_type,arm_compute::DataLayout::NHWC);
    //const size_t     required_alignment = 64;
    arm_compute::Tensor           inputT{};
    inputT.allocator()->init(info); //, required_alignment);

    //const arm_compute::TensorShape         shape3     =  arm_compute::TensorShape(KN, C ,KH,KW); // src_shape z == C
    //const arm_compute::TensorInfo info3(shape3, 1, data_type);    
    //const arm_compute::TensorShape         shape3     =  arm_compute::TensorShape(KH,KW,C,KN); // src_shape z == C
    const arm_compute::TensorShape         shape3     =  arm_compute::TensorShape( C ,KW,KH,KN); // src_shape z == C
    const arm_compute::TensorInfo info3(shape3, 1, data_type,arm_compute::DataLayout::NHWC);
    arm_compute::Tensor           kernelT{};
    kernelT.allocator()->init(info3); //, required_alignment);


    // scale offset??
    //const arm_compute::TensorShape         shape2     =  arm_compute::TensorShape(out_w, out_h,KN);
    //const arm_compute::TensorInfo info2(shape2, 1, data_type);
    const arm_compute::TensorShape         shape2     =  arm_compute::TensorShape(KN,out_w, out_h);
    const arm_compute::TensorInfo info2(shape2, 1, data_type,arm_compute::DataLayout::NHWC);
    arm_compute::Tensor           outputT{};
    outputT.allocator()->init(info2); //, required_alignment);

    arm_compute::Tensor           biasT{};
    if(bias!=nullptr && doBias) {
      const arm_compute::TensorShape         shape4     =  arm_compute::TensorShape(KN);
      const arm_compute::TensorInfo info4(shape4, 1, data_type);
      biasT.allocator()->init(info4); //, required_alignment);

    }
    

    arm_compute::NEConvolutionLayer act_func;
    /*
     * @param[in] input            Source tensor. 3 lower dimensions represent a single input [width, height, IFM],
     *                             while every optional dimension from 4 and above represent a batch of inputs.
     *                             Data types supported: QASYMM8/QASYMM8_SIGNED/F16/F32.
     * @param[in] weights          Weights tensor. Weights are 4D tensor with dimensions [kernel_x, kernel_y, IFM, OFM]. Data type supported:Same as @p input.
     * @param[in] biases           Biases tensor. Shared biases supported. Biases are 1D tensor with dimensions [OFM].
     *                             Data type supported: Should match @p input data type, except for input of QASYMM8/QASYMM8_SIGNED type where biases should be of S32 type.
     * @param[in] output           Destination tensor. 3 lower dimensions represent a single output [width, height, OFM], while the rest represent batch of outputs.
     *                             Data types supported: Same as @p input.
     * @param[in] conv_info        Contains padding and stride information described in @ref PadStrideInfo.
     * @param[in] weights_info     Specifies if the weights tensor has been reshaped with NEWeightsReshapeKernel. If this is not part of the fully connected layer the weights
     *                             tensor has also been transposed with NEGEMMTranspose1xWKernel. Data type supported: Same as @p input.
     * @param[in] dilation         (Optional) Dilation, in elements, across x and y. Defaults to (1, 1).
     * @param[in] act_info         (Optional) Activation layer information in case of a fused activation.
     * @param[in] enable_fast_math (Optional) Enable fast math computation. In case this flag were set, the function could dispatch the fastest implementation
     *                             available which may introduce a drop of accuracy as well. Default is false
     * @param[in] num_groups       (Optional) Number of groups when performing a grouped convolution. num_groups != 1 is not supported
     */
//        void configure(ITensor *input, const ITensor *weights, const ITensor *biases, ITensor *output, const PadStrideInfo &conv_info, const WeightsInfo &weights_info = WeightsInfo(),
                   //const Size2D &dilation = Size2D(1U, 1U), const ActivationLayerInfo &act_info = ActivationLayerInfo(), bool enable_fast_math = false, unsigned int num_groups = 1);

    
    act_func.configure(&inputT, &kernelT, (bias!=nullptr && doBias) ? &biasT : nullptr,
                        &outputT, 
                        pad_stride_info, arm_compute::WeightsInfo(), dilation_2d, act_info);

  /*  act_func.configure(&inputT, &kernelT, nullptr,
                        &outputT, 
                        pad_stride_info); //, arm_compute::WeightsInfo(), dilation_2d, act_info);
*/                        


    inputT.allocator()->allocate();
    outputT.allocator()->allocate();
    kernelT.allocator()->allocate();                        

    if(bias!=nullptr && doBias) {
      biasT.allocator()->allocate();                      
      auto *bptr = reinterpret_cast<int8_t *>(biasT.buffer());
      memcpy(bptr,bias, KN*sizeof(float));
    }

    auto *iptr = reinterpret_cast<int8_t *>(inputT.buffer());
    auto *kptr = reinterpret_cast<int8_t *>(kernelT.buffer());
    auto *optr = reinterpret_cast<int8_t *>(outputT.buffer());

    memcpy(iptr,input, N*H*W*C*sizeof(float));
    memcpy(kptr,kernel, KN*KW*KH*C*sizeof(float));       

    act_func.run();

    memcpy(output, outputT.buffer(),KN*out_w*out_h*sizeof(float));

    StopCacheCounts(&cache_counts1);
  clock_t t1e = clock();
  //cout <<"conv(acl_f32) : "<<(double)(t1e- t1s)/CLOCKS_PER_SEC*1000 << std::endl;
  //PrintCacheCounts(cache_counts1);

#if 0
printf("Output kN=0\n")  ;
    for(int h=0;h<out_h;h++) {
      for(int w=0;w<out_w;w++) {
         printf("%.2f,",output[h*out_w*KN + w*KN]);
      }
      printf("\n");
    } 
    
#endif

  
}


// input nchw, output nchw
void fwdConvolution_nchw_v2(int8_t *input, int8_t *kernel, int8_t *bias, int8_t *output, 
                      float inS, float kS, float bS, float outS,
                      int32_t inOffset, int32_t filterOffset, int32_t biasOffset, int32_t outOffset,
                      dim_t N, dim_t C, dim_t H, dim_t W, dim_t KN, dim_t KH, dim_t KW, int pad_size, int stride_size, size_t group,
                      size_t dilation, bool doRelu, bool doBias, 
                      int out_h, int out_w) {

  // only support out_w is 4*n
  assert(out_w %4 ==0);
  
  float outScale = outS;
  float inScale = inS;
  float filterScale = kS;
  float biasScale = bS;

  // Calculate the scale of the values that come out of the matrix
  // multiplication part of the calculation.
    float matMulScale = inScale * filterScale;
    float scale = matMulScale / outScale;
    float bscale = biasScale / matMulScale;  

#if DEBUG_INFO
  printf("off i:%d f:%d b:%d o:%d\n",inOffset,  filterOffset,  biasOffset,  outOffset);

  printf("\nscale:%f\n", scale);
  printf("bscale:%f\n",bscale);
#endif


  int8_t* flat_input = (int8_t*)malloc (KH*KW*C* out_h * out_w); // c= channel
  //int8_t* kernel_vec = (int8_t*)malloc (KH*KW*C*KN); // n filter * channel
  int32_t* mm_out_temp = (int32_t*)malloc(out_h*out_w*sizeof(int32_t)); // output of 1 filter for all channel
  
  clock_t t1s = clock();
  
  // make patched input
  // For each convolution 'jump' in the input tensor:
  ssize_t x = -ssize_t(pad_size);
  for (dim_t ax = 0; ax < out_h; x += stride_size, ax++) {  //H
    ssize_t y = -ssize_t(pad_size);
    for (dim_t ay = 0; ay < out_w; y += stride_size, ay++) { 
       linearize_volume_nchw(input, flat_input + (ax * KH * KW * out_w * C) + (ay * KH * KW * C), x, y, KW, KH, C, W, H, 1, W, W * H, pad_size > 0 ? 1 : 0, 0, 1, 1, inOffset);
    
#if DEBUG_INFO_V     
      if(ax>4 && ay <4) 
      {
        for(int c=0;c<3;c++)
        {
        printf("\n(%d,%d - %d)[%d,%d]\n",x,y,c,KW,KH);
        //ch0�� ���. 
        for(int k=0;k<KW*KH;k++) { printf("%02x ",flat_input[k + (ax * KH * KW * out_w * C) + (ay * KH * KW * C)+ c*KH*KW]); if(k%7==6) printf("/ "); }

        }
      }
#endif      
      
    }
  }


  clock_t t1e = clock();
  cout <<"linearize: "<<(double)(t1e- t1s)/CLOCKS_PER_SEC*1000 << std::endl;

int cnt[4];
double m1[4];
double m2[4];
clock_t m1s[4]={};
clock_t m2s[4]={};
clock_t m3s[4]={};

for(int k=0;k<4;k++) { cnt[k]=0; m1[k]=0; m2[k]=0; }
  
  int kblock = 4; 
  int bblock = 4; // block = 4x KW*KN x KW*KN * 4
  
  
  clock_t t2s = clock();
  // For each output channel in the group:


 #pragma omp parallel for num_threads(4) //schedule(static) 
 for(dim_t b = 0; b < out_h * out_w; b+=8) { 
   for (dim_t d = 0; d < KN; d+=4) {
    
  // if(d==0) printf("\n");

   
    int pr=sched_getcpu();
    
       
    cnt[pr]++;

  CacheCounts CC;
  ArmPmuEvent __st_retired(ArmPmuEvent::ST_RETIRED);
  ArmPmuEvent __ld_retired(ArmPmuEvent::LD_RETIRED);
  ArmPmuEvent __mem_access(ArmPmuEvent::MEM_ACCESS);
  ArmPmuEvent __l1d_cache(ArmPmuEvent::L1D_CACHE);
  ArmPmuEvent __l1d_cache_refill(ArmPmuEvent::L1D_CACHE_REFILL);
  ArmPmuEvent __l2d_cache(ArmPmuEvent::L2D_CACHE);
  ArmPmuEvent __l2d_cache_refill(ArmPmuEvent::L2D_CACHE_REFILL);
  ArmPmuEvent __cpu_cycle(ArmPmuEvent::CPU_CYCLES);

  __st_retired.Start();
  __ld_retired.Start();
  __mem_access.Start();
  __l1d_cache.Start();
  __l1d_cache_refill.Start();
  __l2d_cache.Start();
  __l2d_cache_refill.Start();
  __cpu_cycle.Start();  

  
      const clock_t m1s = clock();
      gemm_s8_v2(kernel + d*KH*KW*C, flat_input + b*KH * KW * C, output+d*out_w+b, KH * KW * C, out_h * out_w, KH * KW * C, out_w, (int32_t*)bias, scale, doRelu,  filterOffset);

  const clock_t m2s = clock();
  m1[pr] += m2s-m1s;

  CC.st_retired = __st_retired.Stop();
  CC.ld_retired = __ld_retired.Stop();
  CC.mem_access = __mem_access.Stop();
  CC.l1d_cache = __l1d_cache.Stop();
  CC.l1d_cache_refill = __l1d_cache_refill.Stop();
  CC.l2d_cache = __l2d_cache.Stop();
  CC.l2d_cache_refill = __l2d_cache_refill.Stop();
  CC.cpu_cycle = __cpu_cycle.Stop();  

#pragma omp critical(print)  
{
  // printf("%d,",pr);
  //PrintCacheCountsRow(CC);      
}

#if DEBUG_INFO_V
    if(d<KN) 
    {
        
        for (int j = 0; j < 2; j++)
        {
            for (int i = 0; i < 2; i++)
            {
                printf("%d ",mm_out_temp[i + j * out_w]+ *((int32_t*)bias+d));

            }
            printf("\n");
        }
        
    }
#endif
 // clock_t m2s = clock();
    //vector_scale_bias_offset_clip_s8(mm_out_temp, *((int32_t*)bias+d), output  + (d*out_h*out_w) , scale, bscale, out_h * out_w , true, doRelu,outOffset);
  //  const clock_t m3s = clock();
  //m2[pr] += (m3s-m2s);
#if DEBUG_INFO_V
    if(d<KN) 
    {
        
        for (int j = 0; j < 2; j++)
        {
            for (int i = 0; i < 2; i++)
            {
                printf("(%d)",*(output  + (d*out_h*out_w) + i + j * out_w) );
                if(i % 16 == 15) printf(" / ");
            }
            printf("\n");
        }
        
    }
#endif    

  }
 }

  clock_t t2e = clock();
  cout <<"vm:  "<<(double)(t2e- t2s)/CLOCKS_PER_SEC*1000 << std::endl;
  //cout <<" (M1: "<< m1/CLOCKS_PER_SEC*1000 << " M2: "<< m2/CLOCKS_PER_SEC*1000 << ")" << std::endl;
for(int k=0;k<4;k++) { printf("[%d] (%d) %f %f \n", k, cnt[k], m1[k],m2[k]); }  

  free(flat_input);
  //free(kernel_vec);
  free(mm_out_temp);

 
  
}


// input nchw, output nchw
void fwdConvolution_nchw(int8_t *input, int8_t *kernel, int8_t *bias, int8_t *output,
                         float inS, float kS, float bS, float outS,
                         int32_t inOffset, int32_t filterOffset, int32_t biasOffset, int32_t outOffset,
                         dim_t N, dim_t C, dim_t H, dim_t W, dim_t KN, dim_t KH, dim_t KW, int pad_size, int stride_size, size_t group,
                         size_t dilation, bool doRelu, bool doBias,
                         int out_h, int out_w) {

  // only support out_w is 4*n
  assert(out_w %4 ==0);

  float outScale = outS;
  float inScale = inS;
  float filterScale = kS;
  float biasScale = bS;

  // Calculate the scale of the values that come out of the matrix
  // multiplication part of the calculation.
  float matMulScale = inScale * filterScale;
  float scale = matMulScale / outScale;
  float bscale = biasScale / matMulScale;

#if DEBUG_INFO
  printf("off i:%d f:%d b:%d o:%d\n",inOffset,  filterOffset,  biasOffset,  outOffset);

  printf("\nscale:%f\n", scale);
  printf("bscale:%f\n",bscale);
#endif


  int8_t* flat_input = (int8_t*)malloc (KH*KW*C* out_h * out_w); // c= channel
  int8_t* kernel_vec = (int8_t*)malloc (KH*KW*C); // 1 filter * channel
  int32_t* mm_out_temp = (int32_t*)malloc(out_h*out_w*sizeof(int32_t)); // output of 1 filter for all channel



  // make patched input
  // For each convolution 'jump' in the input tensor:
  ssize_t x = -ssize_t(pad_size);
  for (dim_t ax = 0; ax < out_h; x += stride_size, ax++) {  //H
    ssize_t y = -ssize_t(pad_size);
    for (dim_t ay = 0; ay < out_w; y += stride_size, ay++) {
      linearize_volume_nchw(input, flat_input + (ax * KH * KW * out_w * C) + (ay * KH * KW * C), x, y, KW, KH, C, W, H, 1, W, W * H, pad_size > 0 ? 1 : 0, 0, 1, 1, inOffset);

#if DEBUG_INFO
      if(ax>4 && ay <4)
      {
        for(int c=0;c<3;c++)
        {
        printf("\n(%d,%d - %d)[%d,%d]\n",x,y,c,KW,KH);
        //ch0�� ���.
        for(int k=0;k<KW*KH;k++) { printf("%02x ",flat_input[k + (ax * KH * KW * out_w * C) + (ay * KH * KW * C)+ c*KH*KW]); if(k%7==6) printf("/ "); }

        }
      }
#endif

    }
  }

  // For each output channel in the group:
  for (dim_t d = 0; d < KN; d++) {

    // load 1 filter * all channel
    memcpy(kernel_vec, kernel + KH * KW * C * d, KH * KW * C);

#if DEBUG_INFO
    printf("kernel %d\n",d);
      for(int c=0;c<C;c++)
      {
        for (int j = 0; j < KH; j++)
        {
          for (int i = 0; i < KW; i++)
            {
                printf("%x ",kernel_vec[i + j * KW + c*KH*KW]);
            }
          printf(" / ");
        }
        printf("\n");
      }
#endif

    vector_matrix_multiply_s8(kernel_vec, flat_input, (int32_t *)mm_out_temp, KH * KW * C, out_h * out_w, KH * KW * C, filterOffset);

#if DEBUG_INFO
    if(d<KN)
    {

        for (int j = 0; j < 2; j++)
        {
            for (int i = 0; i < 2; i++)
            {
                printf("%d ",mm_out_temp[i + j * out_w]+ *((int32_t*)bias+d));

            }
            printf("\n");
        }

    }
#endif

    vector_scale_bias_offset_clip_s8(mm_out_temp, *((int32_t*)bias+d), output  + (d*out_h*out_w) , scale, bscale, out_h * out_w , true, doRelu,outOffset);

#if DEBUG_INFO
    if(d<KN)
    {

        for (int j = 0; j < 2; j++)
        {
            for (int i = 0; i < 2; i++)
            {
                printf("(%d)",*(output  + (d*out_h*out_w) + i + j * out_w) );
                if(i % 16 == 15) printf(" / ");
            }
            printf("\n");
        }

    }
#endif

  }
  free(flat_input);
  free(kernel_vec);
  free(mm_out_temp);



}


// input nchw, output nchw
void fwdConvolution_nchw_mm(int8_t *input, int8_t *kernel, int8_t *bias, int8_t *output,
                            float inS, float kS, float bS, float outS,
                            int32_t inOffset, int32_t filterOffset, int32_t biasOffset, int32_t outOffset,
                            dim_t N, dim_t C, dim_t H, dim_t W, dim_t KN, dim_t KH, dim_t KW, int pad_size, int stride_size, size_t group,
                            size_t dilation, bool doRelu, bool doBias,
                            int out_h, int out_w) {

  // only support KN%4
  assert(KN%4 ==0);
  assert((out_w*out_w)%16==0);


  float outScale = outS;
  float inScale = inS;
  float filterScale = kS;
  float biasScale = bS;

  // Calculate the scale of the values that come out of the matrix
  // multiplication part of the calculation.
  float matMulScale = inScale * filterScale;
  float scale = matMulScale / outScale;
  float bscale = biasScale / matMulScale;


//#if DEBUG_INFO
#if 1
  printf("off i:%d f:%d b:%d o:%d\n",inOffset,  filterOffset,  biasOffset,  outOffset);

  printf("\nscale:%f\n", scale);
  printf("bscale:%f\n",bscale);
#endif


  int8_t* flat_input = (int8_t*)malloc (KH*KW*C* out_h * out_w); // c= channel
//  int8_t* kernel_vec = (int8_t*)malloc (KH*KW*C); // 1 filter * channel
  int32_t* mm_out_c = (int32_t*)malloc(C*KN*out_w*out_h*sizeof(int32_t)); // 채널별로 나눠서 저장 후 나중에 합침.



  // make patched input
  // For each convolution 'jump' in the input tensor:
  clock_t t1s = clock();




/*
   ssize_t x = -ssize_t(pad_size);
   for (dim_t ax = 0; ax < out_h; ax++, x += stride_size) {  //H
    ssize_t y = -ssize_t(pad_size);
    for (dim_t ay = 0; ay < out_w; y += stride_size, ay++) {
       linearize_volume_nchw(input, flat_input + (ax * KH * KW * out_w * C) + (ay * KH * KW * C), x, y, KW, KH, KN, W, H, 1, W, W * H, pad_size > 0 ? 1 : 0, 0, 1, 1, inOffset);

#if 1
      if(ax<4 && ay <4)
      {
        for(int c=0;c<3;c++)
        {
        printf("\n(%d,%d - %d)[%d,%d]\n",x,y,c,KW,KH);
        //ch0�� ���.
        for(int k=0;k<KW*KH;k++) { printf("%02x ",flat_input[k + (ax * KH * KW * out_w * C) + (ay * KH * KW * C)+ c*KH*KW]); if(k%7==6) printf("/ "); }

        }
      }
#endif

    }
  }

  const int stride = KH * KW * out_w * out_h;
  signed char* ret = (signed char*)flat_input;
  #pragma omp parallel for num_threads(4)
  for (int p = 0; p < C; p++)
  {
      const signed char* in = input + H*W*p;  // p channel
      int retID = stride * p;
      for (int u = 0; u < KH; u++)
      {
          for (int v = 0; v < KW; v++)
          {
              for (int i = 0; i < out_h; i++)
              {
                  for (int j = 0; j < out_w; j++)
                  {
                      int row = u + i * stride_size - pad_size;
                      int col = v + j * stride_size - pad_size;
                      if(row<0 || col<0 || row >= H || col >= W) ret[retID]=0;
                      else {
                        int index = row * W + col;
                        ret[retID] = input[index];

                      }
                      retID++;
                  }
              }
          }
      }
  }
*/

  const int stride_in_c = W*H;
  const int stride_out_c = KH * KW * out_w * out_h;
  signed char* ret = (signed char*)flat_input;

#pragma omp parallel for num_threads(3)
  for (int p = 0; p < C; p++)
  {
    int8_t *in = input + stride_in_c * p;
    int retID = stride_out_c * p;
    for (int i = 0; i < out_h; i++)
    {
      for (int j = 0; j < out_w; j++)
      {

        for (int u = 0; u < KH; u++)
        {
          for (int v = 0; v < KW; v++)
          {
            int row = u + i * stride_size - pad_size;
            int col = v + j * stride_size - pad_size;
            if(row<0 || col<0 || row >= H || col >= W) ret[retID]=0;
            else {
              int index = row * W + col;
              ret[retID] = in[index];
            }
            retID++;
          }
        }
      }
    }
  }


#if 0
  // 위에는 kchw인데 col-major 로 되어있음
for(int ay=0, x=-pad_size;ay<4;ay++,x+=stride_size) {
  for(int ax=0, y=-pad_size;ax<4;ax++,y+=stride_size) {
        for(int c=0;c<3;c++)
        {
       printf("\ncc(%d,%d - %d)[%d,%d]\n",x,y,c,KW,KH);
        //ch0�� ���.
        for(int k=0;k<KW*KH;k++) { printf("%02x ",flat_input[k + (ax * KH * KW) + (ay * KH * KW * out_w)+ c*KH*KW*out_w*out_h]); if(k%7==6) printf("/ "); }
        }

  }

}


#endif

  clock_t t1e = clock();
  cout <<"linearize: : "<<(double)(t1e- t1s)/CLOCKS_PER_SEC*1000 << std::endl;

  const int m = KN;
  const int n = out_w * out_h;
  const int k = C * KW * KH;


  if(filterOffset!=0) printf("!!!!! check filterOffset");
  clock_t t2s = clock();
/*
    // GEMM
    int32_t* pc = mm_out_c;
    const int8_t* pa = kernel;
    int8_t* pb = flat_input;
    const size_t ldc = out_w*out_h; //top_blob.cstep;


    int8kernel((void*)pc, pa, pb, m, k, n, ldc, 0, 0);

  #pragma omp parallel for num_threads(4)
  for (dim_t d = 0; d < KN; d++) {
      vector_scale_bias_offset_clip_s8(mm_out_c + d*out_w*out_h, *((int32_t*)bias+d), output  + (d*out_h*out_w) , scale, bscale, out_h * out_w , true, doRelu,outOffset);
  }

*/

  // For each output channel in the group:
  // row=4

#pragma omp parallel for num_threads(4)
  for(int c=0;c<C;c++) {
    for (dim_t d = 0; d < KN; d+=4) {

      mm_matrix_multiply_s8_v2(kernel + KH * KW * d + c*KH*KW*KN, flat_input + c*out_w*out_h*KW*KH, (int32_t *)mm_out_c + c*out_w*out_h, KH * KW, out_h * out_w, KH * KW, out_w, filterOffset);
      //mm_matrix_multiply_s8(kernel + KH * KW * d + c*KH*KW*KN, flat_input + c*out_w*out_h*KW*KH, (int32_t *)mm_out_c + c*out_w*out_h, KH * KW, out_h * out_w, KH * KW, filterOffset);
      //vector_scale_bias_offset_clip_s8(mm_out_c + d *out_w*out_h, *((int32_t*)bias+d), output  + (d*out_h*out_w) , scale, bscale, out_h * out_w , true, doRelu,outOffset);
      //vector_scale_bias_offset_clip_s8(mm_out_c + (d+1)*out_w*out_h, *((int32_t*)bias+d), output  + ((d+1)*out_h*out_w) , scale, bscale, out_h * out_w , true, doRelu,outOffset);
      //vector_scale_bias_offset_clip_s8(mm_out_c + (d+2)*out_w*out_h, *((int32_t*)bias+d), output  + ((d+2)*out_h*out_w) , scale, bscale, out_h * out_w , true, doRelu,outOffset);
      //vector_scale_bias_offset_clip_s8(mm_out_c + (d+3)*out_w*out_h, *((int32_t*)bias+d), output  + ((d+3)*out_h*out_w) , scale, bscale, out_h * out_w , true, doRelu,outOffset);

    }

/*
  #pragma omp parallel for num_threads(3)
  for(int c=0;c<C;c++) {
    for (dim_t d = 0; d < KN; d++) { //8=k_row

      mm_matrix_multiply_s8(kernel + KH * KW * d + c*KH*KW*KN, flat_input + c*out_w*out_h*KW*KH, (int32_t *)mm_out_c + c*out_w*out_h, KH * KW, out_h * out_w, KH * KW, filterOffset);
      //vector_scale_bias_offset_clip_s8(mm_out_c + d *out_w*out_h, *((int32_t*)bias+d), output  + (d*out_h*out_w) , scale, bscale, out_h * out_w , true, doRelu,outOffset);
      //vector_scale_bias_offset_clip_s8(mm_out_c + (d+1)*out_w*out_h, *((int32_t*)bias+d), output  + ((d+1)*out_h*out_w) , scale, bscale, out_h * out_w , true, doRelu,outOffset);
      //vector_scale_bias_offset_clip_s8(mm_out_c + (d+2)*out_w*out_h, *((int32_t*)bias+d), output  + ((d+2)*out_h*out_w) , scale, bscale, out_h * out_w , true, doRelu,outOffset);
      //vector_scale_bias_offset_clip_s8(mm_out_c + (d+3)*out_w*out_h, *((int32_t*)bias+d), output  + ((d+3)*out_h*out_w) , scale, bscale, out_h * out_w , true, doRelu,outOffset);

    }
*/


#if DEBUG_INFO
    printf("kernel %d\n",d);
      for(int c=0;c<C;c++)
      {
        for (int j = 0; j < KH; j++)
        {
          for (int i = 0; i < KW; i++)
            {
                printf("%x ",kernel_vec[i + j * KW + c*KH*KW]);
            }
          printf(" / ");
        }
        printf("\n");
      }
#endif

  }

  int32_t *inp_c[C];
  for (int c = 0; c < C; c++)
  {
    inp_c[c] = mm_out_c + c*out_w*out_h;
  }

#pragma omp parallel for num_threads(4)
  for (int i = 0; i < out_h; i++)
  {
    for (int j = 0; j < out_w; j++)
    {
      *inp_c[0] = *inp_c[0]+*inp_c[1]+*inp_c[2];
      inp_c[0]++;
      inp_c[1]++;
      inp_c[2]++;
    }
  }

#pragma omp parallel for num_threads(4)
  for(int c=0;c<C;c++) {
    for (dim_t d = 0; d < KN; d++) {

      vector_scale_bias_offset_clip_s8(mm_out_c + d *out_w*out_h, *((int32_t*)bias+d), output  + (d*out_h*out_w) , scale, bscale, out_h * out_w , true, doRelu,outOffset);
      //vector_scale_bias_offset_clip_s8(mm_out_c + (d+1)*out_w*out_h, *((int32_t*)bias+d), output  + ((d+1)*out_h*out_w) , scale, bscale, out_h * out_w , true, doRelu,outOffset);
      //vector_scale_bias_offset_clip_s8(mm_out_c + (d+2)*out_w*out_h, *((int32_t*)bias+d), output  + ((d+2)*out_h*out_w) , scale, bscale, out_h * out_w , true, doRelu,outOffset);
      //vector_scale_bias_offset_clip_s8(mm_out_c + (d+3)*out_w*out_h, *((int32_t*)bias+d), output  + ((d+3)*out_h*out_w) , scale, bscale, out_h * out_w , true, doRelu,outOffset);

    }
  }


  clock_t t2e = clock();
  cout <<"nn + sc: : "<<(double)(t2e- t2s)/CLOCKS_PER_SEC*1000 << std::endl;



#if DEBUG_INFO
  if(d<KN)
    {

        for (int j = 0; j < 2; j++)
        {
            for (int i = 0; i < 2; i++)
            {
                printf("%d ",mm_out_temp[i + j * out_w]+ *((int32_t*)bias+d));

            }
            printf("\n");
        }

    }
#endif



#if DEBUG_INFO
  if(d<KN)
    {

        for (int j = 0; j < 2; j++)
        {
            for (int i = 0; i < 2; i++)
            {
                printf("(%d)",*(output  + (d*out_h*out_w) + i + j * out_w) );
                if(i % 16 == 15) printf(" / ");
            }
            printf("\n");
        }

    }
#endif

  free( flat_input );
//
  free( mm_out_c);


}


#define DEBUG_CORE_PERF_MON
// input nchw, output nchw
// im2col + conv
void fwdConvolution_nchw_mm_v2(
    int8_t *input, int8_t *kernel, int8_t *bias, int8_t *output, float inS,
    float kS, float bS, float outS, int32_t inOffset, int32_t filterOffset,
    int32_t biasOffset, int32_t outOffset, dim_t N, dim_t C, dim_t H, dim_t W,
    dim_t KN, dim_t KH, dim_t KW, int pad_size, int stride_size, size_t group,
    size_t dilation, bool doRelu, bool doBias, int out_h, int out_w) {

  // only support KN%4
  assert(KN % 4 == 0);
  assert((out_w * out_w) % 16 == 0);

  float outScale = outS;
  float inScale = inS;
  float filterScale = kS;
  float biasScale = bS;

  // Calculate the scale of the values that come out of the matrix
  // multiplication part of the calculation.
  float matMulScale = inScale * filterScale;
  float scale = matMulScale / outScale;
  float bscale = biasScale / matMulScale;

//#if DEBUG_INFO
#if 1
  printf("off i:%d f:%d b:%d o:%d\n", inOffset, filterOffset, biasOffset,
         outOffset);

  printf("\nscale:%f\n", scale);
  printf("bscale:%f\n", bscale);
#endif

  int8_t *flat_input =
      (int8_t *)malloc(KH * KW * C * out_h * out_w); // c= channel
  //  int8_t* kernel_vec = (int8_t*)malloc (KH*KW*C); // 1 filter * channel
  int32_t *mm_out_c = (int32_t *)malloc(
      C * KN * out_w * out_h *
      sizeof(int32_t)); // 채널별로 나눠서 저장 후 나중에 합침.

  CacheCounts cache_counts[3];
  CacheCounts cache_counts2;

  // make patched input
  // For each convolution 'jump' in the input tensor:
  clock_t t1s = clock();

  clock_t t2s;

  int num = sysconf(_SC_NPROCESSORS_CONF);
  printf("core num:%d ---\n", num);

  // im2col
  const int stride_in_c = W * H;
  const int stride_out_c = KH * KW * out_w * out_h;
  signed char *ret = (signed char *)flat_input;

  omp_set_num_threads(4);
  //#pragma omp declare simd
  int32_t *output_ptr[3] = {mm_out_c, mm_out_c + out_h * out_w,
                            mm_out_c + 2 * out_h * out_w};

  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(1, &mask);

//#pragma omp parallel
  {
//#pragma omp for
#pragma omp parallel for num_threads(4)
    for (int c = 0; c < C; c++) {
#ifdef DEBUG_CORE_PERF_MON
      ArmPmuEvent st_retired(ArmPmuEvent::ST_RETIRED);
      ArmPmuEvent ld_retired(ArmPmuEvent::LD_RETIRED);
      ArmPmuEvent mem_access(only_reads ? ArmPmuEvent::MEM_ACCESS_RD
                                        : ArmPmuEvent::MEM_ACCESS);
      ArmPmuEvent ll_cache(only_reads ? ArmPmuEvent::LL_CACHE_RD
                                      : ArmPmuEvent::LL_CACHE);
      ArmPmuEvent ll_cache_miss(only_reads ? ArmPmuEvent::LL_CACHE_MISS_RD
                                           : ArmPmuEvent::LL_CACHE_MISS);
      ArmPmuEvent l1d_cache(only_reads ? ArmPmuEvent::L1D_CACHE_RD
                                       : ArmPmuEvent::L1D_CACHE);
      ArmPmuEvent l1d_cache_refill(only_reads ? ArmPmuEvent::L1D_CACHE_REFILL_RD
                                              : ArmPmuEvent::L1D_CACHE_REFILL);
      ArmPmuEvent l1d_tlb(only_reads ? ArmPmuEvent::L1D_TLB_RD
                                     : ArmPmuEvent::L1D_TLB);
      ArmPmuEvent l1d_tlb_refill(only_reads ? ArmPmuEvent::L1D_TLB_REFILL_RD
                                            : ArmPmuEvent::L1D_TLB_REFILL);
      ArmPmuEvent l2d_cache(only_reads ? ArmPmuEvent::L2D_CACHE_RD
                                       : ArmPmuEvent::L2D_CACHE);
      ArmPmuEvent l2d_cache_refill(only_reads ? ArmPmuEvent::L2D_CACHE_REFILL_RD
                                              : ArmPmuEvent::L2D_CACHE_REFILL);
      ArmPmuEvent l3d_cache(only_reads ? ArmPmuEvent::L3D_CACHE_RD
                                       : ArmPmuEvent::L3D_CACHE);
      ArmPmuEvent l3d_cache_refill(only_reads ? ArmPmuEvent::L3D_CACHE_REFILL_RD
                                              : ArmPmuEvent::L3D_CACHE_REFILL);
      ArmPmuEvent cpu_l3d_cache_refill(only_reads
                                           ? ArmPmuEvent::L3D_CACHE_REFILL_RD
                                           : ArmPmuEvent::L3D_CACHE_REFILL);
      ArmPmuEvent cpu_cycle(ArmPmuEvent::CPU_CYCLES);

      st_retired.Start();
      ld_retired.Start();
      mem_access.Start();
      ll_cache.Start();
      ll_cache_miss.Start();
      l1d_cache.Start();
      l1d_cache_refill.Start();
      l1d_tlb.Start();
      l1d_tlb_refill.Start();

      l2d_cache.Start();
      l2d_cache_refill.Start();
      l3d_cache.Start();
      l3d_cache_refill.Start();

      cpu_cycle.Start();
#endif

      cpu_set_t a;
      printf("%d %d %d %d => %d >  ", CPU_ISSET(0, &a), CPU_ISSET(1, &a),
             CPU_ISSET(2, &a), CPU_ISSET(3, &a), sched_getcpu());

      printf("omp_get_num_threads = %d. tid=%d\n", omp_get_num_threads(),
             omp_get_thread_num());

      int8_t *in = input + stride_in_c * c;
      int retID = stride_out_c * c;
      for (int i = 0; i < out_h; i++) {
        for (int j = 0; j < out_w; j++) {
          for (int u = 0; u < KH; u++) {
            for (int v = 0; v < KW; v++) {
              int row = u + i * stride_size - pad_size;
              int col = v + j * stride_size - pad_size;
              if (row < 0 || col < 0 || row >= H || col >= W)
                ret[retID] = 0;
              else {
                int index = row * W + col;
                ret[retID] = in[index];
  }
              retID++;
            }
          }

          if (j % 4 == 0 && j != 0) {
           //  printf("(core:%d) t:%d C:%d H:%d I:%d \n", sched_getcpu(), omp_get_thread_num(), c, j, i);

            for (dim_t d = 0; d < KN; d += 4) {
              mm_matrix_multiply_s8_v3(kernel + KH * KW * d + c * KH * KW * KN,
                                       flat_input + c * out_w * out_h * KW * KH,
                                       (int32_t *)output_ptr[c], KH * KW,
                                       i * out_w + j, KH * KW, out_w,
                                       filterOffset);
            }
          }

          clock_t t1e = clock();
           // printf("--- t:%d C:%d %f \n", omp_get_thread_num(), c, (double)(t1e- t1s)/CLOCKS_PER_SEC*1000);

        } // end j
      }

#ifdef DEBUG_CORE_PERF_MON
      cache_counts[c].st_retired = st_retired.Stop();
      cache_counts[c].ld_retired = ld_retired.Stop();
      cache_counts[c].mem_access = mem_access.Stop();
      cache_counts[c].ll_cache = ll_cache.Stop();
      cache_counts[c].ll_cache_miss = ll_cache_miss.Stop();
      cache_counts[c].l1d_cache = l1d_cache.Stop();
      cache_counts[c].l1d_cache_refill = l1d_cache_refill.Stop();
      cache_counts[c].l1d_tlb = l1d_tlb.Stop();
      cache_counts[c].l1d_tlb_refill = l1d_tlb_refill.Stop();
      cache_counts[c].l2d_cache = l2d_cache.Stop();
      cache_counts[c].l2d_cache_refill = l2d_cache_refill.Stop();
      cache_counts[c].l3d_cache = l3d_cache.Stop();
      cache_counts[c].l3d_cache_refill = l3d_cache_refill.Stop();
      cache_counts[c].cpu_cycle = cpu_cycle.Stop();
      //printf("core=%d\n", sched_getcpu());
      //PrintCacheCounts(cache_counts[c]);
#endif
    } // end of channel
  }

  //    clock_t t1e = clock();
  // printf("t:%d C:%d %f \n", omp_get_thread_num(), c, (double)(t1e-
  // t1s)/CLOCKS_PER_SEC*1000);

  clock_t t1e = clock();
  cout << "linearize: : " << (double)(t1e - t1s) / CLOCKS_PER_SEC * 1000
       << std::endl;
  StartCacheCounts();
  t2s = clock();

  // For each output channel in the group:
  // row=4

  // mm_matrix_multiply_s8_v2_memory(kernel, flat_input, (int32_t *)mm_out_c, KH
  // * KW, out_h * out_w, KH * KW, out_w, filterOffset);

  //  #pragma omp parallel for num_threads(4)
  //  for(int c=0;c<C;c++) {
  // for (dim_t d = 0; d < KN; d+=4) {

  //    mm_matrix_multiply_s8_v2(kernel + KH * KW * d + c*KH*KW*KN, flat_input +
  //    c*out_w*out_h*KW*KH, (int32_t *)mm_out_c + c*out_w*out_h, KH * KW, out_h
  //    * out_w, KH * KW, out_w, filterOffset);
  // mm_matrix_multiply_s8_v2_memory(kernel + KH * KW * d + c*KH*KW*KN,
  // flat_input + c*out_w*out_h*KW*KH, (int32_t *)mm_out_c + c*out_w*out_h, KH *
  // KW, out_h * out_w, KH * KW, out_w, filterOffset);
  // mm_matrix_multiply_s8(kernel + KH * KW * d + c*KH*KW*KN, flat_input +
  // c*out_w*out_h*KW*KH, (int32_t *)mm_out_c + c*out_w*out_h, KH * KW, out_h *
  // out_w, KH * KW, filterOffset); vector_scale_bias_offset_clip_s8(mm_out_c +
  // d *out_w*out_h, *((int32_t*)bias+d), output  + (d*out_h*out_w) , scale,
  // bscale, out_h * out_w , true, doRelu,outOffset);
  // vector_scale_bias_offset_clip_s8(mm_out_c + (d+1)*out_w*out_h,
  // *((int32_t*)bias+d), output  + ((d+1)*out_h*out_w) , scale, bscale, out_h *
  // out_w , true, doRelu,outOffset); vector_scale_bias_offset_clip_s8(mm_out_c
  // + (d+2)*out_w*out_h, *((int32_t*)bias+d), output  + ((d+2)*out_h*out_w) ,
  // scale, bscale, out_h * out_w , true, doRelu,outOffset);
  // vector_scale_bias_offset_clip_s8(mm_out_c + (d+3)*out_w*out_h,
  // *((int32_t*)bias+d), output  + ((d+3)*out_h*out_w) , scale, bscale, out_h *
  // out_w , true, doRelu,outOffset);

  //}

  // }

  /*
  int32_t *inp_c[C];
  for (int c = 0; c < C; c++)
  {
    inp_c[c] = mm_out_c + c*out_w*out_h;
  }

#pragma omp parallel for num_threads(4)
  for (int i = 0; i < out_h; i++)
  {
    for (int j = 0; j < out_w; j++)
    {
      *inp_c[0] = *inp_c[0]+*inp_c[1]+*inp_c[2];
      inp_c[0]++;
      inp_c[1]++;
      inp_c[2]++;
    }
  }

#pragma omp parallel for num_threads(4)
  for(int c=0;c<C;c++) {
    for (dim_t d = 0; d < KN; d++) {

        vector_scale_bias_offset_clip_s8(mm_out_c + d *out_w*out_h,
    *((int32_t*)bias+d), output  + (d*out_h*out_w) , scale, bscale, out_h *
    out_w , true, doRelu,outOffset);
        //vector_scale_bias_offset_clip_s8(mm_out_c + (d+1)*out_w*out_h,
    *((int32_t*)bias+d), output  + ((d+1)*out_h*out_w) , scale, bscale, out_h *
    out_w , true, doRelu,outOffset);
        //vector_scale_bias_offset_clip_s8(mm_out_c + (d+2)*out_w*out_h,
    *((int32_t*)bias+d), output  + ((d+2)*out_h*out_w) , scale, bscale, out_h *
    out_w , true, doRelu,outOffset);
        //vector_scale_bias_offset_clip_s8(mm_out_c + (d+3)*out_w*out_h,
    *((int32_t*)bias+d), output  + ((d+3)*out_h*out_w) , scale, bscale, out_h *
    out_w , true, doRelu,outOffset);

    }
  }
  */

  clock_t t2e = clock();
  StopCacheCounts(&cache_counts2);
  cout << " nn + sc: : " << (double)(t2e - t2s) / CLOCKS_PER_SEC * 1000
       << std::endl;

  printf("----\n");
  //PrintCacheCounts(cache_counts2);

#if DEBUG_INFO
  if (d < KN) {

    for (int j = 0; j < 2; j++) {
      for (int i = 0; i < 2; i++) {
        printf("%d ", mm_out_temp[i + j * out_w] + *((int32_t *)bias + d));
            }
            printf("\n");
        }
  }
#endif

#if DEBUG_INFO
  if (d < KN) {

    for (int j = 0; j < 2; j++) {
      for (int i = 0; i < 2; i++) {
        printf("(%d)", *(output + (d * out_h * out_w) + i + j * out_w));
        if (i % 16 == 15)
          printf(" / ");
            }
            printf("\n");
        }
    }
#endif
}

typedef struct _param {
  int8_t *input;
  int8_t *kernel;
  int8_t *output;
  int out_h;
  int out_w;
  int KH;
  int KN;
  int KW;
  int stride_size;
  int pad_size;
  int N;
  int W;
  int H;
  int C;
  int stride_in_c;
  int stride_out_c;
  int c; // current channel
  int32_t filterOffset;
} param;

void *channel_conv_th(void *pp) {
  param *p = (param *)pp;
  int8_t *flat_input =
      (int8_t *)malloc(p->KH * p->KW * p->out_h * p->out_w); // c= channel
  int32_t *mm_out_c = (int32_t *)malloc(
      p->KN * p->out_w * p->out_h *
      sizeof(int32_t)); // 채널별로 나눠서 저장 후 나중에 합침.
  int8_t *in = p->input + p->stride_in_c * p->c;

  int retID = p->stride_out_c * p->c;

  cpu_set_t m;
  CPU_ZERO(&m);

  if (p->c == 0) {
    CPU_SET(1, &m);
    pthread_setaffinity_np(pthread_self(), sizeof(m), (cpu_set_t *)&m);
  } else if (p->c == 1) {
    CPU_SET(2, &m);
    pthread_setaffinity_np(pthread_self(), sizeof(m), (cpu_set_t *)&m);
  } else if (p->c == 2) {
    CPU_SET(3, &m);
    pthread_setaffinity_np(pthread_self(), sizeof(m), (cpu_set_t *)&m);
    }

#ifdef DEBUG_CORE_PERF_MON
  ArmPmuEvent st_retired(ArmPmuEvent::ST_RETIRED);
  ArmPmuEvent ld_retired(ArmPmuEvent::LD_RETIRED);
  ArmPmuEvent mem_access(only_reads ? ArmPmuEvent::MEM_ACCESS_RD
                                    : ArmPmuEvent::MEM_ACCESS);
  ArmPmuEvent ll_cache(only_reads ? ArmPmuEvent::LL_CACHE_RD
                                  : ArmPmuEvent::LL_CACHE);
  ArmPmuEvent ll_cache_miss(only_reads ? ArmPmuEvent::LL_CACHE_MISS_RD
                                       : ArmPmuEvent::LL_CACHE_MISS);
  ArmPmuEvent l1d_cache(only_reads ? ArmPmuEvent::L1D_CACHE_RD
                                   : ArmPmuEvent::L1D_CACHE);
  ArmPmuEvent l1d_cache_refill(only_reads ? ArmPmuEvent::L1D_CACHE_REFILL_RD
                                          : ArmPmuEvent::L1D_CACHE_REFILL);
  ArmPmuEvent l1d_tlb(only_reads ? ArmPmuEvent::L1D_TLB_RD
                                 : ArmPmuEvent::L1D_TLB);
  ArmPmuEvent l1d_tlb_refill(only_reads ? ArmPmuEvent::L1D_TLB_REFILL_RD
                                        : ArmPmuEvent::L1D_TLB_REFILL);
  ArmPmuEvent l2d_cache(only_reads ? ArmPmuEvent::L2D_CACHE_RD
                                   : ArmPmuEvent::L2D_CACHE);
  ArmPmuEvent l2d_cache_refill(only_reads ? ArmPmuEvent::L2D_CACHE_REFILL_RD
                                          : ArmPmuEvent::L2D_CACHE_REFILL);
  ArmPmuEvent l3d_cache(only_reads ? ArmPmuEvent::L3D_CACHE_RD
                                   : ArmPmuEvent::L3D_CACHE);
  ArmPmuEvent l3d_cache_refill(only_reads ? ArmPmuEvent::L3D_CACHE_REFILL_RD
                                          : ArmPmuEvent::L3D_CACHE_REFILL);
  ArmPmuEvent cpu_l3d_cache_refill(only_reads ? ArmPmuEvent::L3D_CACHE_REFILL_RD
                                              : ArmPmuEvent::L3D_CACHE_REFILL);
  ArmPmuEvent cpu_cycle(ArmPmuEvent::CPU_CYCLES);

  st_retired.Start();
  ld_retired.Start();
  mem_access.Start();
  ll_cache.Start();
  ll_cache_miss.Start();
  l1d_cache.Start();
  l1d_cache_refill.Start();
  l1d_tlb.Start();
  l1d_tlb_refill.Start();

  l2d_cache.Start();
  l2d_cache_refill.Start();
  l3d_cache.Start();
  l3d_cache_refill.Start();

  cpu_cycle.Start();
#endif
  clock_t t1s = clock();

  for (int i = 0; i < p->out_h; i++) {
    for (int j = 0; j < p->out_w; j++) {
      for (int u = 0; u < p->KH; u++) {
        for (int v = 0; v < p->KW; v++) {
          int row = u + i * p->stride_size - p->pad_size;
          int col = v + j * p->stride_size - p->pad_size;
          if (row < 0 || col < 0 || row >= p->H || col >= p->W)
            flat_input[retID] = 0;
          else {
            int index = row * p->W + col;
            flat_input[retID] = in[index];
        }
          retID++;
        }
      }

      if (j % 4 == 0 && j != 0) {
         printf("(core:%d) C:%d H:%d I:%d \n", sched_getcpu(), p->c, j, i);

        for (dim_t d = 0; d < p->KN; d += 4) {
          mm_matrix_multiply_s8_v3(p->kernel + p->KH * p->KW * d, flat_input,
                                   (int32_t *)mm_out_c, p->KH * p->KW,
                                   i * p->out_w + j, p->KH * p->KW, p->out_w,
                                   p->filterOffset);
    }
      }

       clock_t t1e = clock();
       printf("--- t:%d C:%d %f \n", omp_get_thread_num(), p->c, (double)(t1e- t1s)/CLOCKS_PER_SEC*1000);

    } // end j
        }

#ifdef DEBUG_CORE_PERF_MON

  CacheCounts cache_counts;
  cache_counts.st_retired = st_retired.Stop();
  cache_counts.ld_retired = ld_retired.Stop();
  cache_counts.mem_access = mem_access.Stop();
  cache_counts.ll_cache = ll_cache.Stop();
  cache_counts.ll_cache_miss = ll_cache_miss.Stop();
  cache_counts.l1d_cache = l1d_cache.Stop();
  cache_counts.l1d_cache_refill = l1d_cache_refill.Stop();
  cache_counts.l1d_tlb = l1d_tlb.Stop();
  cache_counts.l1d_tlb_refill = l1d_tlb_refill.Stop();
  cache_counts.l2d_cache = l2d_cache.Stop();
  cache_counts.l2d_cache_refill = l2d_cache_refill.Stop();
  cache_counts.l3d_cache = l3d_cache.Stop();
  cache_counts.l3d_cache_refill = l3d_cache_refill.Stop();
  cache_counts.cpu_cycle = cpu_cycle.Stop();
  printf("core=%d\n", sched_getcpu());
  //PrintCacheCounts(cache_counts);
#endif
  clock_t t1e = clock();
  printf("--- C:%d %f \n", p->c, (double)(t1e - t1s) / CLOCKS_PER_SEC * 1000);
  free(flat_input);
  free(mm_out_c);
    }

// input nchw, output nchw
// im2col + conv
void fwdConvolution_nchw_mm_v3(
    int8_t *input, int8_t *kernel, int8_t *bias, int8_t *output, float inS,
    float kS, float bS, float outS, int32_t inOffset, int32_t filterOffset,
    int32_t biasOffset, int32_t outOffset, dim_t N, dim_t C, dim_t H, dim_t W,
    dim_t KN, dim_t KH, dim_t KW, int pad_size, int stride_size, size_t group,
    size_t dilation, bool doRelu, bool doBias, int out_h, int out_w) {

  // only support KN%4
  assert(KN % 4 == 0);
  assert((out_w * out_w) % 16 == 0);

  float outScale = outS;
  float inScale = inS;
  float filterScale = kS;
  float biasScale = bS;

  // Calculate the scale of the values that come out of the matrix
  // multiplication part of the calculation.
  float matMulScale = inScale * filterScale;
  float scale = matMulScale / outScale;
  float bscale = biasScale / matMulScale;

//#if DEBUG_INFO
#if 1
  printf("off i:%d f:%d b:%d o:%d\n", inOffset, filterOffset, biasOffset,
         outOffset);

  printf("\nscale:%f\n", scale);
  printf("bscale:%f\n", bscale);
#endif

  //  int8_t* kernel_vec = (int8_t*)malloc (KH*KW*C); // 1 filter * channel

  CacheCounts cache_counts[3];
  CacheCounts cache_counts2;

  // make patched input
  // For each convolution 'jump' in the input tensor:
  clock_t t1s = clock();

  clock_t t2s;

  int num = sysconf(_SC_NPROCESSORS_CONF);
  printf("core num:%d ---\n", num);

  omp_set_num_threads(4);

  param p[3];
  for (int c = 0; c < 3; c++) {
    p[c].input = input + W * H * c;
    p[c].kernel = kernel + c * KW * KH * KN;
    p[c].output = output + c * out_w * out_h;
    p[c].out_h = out_h;
    p[c].out_w = out_w;
    p[c].KH = KH;
    p[c].KN = KN;
    p[c].KW = KW;
    p[c].stride_size = stride_size;
    p[c].pad_size = pad_size;
    p[c].N = N;
    p[c].W = W;
    p[c].H = H;
    p[c].C = C;
    p[c].stride_in_c = W * H;
    p[c].stride_out_c = KH * KW * out_w * out_h;
    p[c].c = c; // current channel
    p[c].filterOffset = filterOffset;
}

  cpu_set_t mask;
  CPU_ZERO(&mask);

  pthread_t p_thread[3];
  void *pRtn;
  pthread_create(&p_thread[0], NULL, channel_conv_th, (void *)&p[0]);
  pthread_create(&p_thread[1], NULL, channel_conv_th, (void *)&p[1]);
  pthread_create(&p_thread[2], NULL, channel_conv_th, (void *)&p[2]);

  pthread_join(p_thread[0], (void **)pRtn);
  pthread_join(p_thread[1], (void **)pRtn);
  pthread_join(p_thread[2], (void **)pRtn);

  clock_t t1e = clock();
  cout << "linearize: : " << (double)(t1e - t1s) / CLOCKS_PER_SEC * 1000
       << std::endl;
}



int cpuvtaconvolution(int8_t* input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale, int32_t kernelOffset,
                      int8_t *bias, float biasScale, int32_t biasOffset, int8_t *output, float outputScale, int32_t outputOffset, dim_t N, dim_t H, dim_t W, dim_t C, dim_t KN, dim_t KH, dim_t KW, int pad_size,
                      int stride_size, size_t group, size_t dilation, bool doRelu, bool doBias, int out_h, int out_w) {
  dim_t inCperG = C / group;
  dim_t outCperG = KN / group;
  float matMulScale = inputScale * kernelScale;
  for (dim_t n = 0; n < N; n++) {
    // For each group of input channels:
    for (dim_t g = 0; g < group; g++) {

      // For each output channel in the group:
      for (dim_t d = g * outCperG; d < (g + 1) * outCperG; d++) {

        // For each convolution 'jump' in the input tensor:
        ssize_t x = -ssize_t(pad_size);
        for (dim_t ax = 0; ax < out_h; x += stride_size, ax++) {
          ssize_t y = -ssize_t(pad_size);
          for (dim_t ay = 0; ay < out_w; y += stride_size, ay++) {

            // For each element in the convolution-filter:
            int32_t sum = 0;
            for (dim_t fx = 0; fx < KH; fx++) {
              for (dim_t fy = 0; fy < KW; fy++) {
                int64_t ox = x + fx * dilation;
                int64_t oy = y + fy * dilation;

                // Ignore index access below zero (this is due to padding).
                if (ox < 0 || oy < 0 || ox >= ssize_t(H) ||
                    oy >= int64_t(W)) {
                  continue;
                }
                for (dim_t fd = 0; fd < inCperG; fd++) {

                  int32_t F = (int32_t)*(kernel+d*KH*KW*inCperG + fx*KW*inCperG + fy*inCperG + fd);
                  int32_t I = (int32_t)*(input+n*H*W*C + ox*W*C + oy*C+ (g*inCperG +fd));
                  // We represent the element multiplication with offset as
                  // (value - offset).
                  sum += (F - kernelOffset) * (I - inputOffset);
                }
              }
            }

            // Scale the bias to match the scale of the matrix multiplication.
            int32_t B =std::round(float(*((int32_t *)bias+d) - biasOffset) *
                                  (biasScale / matMulScale));

            // Add the bias.
            sum += B;

            if(doRelu && sum<0){
              sum = 0;
            }
            // Scale the result back to the expected destination scale.
            auto out32 = std::round(float(sum) * (matMulScale / outputScale) + outputOffset);
            int8_t out8;
            if(out32>127) out8=127;
            else if(out32<-128) out8=-128;
            else out8=(int8_t)out32;
            *(output + n*out_h*out_w*KN + ax*out_w*KN + ay*KN + d) = out8;
          } // W
        }   // H
      }     // C
    }       // G
  }         // NS
  return 0;

}


int nonvtaconvolution_f32(float* input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale, int32_t kernelOffset,
                      float *bias, float biasScale, int32_t biasOffset, float *output, float outputScale, int32_t outputOffset, dim_t N, dim_t H, dim_t W, dim_t C, dim_t KN, dim_t KH, dim_t KW, int pad_size,
                      int stride_size, size_t group, size_t dilation, bool doRelu, bool doBias, int out_h, int out_w) {


  // input NHWC
  // kernel CHWN
  //float *kernel_f32 = (float*)malloc(C*KN*KW*KN*sizeof(float));
  //for(int k=0;k<C*KN*KW*KN;k++) kernel_f32[k]=(float)kernel[k]*kernelScale;

  fwdConvolution_nhwc_acl_f32(input, (float*)kernel, bias,output,
                       inputScale, kernelScale, biasScale, outputScale, 0,0,0,0,
                        N,C,H,W, KN, KH, KW,  pad_size, stride_size, 1,
                        dilation, doRelu, doBias, out_h, out_w);

  //  free(kernel_f32);

  // return NCHW format


   return 0;
}


#ifdef CPU_CONV_DBG
int nonvtaconvolution(int8_t* input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale, int32_t kernelOffset,
                      int8_t *bias, float biasScale, int32_t biasOffset, int8_t *output, float outputScale, int32_t outputOffset, dim_t N, dim_t H, dim_t W, dim_t C, dim_t KN, dim_t KH, dim_t KW, int pad_size,
                      int stride_size, size_t group, size_t dilation, bool doRelu, bool doBias, int out_h, int out_w)
{

  return cpuvtaconvolution(input, inputScale, inputOffset, kernel, kernelScale, kernelOffset,
                           bias, biasScale, biasOffset, output, outputScale, outputOffset, N, H, W, C, KN, KH, KW, pad_size,
                           stride_size, group, dilation, doRelu, doBias, out_h, out_w);
}
#else
int nonvtaconvolution(int8_t* input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale, int32_t kernelOffset,
                      int8_t *bias, float biasScale, int32_t biasOffset, int8_t *output, float outputScale, int32_t outputOffset, dim_t N, dim_t H, dim_t W, dim_t C, dim_t KN, dim_t KH, dim_t KW, int pad_size,
                      int stride_size, size_t group, size_t dilation, bool doRelu, bool doBias, int out_h, int out_w) {



//#define NCHW_CONV_S8
#ifdef NCHW_CONV_S8

    printf("-- fwdVTAConv --: NHWC:%d,%d,%d,%d pad:%d, stride:%d scale i:%f o:%f b:%f f:%f, C:%d KH:%d KW:%d KN:%d \n",N,H,W,C,pad_size, stride_size, inputScale,outputScale,biasScale, kernelScale,C,KH,KW,KN);

    // convert to nchw
    int8_t* input_nchw = (int8_t*)malloc(N*H*W*C);
    int8_t* k_nchw = (int8_t*)malloc(C*KH*KW*KN);
    int8_t* output_nchw = (int8_t*)malloc(N*out_w*out_h*KN);

    if(C==3 && W%16==0) {
      nhwc_to_nchw_s8_c3_w16n(input,input_nchw,N,H,W,C);
      printf("channel 3 => nhwc_to_nchw_s8_c3\n");
    } else {
      nhwc_to_nchw_s8(input,input_nchw,N,H,W,C);
      printf("nhwc_to_nchw_s8\n");
    }

    //TODO change to neon
    // kernel nhwc -> nchw
    #pragma omp parallel for num_threads(4)
    for(int kn=0;kn<KN;kn++)
    {
      for(int j=0;j<KH;j++)
      {
        for(int i=0;i<KW;i++)
        {
          for(int c=0;c<C;c++)
          {
            k_nchw[kn*KH*KW*C + c*KW*KH + j*KW + i ] = kernel[ kn*KH*KW*C + j*KW*C + i*C + c];
          }
        }
      }
    }



    fwdConvolution_nchw_acl(input_nchw, k_nchw, bias,output_nchw,
                inputScale, kernelScale, biasScale, outputScale, inputOffset, kernelOffset, biasOffset, outputOffset,
                                       N,C,H,W, KN, KH, KW,  pad_size, stride_size, 1,
                                       dilation, doRelu, doBias, out_h, out_w);



    //TODO change to neon
    // output nchw -> nhwc && row-major <-> column-major
    #pragma omp parallel for num_threads(4)
    for(int n=0;n<N;n++)
    {
      for(int j=0;j<out_h;j++)
      {
        for(int i=0;i<out_w;i++)
        {
          for(int kn=0;kn<KN;kn++)
          {

            output[n*out_w*out_h*KN + j*out_w*KN + i*KN + kn] = output_nchw[ n*out_w*out_h*KN + kn*out_w*out_h + j*out_w + i];
          }
        }
      }
    }


    free(input_nchw);
    free(k_nchw);
    free(output_nchw);


  return 0;

#else //nhwc

    fwdConvolution_nhwc_acl(input, kernel, bias,output,
                        inputScale, kernelScale, biasScale, outputScale, inputOffset, kernelOffset, biasOffset, outputOffset,
                        N,C,H,W, KN, KH, KW,  pad_size, stride_size, 1,
                        dilation, doRelu, doBias, out_h, out_w);

  return 0;

#endif // end nhwc

}
#endif

int fullyconnected_acl(int8_t* input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale, int32_t kernelOffset,
                   int8_t *bias, float biasScale, int32_t biasOffset, int8_t *output, float outputScale, int32_t outputOffset,
                   dim_t inputDim0, dim_t inputDim1, dim_t kernelDim0, dim_t kernelDim1, dim_t outputDim0, dim_t outputDim1, bool doBias) {
#if 0
   printf("fullly: in(%d.%d) is:%f io:%d -- kernel(%d,%d) ks:%f ko:%d -- out(%d,%d) os:%f oo:%d , doBais:%d bs:%f bo:%d\n",
   inputDim0, inputDim1, inputScale, inputOffset, kernelDim0, kernelDim1, kernelScale, kernelOffset, outputDim0, outputDim1, outputScale, outputOffset, doBias,biasScale,biasOffset);
#endif 

    const arm_compute::DataType            data_type = arm_compute::DataType::QASYMM8_SIGNED;

    // Create tensor
    // input NHWC. 
    const arm_compute::TensorShape         src_shape     =  arm_compute::TensorShape(inputDim1,inputDim0);
    const arm_compute::TensorInfo info(src_shape, 1, data_type);
    arm_compute::Tensor           inputT{};
    inputT.allocator()->init(info); //, required_alignment);


    const arm_compute::TensorShape         shape2     =  arm_compute::TensorShape(kernelDim1, kernelDim0);
    const arm_compute::TensorInfo info2(shape2, 1, data_type);
    arm_compute::Tensor           weightT{};
    weightT.allocator()->init(info2); //, required_alignment);

    // kernelDim1 = num of lables? out, biase is shape of num lables
    const arm_compute::TensorShape         shape3     =  arm_compute::TensorShape(outputDim1,outputDim0);
    const arm_compute::TensorInfo info3(shape3, 1, data_type);
    arm_compute::Tensor           outT{};
    outT.allocator()->init(info3); //, required_alignment);    

    arm_compute::Tensor           biasT{};
    if(bias!=nullptr && doBias) {
      const arm_compute::TensorShape         shape4     =  arm_compute::TensorShape(outputDim1);
      const arm_compute::TensorInfo info4(shape4, 1, arm_compute::DataType::S32);
      biasT.allocator()->init(info4); //, required_alignment);
      //biasT.info()->set_quantization_info(QuantizationInfo(bS, 0)); 
    }    


    inputT.info()->set_quantization_info(QuantizationInfo(inputScale, inputOffset));  // scale, offset
    weightT.info()->set_quantization_info(QuantizationInfo(kernelScale, kernelOffset));  // scale, offset
    outT.info()->set_quantization_info(QuantizationInfo(outputScale, outputOffset));  // scale, offset


    arm_compute::NEFullyConnectedLayer act_func;
    FullyConnectedLayerInfo fc_info;
    fc_info.transpose_weights    = false;
    fc_info.are_weights_reshaped = false;    

    act_func.configure(&inputT, &weightT, (bias!=nullptr && doBias) ? &biasT : nullptr, &outT,fc_info);

    inputT.allocator()->allocate();
    weightT.allocator()->allocate();
    outT.allocator()->allocate();

    auto *iptr = reinterpret_cast<int8_t *>(inputT.buffer());
    auto *wptr = reinterpret_cast<int8_t *>(weightT.buffer());
    auto *optr = reinterpret_cast<int8_t *>(outT.buffer());

    memcpy(iptr,input, inputDim0 * inputDim0 );
    memcpy(wptr,kernel, kernelDim0 * kernelDim1 );

    if(bias!=nullptr && doBias) {
      biasT.allocator()->allocate();                      
      auto *bptr = reinterpret_cast<int8_t *>(biasT.buffer());
      memcpy(bptr,bias, outputDim1 * sizeof(int32_t));
    }
   

    act_func.run();

    memcpy(output,optr, outputDim0 * outputDim1 );

  return 0;
}


int fullyconnected_cpu(int8_t* input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale, int32_t kernelOffset,
                   int8_t *bias, float biasScale, int32_t biasOffset, int8_t *output, float outputScale, int32_t outputOffset,
                   dim_t inputDim0, dim_t inputDim1, dim_t kernelDim0, dim_t kernelDim1, dim_t outputDim0, dim_t outputDim1, bool doBias) {
#if 0
  printf("fullly: in(%d.%d) is:%f io:%d -- kernel(%d,%d) ks:%f ko:%d -- out(%d,%d) os:%f oo:%d , doBais:%d bs:%f bo:%d\n",
   inputDim0, inputDim1, inputScale, inputOffset, kernelDim0, kernelDim1, kernelScale, kernelOffset, outputDim0, outputDim1, outputScale, outputOffset, doBias,biasScale,biasOffset);
#endif

  if(inputDim1 != kernelDim0 || inputDim1%16 !=0 ||  kernelDim1%4 !=0) {
    printf("[ERROR] invalid layout\n");
    return -1;
  }

  // layout: input = row-major, kernel => row-major. convrt row-major to column-major
  if(inputDim0 == 1 )
  {
    int8_t* K = (int8_t*)malloc( inputDim1 * kernelDim1);
    int32_t* C = (int32_t*)malloc(kernelDim1 * sizeof(int32_t));

    //TODO neon
    // conv row to  column-major
    for(int i=0;i<kernelDim1;i++)
      for(int j=0;j<inputDim1;j++)
        *(K+ inputDim1*i + j) = kernel[i+j*kernelDim1];

    // K=col-major
    float matMulScale = kernelScale * inputScale;
    float scale = matMulScale / outputScale;
    float bscale = biasScale / matMulScale;

    vector_matrix_multiply_s8(input, K, (int32_t *)C, inputDim1, kernelDim1, kernelDim0,kernelOffset );
    vector_scale_bias_offset_clip_s8(C, *((int32_t*)bias), output, scale,bscale, kernelDim1, 1, 0,outputOffset);
#if DEBUG_INFO
    for(int k=0;k<kernelDim1;k++) {
        int32_t *B = (int32_t*)bias;
        printf("[%d]i32:%d (%d+%d) output[k]=%d  ",k, C[k]+B[k],C[k],B[k],output[k]);
        if(k%4==0) printf("\n");
      }
       printf("\n\n (0,0) b0:%d C0:%d out0:%d",bias[0], C[0],output[0]);
#endif

    free(K);
    free(C);

  }

  return 0;
}


int fullyconnected(int8_t* input, float inputScale, int32_t inputOffset, int8_t *kernel, float kernelScale, int32_t kernelOffset,
                   int8_t *bias, float biasScale, int32_t biasOffset, int8_t *output, float outputScale, int32_t outputOffset,
                   dim_t inputDim0, dim_t inputDim1, dim_t kernelDim0, dim_t kernelDim1, dim_t outputDim0, dim_t outputDim1, bool doBias) {
#if 1 // #ifdef CPU_CONV_DBG
  return fullyconnected_cpu(input,inputScale,inputOffset,kernel, kernelScale,  kernelOffset,
                   bias, biasScale, biasOffset, output, outputScale, outputOffset,
                   inputDim0, inputDim1,  kernelDim0,  kernelDim1,  outputDim0,  outputDim1,  doBias);
#else
  return fullyconnected_acl(input,inputScale,inputOffset,kernel, kernelScale,  kernelOffset,
                   bias, biasScale, biasOffset, output, outputScale, outputOffset,
                   inputDim0, inputDim1,  kernelDim0,  kernelDim1,  outputDim0,  outputDim1,  doBias);
#endif
}

void maxpool_nxn_nhwc(int8_t *input, int8_t *output, dim_t N, dim_t H, dim_t W, dim_t C,
                      unsigned_t KH, unsigned_t KW, unsigned_t pad_size,
                      unsigned_t stride_size) {

  //TODO padding size

  // kernel size = nXn
  // nhwc. channel should be multiple of 16 to use int8x16_t type.
  if(KH!=KW && pad_size!=1) {
    printf("!!!!!!!!!! not impl yet\n");
    return;
  }

  int p=(int)pad_size;
  int out_w = ((W+2*p - KW )/stride_size) + 1;
  int out_h = ((H+2*p - KH )/stride_size) + 1;  

#if DEBUG_INFO
  printf("maxpool NHWC=%d,%d,%d,%d out_w =%d, out_h=%d\n",N,H,W,C,out_w, out_h);
#endif

  for(int n=0;n<N;n++)
  {
    for(int c=0;c<C/16;c++) // 16 channel once
    {
      for(int h=0,ho=0;ho < out_h;h+=stride_size,ho++)
      {
        for(int w=0,wo=0;wo< out_w ;w+=stride_size,wo++)
        {

          // assume kernel 3x3
          // pass if padding. start 0,0 including padding
          const int pool_start_y = std::max(p, h);
          const int pool_end_y   = std::min((int)H+p, (int)(h+KH));
          const int pool_start_x = std::max(p, w);
          const int pool_end_x   = std::min((int)W+p, (int)(w+KW));

          int8x16_t vres = vdupq_n_s8(-128);  // min value
#if DEBUG_INFO
          printf("--[%d~%d, %d~%d]--\n",pool_start_x,pool_end_x,pool_start_y,pool_end_y);
#endif
          // pool start index
          const int nopad_x=pool_start_x-p;
          const int nopad_y=pool_start_y-p;
          int start_idx = n*H*W*C + nopad_y*W*C + nopad_x*C + (c*16);

          for(int y = 0; y < pool_end_y - pool_start_y; ++y)
          {
            for(int x = 0; x < pool_end_x - pool_start_x; ++x)
            {
              //load a single vector
              const int8x16_t data = vld1q_s8(input + start_idx + x*C + y*W*C);
#if DEBUG_INFO
              //debug
                int8_t at[16];
                vst1q_s8(at, data);
                printf("[%d,%d]%x(%p) ",x,y,at[0],input + start_idx + x*C + y*W*C);
#endif
              // Vr[i] := (Va[i] >= Vb[i]) ? Va[i] : Vb[i]
              vres = vmaxq_s8(vres, data);
            }
          }

          // Store result
          // TODO check if qinfo matters. in orignal code, true means input , output scale offsets are same
          // org code -> (input_qinfo != output_qinfo) ? vrequantize_pooling<q8x8_t, q8x16_t>(wrapper::vgetlow(vres), wrapper::vgethigh(vres), requant_qinfo) :
          int out_idx = n* out_w*out_h*C +ho*out_w*C + wo*C + (c*16);
          vst1q_s8(output+out_idx,  vres);
#if DEBUG_INFO
          //debug
          int8_t ot[16];
          vst1q_s8(ot, vres);
          printf("\n out = %x\n ",ot[0]);
#endif

        }
      }
    }
  }
#if DEBUG_INFO
  for(int j=0;j<out_w*out_h;j++)
  {
    if(j%16==0) printf("\n");
    printf("%x ", output[j]);
  }
#endif

}


void maxpool_nhwc_acl(int8_t *input, int8_t *output, dim_t N, dim_t H, dim_t W, dim_t C,
            unsigned_t KH, unsigned_t KW, unsigned_t pad_size,
            unsigned_t stride_size) {
  

  int p=(int)pad_size;
  int out_w = ((W+2*p - KW )/stride_size) + 1;
  int out_h = ((H+2*p - KH )/stride_size) + 1;  

#if DEBUG_INFO  
  printf("maxpool NHWC=%d,%d,%d,%d out_w =%d, out_h=%d\n",N,H,W,C,out_w, out_h);
#endif  
  

    arm_compute::PadStrideInfo pad_stride_info = arm_compute::PadStrideInfo(stride_size,stride_size,pad_size,pad_size);
    arm_compute::Size2D poolsize(KH,KW);

    arm_compute::PoolingLayerInfo pi(arm_compute::PoolingType::MAX, poolsize, DataLayout::NHWC, pad_stride_info,false,false);

    const arm_compute::DataType            data_type = arm_compute::DataType::QASYMM8_SIGNED;

    // Create tensor
    // input NHWC. 
    const arm_compute::TensorShape         src_shape     =   arm_compute::TensorShape(C, W, H);
    const arm_compute::TensorInfo info(src_shape, 1, data_type,arm_compute::DataLayout::NHWC);
    arm_compute::Tensor           inputT{};
    inputT.allocator()->init(info); //, required_alignment);

    // scale offset??
    const arm_compute::TensorShape         shape2     =  arm_compute::TensorShape(C,out_w, out_h);
    const arm_compute::TensorInfo info2(shape2, 1, data_type,arm_compute::DataLayout::NHWC);
    arm_compute::Tensor           outputT{};
    outputT.allocator()->init(info2); //, required_alignment);


    arm_compute::NEPoolingLayer act_func;

    act_func.configure(&inputT, &outputT, pi);

  
    inputT.allocator()->allocate();
    outputT.allocator()->allocate();

    auto *iptr = reinterpret_cast<int8_t *>(inputT.buffer());
    auto *optr = reinterpret_cast<int8_t *>(outputT.buffer());

    memcpy(iptr,input, N*H*W*C);

    act_func.run();

    memcpy(output, optr,out_w*out_h*C);




}



int maxpool(int8_t* input, int8_t *output, dim_t N, dim_t H, dim_t W, dim_t C, unsigned_t KH, unsigned_t KW, unsigned_t pad_size,
            unsigned_t stride_size){

  //printf("maxpool kh=%d kw=%d pad=%d str=%d DIM=N:%d H:%d W:%d C:%d\n", KH,KW,pad_size,stride_size, N,H,W,C );

#ifdef CPU_CONV_DBG
  maxpool_nxn_nhwc(input,output,N,H,W,C,KH,KW,pad_size,stride_size);
#else  
  //maxpool_nhwc_acl(input,output,N,H,W,C,KH,KW,pad_size,stride_size);
  maxpool_nxn_nhwc(input,output,N,H,W,C,KH,KW,pad_size,stride_size);
#endif

  return 0;

}

int relu(int8_t* input, int8_t *output, unsigned_t size, float inputScale, float outputScale){

  int int_scale = (int)(inputScale / outputScale);
  const int8x16_t scale_v = vdupq_n_s8(int_scale);
  const int8x16_t zero_v = vdupq_n_s8(0);
  int div16_cnt = size / 16;
  int offset=0;
  for (int i = 0; i < div16_cnt ; i++) {
    const int8x16_t a = vld1q_s8(input+i*16);
    if(int_scale != 1) vst1q_s8(output+i*16, vmulq_s8(vmaxq_s8(zero_v,a),scale_v));
    else vst1q_s8(output+i*16, vmaxq_s8(zero_v,a));
  }
  if(size%16 >=8)
  {
    const int8x8_t a8 = vld1_s8(input+div16_cnt*16);
    const int8x8_t scale_v8 = vdup_n_s8(int_scale);
    const int8x8_t zero_v8 = vdup_n_s8(0);
    if(int_scale != 1) vst1_s8(output+div16_cnt*16, vmul_s8(vmax_s8(zero_v8,a8),scale_v8));
    else vst1_s8(output+div16_cnt*16, vmax_s8(zero_v8,a8));
    offset+=8;
  }
  int left = size - div16_cnt*16 - offset;
  for (int i = 0; i < left; i++) {
    int idx=div16_cnt*16 + offset + i;
    if (input[idx] > 0) output[idx] = input[idx];
    else output[idx] = 0;
    output[idx] *= int_scale;
  }

  return 0;
}


int avgpool_nhwc_acl(int8_t *input, float inputScale, int32_t inputOffset, int8_t *output, float outputScale, int32_t outputOffset, dim_t N, dim_t H, dim_t W, dim_t C,
                      dim_t outputDim0, dim_t outputDim1, dim_t outputDim2, dim_t outputDim3,unsigned_t KH, unsigned_t KW, unsigned_t pad_size,
                      unsigned_t stride_size) {

  #if 0
printf("avgpool acl is:%f, io%d I(%d,%d,%d,%d) os:%f oo:%d I(%d,%d,%d,%d)  K(%d,%d) pad:%d, st:%d\n",
 inputScale, inputOffset, N, H, W, C, 
  outputScale, outputOffset,   
        outputDim0,  outputDim1, outputDim2, outputDim3,  KH,  KW,  pad_size, stride_size);
#endif
  

  int p=(int)pad_size;
  int out_w = ((W+2*p - KW )/stride_size) + 1;
  int out_h = ((H+2*p - KH )/stride_size) + 1;  

 

    arm_compute::PadStrideInfo pad_stride_info = arm_compute::PadStrideInfo(stride_size,stride_size,pad_size,pad_size);
    arm_compute::Size2D poolsize(KH,KW);

    arm_compute::PoolingLayerInfo pi(arm_compute::PoolingType::AVG, poolsize, DataLayout::NHWC, pad_stride_info,false,false);

    const arm_compute::DataType            data_type = arm_compute::DataType::QASYMM8_SIGNED;

    // Create tensor
    // input NHWC. 
    const arm_compute::TensorShape         src_shape     =   arm_compute::TensorShape(C, W, H);
    const arm_compute::TensorInfo info(src_shape, 1, data_type,arm_compute::DataLayout::NHWC);
    arm_compute::Tensor           inputT{};
    inputT.allocator()->init(info); //, required_alignment);

    // scale offset??
    const arm_compute::TensorShape         shape2     =  arm_compute::TensorShape(C,out_w, out_h);
    const arm_compute::TensorInfo info2(shape2, 1, data_type,arm_compute::DataLayout::NHWC);
    arm_compute::Tensor           outputT{};
    outputT.allocator()->init(info2); //, required_alignment);


    arm_compute::NEPoolingLayer act_func;

    act_func.configure(&inputT, &outputT, pi);

  
    inputT.allocator()->allocate();
    outputT.allocator()->allocate();

    auto *iptr = reinterpret_cast<int8_t *>(inputT.buffer());
    auto *optr = reinterpret_cast<int8_t *>(outputT.buffer());

    memcpy(iptr,input, N*H*W*C);

    act_func.run();

    memcpy(output, optr,out_w*out_h*C);




}



int avgpool_cpu(int8_t *input, float inputScale, int32_t inputOffset, int8_t *output, float outputScale, int32_t outputOffset, dim_t N, dim_t H, dim_t W, dim_t C,
                      dim_t outputDim0, dim_t outputDim1, dim_t outputDim2, dim_t outputDim3,unsigned_t KH, unsigned_t KW, unsigned_t pad_size,
                      unsigned_t stride_size) {

  #if 0
printf("is:%f, io%d I(%d,%d,%d,%d) os:%f oo:%d I(%d,%d,%d,%d)  K(%d,%d) pad:%d, st:%d\n",
 inputScale, inputOffset, N, H, W, C, 
  outputScale, outputOffset,   
        outputDim0,  outputDim1, outputDim2, outputDim3,  KH,  KW,  pad_size, stride_size);
#endif

  //TODO padding > 1

  // kernel size = nXn
  // nhwc. channel should be multiple of 16
  if(KH!=KW && pad_size<=1) {
    printf("!!!!!!!!!! not impl yet\n");
    return -1;
  }

  const float32x4_t             half_scale_v = vdupq_n_f32(0.5f);
  int p=(int)pad_size;
  int out_w = ((W+2*p - KW )/stride_size) + 1;
  int out_h = ((H+2*p - KH )/stride_size) + 1;  

  const float scale = 1.0f * (inputScale / outputScale / (KW*KH));

#if DEBUG_INFO
  printf("avgpool scale:%f outw:%d outh:%d\n",scale, out_w, out_h);
#endif

  for(int n=0;n<N;n++)
  {
    for(int c=0;c<C/16;c++)
    {
      for(int h=0,ho=0;ho<out_h;h+=stride_size,ho++) // ho = h out.
      {
        for(int w=0,wo=0;wo<out_w;w+=stride_size,wo++)
        {


          const int pool_start_y = std::max(p, h);
          const int pool_end_y   = std::min((int)H+p, (int)(h+KH));
          const int pool_start_x = std::max(p, w);
          const int pool_end_x   = std::min((int)W+p, (int)(w+KW));


          float32x4_t vres1 = vdupq_n_f32(0.f);
          float32x4_t vres2 = vdupq_n_f32(0.f);
          float32x4_t vres3 = vdupq_n_f32(0.f);
          float32x4_t vres4 = vdupq_n_f32(0.f);



#if DEBUG_INFO
          printf("--[%d~%d, %d~%d]--\n",pool_start_x,pool_end_x,pool_start_y,pool_end_y);
#endif
          // pool start idx
          const int nopad_x=pool_start_x-p;
          const int nopad_y=pool_start_y-p;
          int start_idx = n*H*W*C + nopad_y*W*C + nopad_x*C + (c*16);


          for(int y = 0; y < pool_end_y - pool_start_y; ++y)
          {
            for(int x = 0; x < pool_end_x - pool_start_x; ++x)
            {
              //load a single vector
              const int8x16_t data = vld1q_s8(input + start_idx + x*C + y*W*C);

              const int16x8_t data_s16  = vmovl_s8(vget_low_s8(data));
              const int16x8_t data2_s16 = vmovl_s8(vget_high_s8(data));

              vres1   = vaddq_f32(vres1, vcvtq_f32_s32(vmovl_s16(vget_low_s16(data_s16))));
              vres2   = vaddq_f32(vres2, vcvtq_f32_s32(vmovl_s16(vget_high_s16(data_s16))));
              vres3   = vaddq_f32(vres3, vcvtq_f32_s32(vmovl_s16(vget_low_s16(data2_s16))));
              vres4   = vaddq_f32(vres4, vcvtq_f32_s32(vmovl_s16(vget_high_s16(data2_s16))));

#if DEBUG_INFO
              //debug
                int8_t at[16];
                float32_t tt[4];
                vst1q_f32(tt,vres1);
                vst1q_s8(at, data);
                printf("[%d,%d]%x(%f)  ",x,y,at[0],vres1[0]); // %p  ,input + start_idx + x*C + y*W*C);
#endif

            }
          }

          //TODO check using vcvtaq or vcvtnq
          // Store result
          const float32x4_t scale_v = vdupq_n_f32(scale);
          // Divide by scale and add 0.5f to round to nearest instead of rounding towards zero
          vres1 = vmlaq_f32(half_scale_v, vres1, scale_v);
          vres2 = vmlaq_f32(half_scale_v, vres2, scale_v);
          vres3 = vmlaq_f32(half_scale_v, vres3, scale_v);
          vres4 = vmlaq_f32(half_scale_v, vres4, scale_v);

          // vcvtq round towrd zero shows same result with original one
          const int8x8_t res1 = vqmovn_s16(vcombine_s16(vqmovn_s32(vcvtq_s32_f32(vres1)), vqmovn_s32(vcvtq_s32_f32(vres2))));
          const int8x8_t res2 = vqmovn_s16(vcombine_s16(vqmovn_s32(vcvtq_s32_f32(vres3)), vqmovn_s32(vcvtq_s32_f32(vres4))));
          int out_idx = n*(out_h*out_w)*C +ho*(out_w)*C + wo*C + (c*16);
          const int8x16_t vres = vcombine_s8(res1, res2);
          vst1q_s8(output + out_idx, vres);


#if DEBUG_INFO
          //debug
          float32_t t1[4];
          vst1q_f32(t1, vres1);
          int8_t ot[16];
          vst1q_s8(ot, vres);
          printf("\n out = %f %d [%d]\n ",t1[0],ot[0],out_compare[out_idx]);
#endif

        }
      }
    }
  }
#if DEBUG_INFO
  for(int j=0;j<1024;j++)
  {
    if(j%16==0) printf("\n");
    printf("%x ", output[j]);
  }
#endif

  return 0;

}

int avgpool(int8_t *input, float inputScale, int32_t inputOffset, int8_t *output, float outputScale, int32_t outputOffset, dim_t N, dim_t H, dim_t W, dim_t C,
                      dim_t outputDim0, dim_t outputDim1, dim_t outputDim2, dim_t outputDim3,unsigned_t KH, unsigned_t KW, unsigned_t pad_size,
                      unsigned_t stride_size) {

#if 1
  return avgpool_cpu(input,  inputScale,  inputOffset, output,  outputScale,  outputOffset,  N,  H,  W,  C,
                       outputDim0,  outputDim1,  outputDim2,  outputDim3, KH,  KW,  pad_size, stride_size);
#else
   return avgpool_nhwc_acl(input,  inputScale,  inputOffset, output,  outputScale,  outputOffset,  N,  H,  W,  C,
                       outputDim0,  outputDim1,  outputDim2,  outputDim3, KH,  KW,  pad_size, stride_size);
#endif

}



int quantize(int8_t *input, int8_t *output, dim_t size, float scale, int32_t offset){
  const float32x4_t       scale_v = vdupq_n_f32( 1.0f / scale);
  assert(size%16==0);

  //printf("scale:%f\n",scale);
  float32_t* in = (float32_t*)input;
  for(int i=0;i<size;i+=16)
  {
    //const float32x4x4_t src = vld1q_f32_x4(in + i);
    const float32x4_t s1 = vld1q_f32(in + i);
    const float32x4_t s2 = vld1q_f32(in + i + 4);
    const float32x4_t s3 = vld1q_f32(in + i + 8);
    const float32x4_t s4 = vld1q_f32(in + i + 12);
    //vcvtaq�ݿø��� ���� �ȸ���
    //vcttnq�� �ؾ���. round even to zero
    const int32x4x4_t rf =
        {
            {
                vcvtnq_s32_f32(vmulq_f32(s1, scale_v)),
                vcvtnq_s32_f32(vmulq_f32(s2, scale_v)),
                vcvtnq_s32_f32(vmulq_f32(s3, scale_v)),
                vcvtnq_s32_f32(vmulq_f32(s4, scale_v)),
            }
        };
    const int8x8_t pa = vqmovn_s16(vcombine_s16(vqmovn_s32(rf.val[0]), vqmovn_s32(rf.val[1])));
    const int8x8_t pb = vqmovn_s16(vcombine_s16(vqmovn_s32(rf.val[2]), vqmovn_s32(rf.val[3])));
    vst1q_s8(output + i, vcombine_s8(pa, pb));
  }
  return 0;
}

int dequantize(int8_t *input, int8_t *output, dim_t size, float scale, int32_t offset){
  // output is type float*

  const float32x4_t       scale_v = vdupq_n_f32(scale);

  // only for size%8 ==0
  assert(size%8==0);

  float32_t *out = (float32_t*)output;

  for(int i=0;i<size/8;i++)
  {
    const int8x8_t     a = vld1_s8(input + i*8);

    const float32x4x2_t af =
        {
            {
                vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(vmovl_s8(a)))), scale_v),
                vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(vmovl_s8(a)))),  scale_v),
            }
        };

    //vst1q_f32_x2(out+i*8, af);
    vst1q_f32(out+i*8, af.val[0]);
    vst1q_f32(out+i*8+4, af.val[1]);
  }
  return 0;
}


int transpose(int8_t *input, int8_t *output, dim_t inDim0, dim_t inDim1, dim_t inDim2, dim_t inDim3,
              dim_t outDim0, dim_t outDim1, dim_t outDim2, dim_t outDim3,
              unsigned_t shf0, unsigned_t shf1, unsigned_t shf2, unsigned_t shf3) {
//TODO re-implement
  return -1;
}

int elemadd(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
            int8_t *output, float destScale, int32_t destOffset, unsigned_t size) {

  assert(input0Offset==0);
  assert(input1Offset==0);


  //use largeScale to prevent overflow
  const float32x4_t i0scale  = vdupq_n_f32(input0Scale * (1<<15));
  const float32x4_t i1scale  = vdupq_n_f32(input1Scale * (1<<15));
  const float32x4_t invdscale  = vdupq_n_f32((float(1) / ((1<<15)) / destScale));

  // clip
  const int32x4_t max = vdupq_n_s32(127);
  const int32x4_t min = vdupq_n_s32(-128);

#if DEBUG_INFO
  printf("lio: %d rio: %d\n", input0Offset, input1Offset);
  printf("largeInv: %f\n",(float(1) / ((1<<15)) / destScale));

#endif

  if(size%16 == 0)
  {
    for(int i=0; i<size; i+=16)
    {


      const int8x16_t     a = vld1q_s8(input0 + i);
      const int32x4x4_t af =
          {
              {
                  // vget_low_s8 = 16���� 8���� ������
                  // vmovl_s8 = int8->int16(������ int16, int32�� ����)
                  // vget_low_s16 = int16*8 -> int 16*4
                  // vmovl_s16 = int16->int32 (float conv�� s32�� ����.)
                  // vcctq_f32_s32 = conv s32->f32

                  vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(vmovl_s8(vget_low_s8(a))))), i0scale)),
                  vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(vmovl_s8(vget_low_s8(a))))),  i0scale)),
                  vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(vmovl_s8(vget_high_s8(a))))),  i0scale)),
                  vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(vmovl_s8(vget_high_s8(a))))), i0scale)),
              }
          };
      const int8x16_t     b = vld1q_s8(input1 + i);
      const int32x4x4_t bf =
          {
              {
                  vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(vmovl_s8(vget_low_s8(b))))), i1scale)),
                  vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(vmovl_s8(vget_low_s8(b))))),  i1scale)),
                  vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(vmovl_s8(vget_high_s8(b))))), i1scale)),
                  vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(vmovl_s8(vget_high_s8(b))))),  i1scale)),
              }
          };
      //���� �ڵ尡 ������ roundó�� �� �ڿ� �ٽ� float�� �ٲ� ó���ϱ⶧���� �����ϰ� ó��
      const int32x4x4_t rf =
          {
              {

                  vmaxq_s32(vminq_s32(vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vaddq_s32(af.val[0], bf.val[0])), invdscale)), max),min),
                  vmaxq_s32(vminq_s32(vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vaddq_s32(af.val[1], bf.val[1])), invdscale)), max),min),
                  vmaxq_s32(vminq_s32(vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vaddq_s32(af.val[2], bf.val[2])), invdscale)), max),min),
                  vmaxq_s32(vminq_s32(vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vaddq_s32(af.val[3], bf.val[3])), invdscale)), max),min),
              }
          };
      const int8x8_t pa = vqmovn_s16(vcombine_s16(vqmovn_s32(rf.val[0]), vqmovn_s32(rf.val[1])));
      const int8x8_t pb = vqmovn_s16(vcombine_s16(vqmovn_s32(rf.val[2]), vqmovn_s32(rf.val[3])));
      vst1q_s8(output + i, vcombine_s8(pa, pb));

#if DEBUG_INFO

      // load org
        int8_t aa[32],bb[32];
        vst1q_s8(aa,a);
        vst1q_s8(bb,b);
        for(int k=0;k<16;k++)
        {
          printf("(%d,%d) ", aa[k],bb[k]);
        }
        printf("\n");

        int at[4],bt[4];
        int rt[4];

        for(int k=0;k<4;k++)
        {
          vst1q_s32(at, af.val[k]);
          vst1q_s32(bt, bf.val[k]);
          vst1q_s32(rt, rf.val[k]);
          for(int j=0;j<4;j++)
          {
            int idx=i+k*4+j;
            printf("[%d]  l:%d r:%d sum32:%d o:%d \n",idx, at[j], rt[j], rt[j],output[idx]);
          }
        }
#endif
    }
  }
  return 0;
}

int elemadd_relu(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
                 int8_t *output, float destScale, int32_t destOffset, unsigned_t size) {
  for (dim_t i = 0, e = size; i < e; i++) {
    int32_t L = input0[i];
    int32_t R = input1[i];

    // We increase the size of the integer up to 16 bits to prevent overflow.
    const float largeScale = float(1) / (1 << 15);
    // Scale both sides from 8-bit to 16-bits.
    int32_t L32 = std::round(float(L - input0Offset) * (input0Scale / largeScale));
    int32_t R32 = std::round(float(R - input1Offset) * (input1Scale / largeScale));
    int32_t sum32 = L32 + R32;
    sum32 = std::round(float(sum32) * (largeScale / destScale) + destOffset);
    if(sum32>127) sum32 = 127;
    if(sum32<-128) sum32 = -128;
    output[i] = (int8_t) sum32;
    if(output[i]<0) output[i]=0;
  }
  return 0;
}


int elemsub(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
            int8_t *output, float destScale, int32_t destOffset, unsigned_t size) {

  assert(input0Offset==0);
  assert(input1Offset==0);
  assert(size%16==0);

  // use largeScale to prevent overflow as elemadd
  const float32x4_t i0scale  = vdupq_n_f32(input0Scale * (1<<15));
  const float32x4_t i1scale  = vdupq_n_f32(input1Scale * (1<<15));
  const float32x4_t invdscale  = vdupq_n_f32((float(1) / ((1<<15)) / destScale));

  // clip
  const int32x4_t max = vdupq_n_s32(127);
  const int32x4_t min = vdupq_n_s32(-128);

#if DEBUG_INFO
  printf("lio: %d rio: %d\n", input0Offset, input1Offset);
  printf("largeInv: %f\n",(float(1) / ((1<<15)) / destScale));
#endif

  if(size%16 == 0)
  {
    for(int i=0; i<size; i+=16)
    {
      const int8x16_t     a = vld1q_s8(input0 + i);
      const int32x4x4_t af =
          {
              {
                  vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(vmovl_s8(vget_low_s8(a))))), i0scale)),
                  vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(vmovl_s8(vget_low_s8(a))))),  i0scale)),
                  vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(vmovl_s8(vget_high_s8(a))))),  i0scale)),
                  vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(vmovl_s8(vget_high_s8(a))))), i0scale)),
              }
          };
      const int8x16_t     b = vld1q_s8(input1 + i);
      const int32x4x4_t bf =
          {
              {
                  vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(vmovl_s8(vget_low_s8(b))))), i1scale)),
                  vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(vmovl_s8(vget_low_s8(b))))),  i1scale)),
                  vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(vmovl_s8(vget_high_s8(b))))), i1scale)),
                  vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(vmovl_s8(vget_high_s8(b))))),  i1scale)),
              }
          };
      const int32x4x4_t rf =
          {
              {
                  vmaxq_s32(vminq_s32(vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vsubq_s32(af.val[0], bf.val[0])), invdscale)), max),min),
                  vmaxq_s32(vminq_s32(vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vsubq_s32(af.val[1], bf.val[1])), invdscale)), max),min),
                  vmaxq_s32(vminq_s32(vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vsubq_s32(af.val[2], bf.val[2])), invdscale)), max),min),
                  vmaxq_s32(vminq_s32(vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vsubq_s32(af.val[3], bf.val[3])), invdscale)), max),min),
              }
          };
      const int8x8_t pa = vqmovn_s16(vcombine_s16(vqmovn_s32(rf.val[0]), vqmovn_s32(rf.val[1])));
      const int8x8_t pb = vqmovn_s16(vcombine_s16(vqmovn_s32(rf.val[2]), vqmovn_s32(rf.val[3])));
      vst1q_s8(output + i, vcombine_s8(pa, pb));
    }
  }
  return 0;
}


int elemsub_f32(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
                int8_t *output, float destScale, int32_t destOffset, unsigned_t size) {
  for (dim_t i = 0, e = size; i < e; i++) {
    float L = ((float*)input0)[i];
    float R = ((float*)input1)[i];
    ((float*)output)[i] = L - R;
  }
  return 0;
}


int elemdiv(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
            int8_t *output, float destScale, int32_t destOffset, unsigned_t size) {
#if DEBUG_INFO
  printf("elemdiv: i0s:%f i0o:%d  i1s:%f i1o:%d  ds:%f do:%d \n",input0Scale, input0Offset, input1Scale, input1Offset, destScale, destOffset);
#endif

  assert(input0Offset==0);
  assert(input1Offset==0);
  assert(size%16==0);

  const float32x4_t i0scale  = vdupq_n_f32(input0Scale);
  const float32x4_t i1scale  = vdupq_n_f32(input1Scale * destScale);

  // clip
  const int32x4_t max = vdupq_n_s32(127);
  const int32x4_t min = vdupq_n_s32(-128);

  if(size%16 == 0)
  {
    for(int i=0; i<size; i+=16)
    {
      const int8x16_t     a = vld1q_s8(input0 + i);
      const float32x4x4_t af =
          {
              {
                  vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(vmovl_s8(vget_low_s8(a))))), i0scale),
                  vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(vmovl_s8(vget_low_s8(a))))),  i0scale),
                  vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(vmovl_s8(vget_high_s8(a))))),  i0scale),
                  vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(vmovl_s8(vget_high_s8(a))))), i0scale),
              }
          };
      const int8x16_t     b = vld1q_s8(input1 + i);
      const float32x4x4_t bf =
          {
              {
                  vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(vmovl_s8(vget_low_s8(b))))), i1scale),
                  vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(vmovl_s8(vget_low_s8(b))))),  i1scale),
                  vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(vmovl_s8(vget_high_s8(b))))), i1scale),
                  vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(vmovl_s8(vget_high_s8(b))))),  i1scale),
              }
          };
      //rounding to nearest with ties to away
      const int32x4x4_t rf =
          {
              {
                  vmaxq_s32(vminq_s32(vcvtaq_s32_f32(vdivq_f32(af.val[0], bf.val[0])), max),min),
                  vmaxq_s32(vminq_s32(vcvtaq_s32_f32(vdivq_f32(af.val[1], bf.val[1])), max),min),
                  vmaxq_s32(vminq_s32(vcvtaq_s32_f32(vdivq_f32(af.val[2], bf.val[2])), max),min),
                  vmaxq_s32(vminq_s32(vcvtaq_s32_f32(vdivq_f32(af.val[3], bf.val[3])), max),min),
              }
          };
      const int8x8_t pa = vqmovn_s16(vcombine_s16(vqmovn_s32(rf.val[0]), vqmovn_s32(rf.val[1])));
      const int8x8_t pb = vqmovn_s16(vcombine_s16(vqmovn_s32(rf.val[2]), vqmovn_s32(rf.val[3])));
      vst1q_s8(output + i, vcombine_s8(pa, pb));
    }
  }
  return 0;
}


int elemdiv_f32(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
                int8_t *output, float destScale, int32_t destOffset, unsigned_t size) {
  for (dim_t i = 0, e = size; i < e; i++) {
    float L = ((float*)input0)[i];
    float R = ((float*)input1)[i];
    ((float*)output)[i] = L / R;
  }
  return 0;
}



int splat(int8_t *output, unsigned_t size, int8_t value) {

  //size = n * 16
  assert(size%16==0);

  const int8x16_t v = vdupq_n_s8(value);
  for(int i=0; i<size; i+=16)
  {
    vst1q_s8(output+i, v);
  }
  return 0;
}


int elemmax(int8_t *input0, float input0Scale, int32_t input0Offset, int8_t *input1, float input1Scale, int32_t input1Offset,
            int8_t *output, float destScale, int32_t destOffset, unsigned_t size) {

#if DEBUG_INFO
  printf("elemmax: i0s:%f i0o:%d  i1s:%f i1o:%d  ds:%f do:%d \n",input0Scale, input0Offset, input1Scale, input1Offset, destScale, destOffset);
#endif

  assert(input0Offset==0);
  assert(input1Offset==0);
  assert(size%16==0);

  const float32x4_t i0scale  = vdupq_n_f32(input0Scale / destScale);
  const float32x4_t i1scale  = vdupq_n_f32(input1Scale / destScale);

  // for clip s8
  const int32x4_t max = vdupq_n_s32(127);
  const int32x4_t min = vdupq_n_s32(-128);

  //case 1. size = n * 16, offset = 0
  if(size%16 == 0)
  {
    int s = size/16;
    for(int i=0; i<size; i+=16)
    {
      const int8x16_t   a = vld1q_s8(input0 + i);
      const int32x4x4_t af =
          {
              {
                  vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(vmovl_s8(vget_low_s8(a))))), i0scale)),
                  vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(vmovl_s8(vget_low_s8(a))))),  i0scale)),
                  vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(vmovl_s8(vget_high_s8(a))))),  i0scale)),
                  vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(vmovl_s8(vget_high_s8(a))))), i0scale)),
              }
          };

      const int8x16_t   b = vld1q_s8(input1 + i);
      const int32x4x4_t bf =
          {
              {
                  vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(vmovl_s8(vget_low_s8(b))))), i1scale)),
                  vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(vmovl_s8(vget_low_s8(b))))),  i1scale)),
                  vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(vmovl_s8(vget_high_s8(b))))), i1scale)),
                  vcvtaq_s32_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_high_s16(vmovl_s8(vget_high_s8(b))))),  i1scale)),
              }
          };

      const int32x4x4_t rf =
          {
              {
                  vmaxq_s32(vminq_s32(vmaxq_s32(af.val[0], bf.val[0]), max),min),
                  vmaxq_s32(vminq_s32(vmaxq_s32(af.val[1], bf.val[1]), max),min),
                  vmaxq_s32(vminq_s32(vmaxq_s32(af.val[2], bf.val[2]), max),min),
                  vmaxq_s32(vminq_s32(vmaxq_s32(af.val[3], bf.val[3]), max),min),
              }
          };
      const int8x8_t pa = vqmovn_s16(vcombine_s16(vqmovn_s32(rf.val[0]), vqmovn_s32(rf.val[1])));
      const int8x8_t pb = vqmovn_s16(vcombine_s16(vqmovn_s32(rf.val[2]), vqmovn_s32(rf.val[3])));
      vst1q_s8(output + i, vcombine_s8(pa, pb));
    }
  }
  return 0;
}

int softmax(int8_t *input, int8_t *output, dim_t inDim0, dim_t inDim1,
            dim_t outDim0, dim_t outDim1){
  float *inputP = (float *)input;
  float *outputP = (float *)output;
  for (dim_t n = 0; n < inDim0; n++) {
    // Find Max.
    float max = inputP[n * inDim0];
    for (dim_t i = 1; i < inDim1; i++) {
      max = std::max(max, inputP[n * inDim0 + i]);
    }

    // Compute exp.
    float sum = 0;
    for (dim_t i = 0; i < inDim1; i++) {
      float e = std::exp(inputP[n * inDim0 + i] - max);
      sum += e;
      outputP[n*inDim0 +i] = float(e);
    }
    // Normalize the output.
    for (dim_t i = 0; i < inDim1; i++) {
      outputP[n*inDim0 +i] =
          outputP[n*inDim0 +i] / sum;
    }
  }

  return 0;
}


void debugprint(int8_t *input, unsigned_t size, std::string filename, bool isInt8 ) {
  std::ofstream fos(filename.c_str(), std::ios::binary);

  if(isInt8) {
    int16_t data16 = 0;
    for (size_t i = 0, e = size; i < e; i++) {
      auto data = input[i];
      if (i % 2 == 0) {
        data16 = 0xff & data;
      } else {
        data16 = data16 | data << 8;
        fos.write((const char *) &data16, 2);
      }
    }
  }
  else{
    int32_t* input_32 = (int32_t*) input;
    for (size_t i = 0, e = size; i < e; i++) {
      auto data = input_32[i];
      fos.write((const char *)&data, 4);
    }
  }
  fos.close();
}


//#### JO added (2026-10-31): NHWC <-> N{C/16}HW16 as one pass of 16-byte copies (the 16 channels of a block are
//#### contiguous in both layouts); was a malloc + NHWC -> NCHW -> N{C/16}HW16 byte by byte (Conv4Test 56x56x64:
//#### both transposes ~5.3 ms per run on the A53). Same channels as before (blocks c_ < C/16).
void transpose_nhwc2vtaio(int8_t* input, int8_t* output, int N, int H, int W, int C){
  const int CB = C / 16;
  for (int n = 0; n < N; n++)
    for (int c_ = 0; c_ < CB; c_++) {
      const int8_t *src = input + (size_t)n * H * W * C + c_ * 16;
      int8_t *dst = output + ((size_t)n * CB + c_) * H * W * 16;
      for (int p = 0; p < H * W; p++)
        memcpy(dst + (size_t)p * 16, src + (size_t)p * C, 16);
    }
}

void transpose_nhwc2vtaio_old(int8_t* input, int8_t* output, int N, int H, int W, int C){
  int8_t *IN_NHWC = input;

  int8_t *IN_NCHW = (int8_t *)malloc(N*C*H*W);
  /*
  for (int n = 0; n < N; n++) {
    for (int h = 0; h < H; h++) {
      for (int w = 0; w < W; w++) {
        for (int c = 0; c < C; c++) {
          *(IN_NCHW + n * C * H * W + c * H * W + h * W + w) =
              *(IN_NHWC + n * H * W * C + h * W * C + w * C + c);
        }
      }
    }
  }
  */
  nhwc_to_nchw_s8(input, IN_NCHW, N,H,W,C);


  // reshape input NCHW -> N{C//16}16HW
  int8_t *in_NC_16HW = IN_NCHW;


  for (int n = 0; n < N; n++) {
    for (int c_ = 0; c_ < C / 16; c_++) {
      for (int h = 0; h < H; h++) {
        for (int w = 0; w < W; w++) {
          for (int c = 0; c < 16; c++) {
            *(output + n*(C/16)*H*W*16 + c_*H*W*16 + h*W*16 + w*16 + c) =
                *(in_NC_16HW + n*(C/16)*16*H*W + c_*16*H*W + c*H*W + h*W + w);
          }
        }
      }
    }
  }

  free(IN_NCHW);
}

void transpose_nhwc2vtak(int8_t* input, int8_t* output, int KN, int KH, int KW, int C){
  // filter NHWC
  int8_t *filter_NHWC = input;

  int8_t* filter_N_16HWC_16 = filter_NHWC;
  // transpose filter {N//16}16HW{C//16}16 =>  {N//16}{C//16}HW1616
  for (int n_ = 0; n_ < KN / 16; n_++) {
    for (int c_ = 0; c_ < C / 16; c_++) {
      for (int h = 0; h < KH; h++) {
        for (int w = 0; w < KW; w++) {
          for (int n = 0; n < 16; n++) {
            for (int c = 0; c < 16; c++) {
              *(output + n_*(C/16)*KH*KW*16*16 + c_ *KH*KW*16*16 + h*KW*16*16 + w*16*16 + n*16 + c) =
                  *(filter_N_16HWC_16 + n_*16*KH*KW*C + n*KH*KW*C + h*KW*C + w*C + c_*16 + c);
            }
          }
        }
      }
    }
  }
}

void transpose_vtak2nhwc(int8_t* input, int8_t* output, int KN, int KH, int KW, int C){
  for (int n_ = 0; n_ < KN / 16; n_++) {
    for (int c_ = 0; c_ < C / 16; c_++) {
      for (int h = 0; h < KH; h++) {
        for (int w = 0; w < KW; w++) {
          for (int n = 0; n < 16; n++) {
            for (int c = 0; c < 16; c++) {
              *(output + n_*16*KH*KW*C + n*KH*KW*C + h*KW*C + w*C + c_*16 + c)
                  = *(input + n_*(C/16)*KH*KW*16*16 + c_ *KH*KW*16*16 + h*KW*16*16 + w*16*16 + n*16 + c);
            }
          }
        }
      }
    }
  }
}


//#### JO added (2026-10-31): 16-byte copies (see transpose_nhwc2vtaio); C % 16 channels of the last block byte by byte.
//#### The old code read every batch from batch 0 of the input; this one reads batch b at b * C/16 blocks (N = 1 the same).
void transpose_vtaio2nhwc(int8_t* input, int8_t* output, int N, int H, int W, int C){
  const int CB = (C + 15) / 16;
  for (int b = 0; b < N; b++)
    for (int c_ = 0; c_ < CB; c_++) {
      const int nc = std::min(16, C - c_ * 16);
      const int8_t *src = input + ((size_t)b * CB + c_) * H * W * 16;
      int8_t *dst = output + (size_t)b * H * W * C + c_ * 16;
      if (nc == 16)
        for (int p = 0; p < H * W; p++)
          memcpy(dst + (size_t)p * C, src + (size_t)p * 16, 16);
      else
        for (int p = 0; p < H * W; p++)
          memcpy(dst + (size_t)p * C, src + (size_t)p * 16, nc);
    }
}

void transpose_vtaio2nhwc_old(int8_t* input, int8_t* output, int N, int H, int W, int C){
  for (int b = 0; b < N; b++) {
    for (int f = 0; f < C; f++) {
      for (int i = 0; i < H; i++) {    //h
        for (int j = 0; j < W; j++) {    //w

          int out_ch = f / 16;
          int oo = f % 16;
          auto value =*(input + (out_ch * H * W * 16 + i * W * 16 + j * 16 + oo));
          *(output + b * H * W * C + i * W * C + j * C + f) = value;
        }
      }
    }
  }
}
