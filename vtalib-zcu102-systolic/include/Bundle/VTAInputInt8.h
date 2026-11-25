#ifndef VTALIB_SYSTOLIC_VTAINPUTINT8_H
#define VTALIB_SYSTOLIC_VTAINPUTINT8_H
#include <cstdint>
//#### JO added (2026-11-25, docs/evta_dpu_compare.md 6.10): int8 input. The bundle takes a float image and quantizes it in
//#### preprocess_conv0pack_stream (scale / offset / table from the compiler); a main can instead quantize the image once
//#### (VTAInputQuantize, the same bytes) and pass the int8 bytes in the float input's place, the pack then only moves bytes.
//#### VTA_INPUT_INT8 tells a main that this vtalib has it (vta/vtalib does not).
#define VTA_INPUT_INT8 1
// the quantize of a preprocess_conv0pack_stream call; lutHash = FNV-1a of the C x 256 table bytes (0 = no table)
struct VTAInputInt8 {
  int32_t N, H, W, C, nchw;
  float scale;
  int32_t offset;
  uint32_t lutHash;
};
// the quantize of the first preprocess_conv0pack_stream call of the process (after one float run): 1, else 0
int VTAInputInt8Info(VTAInputInt8 *d);
// in (the float input) -> out (N C H W bytes in the input's layout) with that call's quantize; 0 = none recorded yet
int VTAInputQuantize(const float *in, int8_t *out);
// this thread's next preprocess_conv0pack_stream calls read the int8 input made with d (nullptr: float again) from data
// (nullptr: from the call's input, the bytes in the float's place). A call whose quantize differs from d prints both and
// uses its float input if hasFloat (once per process), else exits 6.
//#### JO changed (2026-11-25): + data, hasFloat (the main's default int8 input with the float image as the fallback)
void VTASetInputInt8(const VTAInputInt8 *d, const int8_t *data = nullptr, int hasFloat = 0);
void preprocess_conv0pack_rows_i8(const int8_t *input, int8_t *output, int N, int H, int W, int C, int nchw, int KH,
                                  int KW, int stride, int pad, int CO, int q0, int q1, int padEnd = -1);
void preprocess_quantize_input(const float *input, int8_t *output, int N, int H, int W, int C, int nchw, float scale,
                               int32_t offset, const int8_t *lut);
#endif // VTALIB_SYSTOLIC_VTAINPUTINT8_H
