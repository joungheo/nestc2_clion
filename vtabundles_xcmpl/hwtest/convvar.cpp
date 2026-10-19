// Isolate which conv feature breaks on the EVTA hardware: calls vtalib convolution() (the same
// VTABundle path the VTA backend uses) and compares against a CPU reference.
// Inputs/weights are small so outputs do not saturate at +-127 (saturation hides errors).
//
// usage: ./convvar K pad bias [C] [KN] [H]
//   K    : kernel size (1 or 3)       pad : 0 or 1
//   bias : 0 or 1                     C/KN/H default 16/32/14
// examples: ./convvar 1 0 0   (like conv13)     ./convvar 3 1 1   (like vtaConv3Test)
#include "VTABundle.h"
#include "vta/runtime.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>

static void refConv(const int8_t *in, const int8_t *k, const int32_t *b, int8_t *out, int H,
                    int W, int C, int KN, int KH, int KW, int pad, int stride, int shift,
                    int oh, int ow) {
  for (int h = 0; h < oh; h++)
    for (int w = 0; w < ow; w++)
      for (int n = 0; n < KN; n++) {
        int32_t s = 0;
        for (int fh = 0; fh < KH; fh++)
          for (int fw = 0; fw < KW; fw++) {
            int ih = h * stride - pad + fh, iw = w * stride - pad + fw;
            if (ih < 0 || iw < 0 || ih >= H || iw >= W) continue;
            for (int c = 0; c < C; c++)
              s += k[((n * KH + fh) * KW + fw) * C + c] * in[(ih * W + iw) * C + c];
          }
        s += b ? b[n] : 0;
        s >>= shift;
        out[(h * ow + w) * KN + n] = (int8_t)std::max(-128, std::min(127, s));
      }
}

int main(int argc, char **argv) {
  if (argc < 4) { fprintf(stderr, "usage: %s K pad bias [C] [KN] [H]\n", argv[0]); return 2; }
  const int K = atoi(argv[1]), pad = atoi(argv[2]);
  const bool doBias = atoi(argv[3]);
  const int C = argc > 4 ? atoi(argv[4]) : 16, KN = argc > 5 ? atoi(argv[5]) : 32;
  const int H = argc > 6 ? atoi(argv[6]) : 14, W = H, N = 1, stride = 1, shift = 4;
  const int oh = (H + 2 * pad - K) / stride + 1, ow = oh;

  srand(1);
  int8_t *in = (int8_t *)malloc(H * W * C);
  for (int i = 0; i < H * W * C; i++) in[i] = rand() % 16;
  int8_t *k = (int8_t *)malloc(KN * K * K * C + KN * 4);   // bias follows the weights
  for (int i = 0; i < KN * K * K * C; i++) k[i] = rand() % 7 - 3;
  int32_t *b = (int32_t *)(k + KN * K * K * C);
  for (int i = 0; i < KN; i++) b[i] = doBias ? rand() % 401 - 200 : 0;

  int8_t *out = (int8_t *)calloc(oh * ow * KN, 1), *ref = (int8_t *)calloc(oh * ow * KN, 1);
  convolution(in, k, out, N, H, W, C, KN, K, K, pad, stride, false, doBias, shift, oh, ow);
  refConv(in, k, doBias ? b : nullptr, ref, H, W, C, KN, K, K, pad, stride, shift, oh, ow);

  int diff = 0, sat = 0, badc = 0;
  for (int i = 0; i < oh * ow * KN; i++) { diff += out[i] != ref[i]; sat += ref[i] == 127 || ref[i] == -128; }
  for (int n = 0; n < KN; n++) {
    int d = 0;
    for (int p = 0; p < oh * ow; p++) d += out[p * KN + n] != ref[p * KN + n];
    badc += d > 0;
  }
  printf("K=%d pad=%d bias=%d C=%d KN=%d H=%d -> out %dx%dx%d  mismatches=%d/%d  bad_channels=%d/%d  saturated_ref=%d\n",
         K, pad, doBias, C, KN, H, oh, ow, KN, diff, oh * ow * KN, badc, KN, sat);
  for (int i = 0, shown = 0; i < oh * ow * KN && shown < 6; i++)
    if (out[i] != ref[i]) {
      printf("  [h%d w%d c%d] hw=%d ref=%d\n", i / KN / ow, i / KN % ow, i % KN, out[i], ref[i]);
      shown++;
    }
  printf("RESULT convvar %d %d %d: %s\n", K, pad, (int)doBias, diff ? "FAIL" : "PASS");
  VTARuntimeShutdown();
  return diff != 0;
}
