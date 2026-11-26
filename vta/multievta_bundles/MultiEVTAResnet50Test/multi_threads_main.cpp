/*****************************************************************************
*
* Copyright Next-Generation System Software Research Group, All rights reserved.
* Future Computing Research Division, Artificial Intelligence Reserch Laboratory
* Electronics and Telecommunications Research Institute (ETRI)
*
* THESE DOCUMENTS CONTAIN CONFIDENTIAL INFORMATION AND KNOWLEDGE
* WHICH IS THE PROPERTY OF ETRI. NO PART OF THIS PUBLICATION IS
* TO BE USED FOR ANY OTHER PURPOSE, AND THESE ARE NOT TO BE
* REPRODUCED, COPIED, DISCLOSED, TRANSMITTED, STORED IN A RETRIEVAL
* SYSTEM OR TRANSLATED INTO ANY OTHER HUMAN OR COMPUTER LANGUAGE,
* IN ANY FORM, BY ANY MEANS, IN WHOLE OR IN PART, WITHOUT THE
* COMPLETE PRIOR WRITTEN PERMISSION OF ETRI.
*
* LICENSE file : LICENSE_ETRI located in the top directory
*
*****************************************************************************/

//#### JO added (2026-11-09): DeiTTest copy of MobileNetV2Test/multi_threads_main.cpp with MODEL_MEAN / MODEL_STD (per channel
//#### normalization of the input) and MODEL_PRINT_TOP5.
//#### JO changed (2026-11-22): the one copy for every single-EVTA model main (DeiT, MobileNetV2, and the eval mains of Resnet18 /
//#### Resnet50 / VGG16, <x>EvalMain.cpp): the same file in each vta/bundles/<Dir>/, the model in its multi_model.h.
//#### JO added (2026-11-06): one bundle copy per EVTA in its own pthread for any image classifier (multi_model.h names
//#### the copies' entries, the input and the output, MultiEVTAResnet50Test / MultiEVTAVgg16Cifar10Test); the same as
//#### MultiEVTAResnet18Test/multi_threads_main.cpp: every copy runs MULTI_RUNS (default 25) inferences, the copies start
//#### each one together (barrier), MULTI_LOCKSTEP=1 also before every EVTA call (evta_lockstep_hook, timed builds).
//#### single_thread_main.cpp includes this file with MULTI_SINGLE: copy 0 alone, 4 x MULTI_RUNS inferences.
//#### The copies' weights are the same bytes; load_module shares their VTA buffers (vtalib VTAConstBufferAlloc).

#include <sys/time.h>
#include <unistd.h>   //#### JO added (2026-11-24): usleep (MULTI_STAGGER_US)
#include <sys/stat.h>   //#### JO added (2026-11-25): mkdir (INPUT_INT8_SAVE)
#include "VTARuntime.h"
#include <assert.h>
#include <inttypes.h>
#include <iostream>
#include <math.h>
#include <png.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>
#include <condition_variable>   //#### JO added (2026-11-27): INPUT_PIPELINE
#include <mutex>
#include <thread>

#include "multi_model.h"
#include "vta/runtime.h"

struct ModelCopy {
  BundleConfig *config;
  void (*load)(uint8_t *);
  void (*destroy)();
  int (*run)(uint8_t *, uint8_t *, uint8_t *);
  const char *weights;
  void (*initRuntime)();
  void (*destroyRuntime)();
};
#ifdef MULTI_SINGLE
static ModelCopy copies[] = {MODEL_COPY0};
#else
static ModelCopy copies[] = MODEL_COPIES;
#endif
static const int nCopies = sizeof(copies) / sizeof(copies[0]);
static std::vector<std::string> inputImageFilenames;

/// Reads a PNG image into a newly allocated H x W x C float array (values mapped to [lo, hi]).
static bool readPngImage(const char *filename, float lo, float hi, float *&imageT, size_t *imageDims) {
  unsigned char header[8];
  FILE *fp = fopen(filename, "rb");
  if (!fp || fread(header, 1, 8, fp) != 8 || png_sig_cmp(header, 0, 8))
    return true;
  png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info_ptr = png_ptr ? png_create_info_struct(png_ptr) : nullptr;
  if (!info_ptr || setjmp(png_jmpbuf(png_ptr)))
    return true;
  png_init_io(png_ptr, fp);
  png_set_sig_bytes(png_ptr, 8);
  png_read_info(png_ptr, info_ptr);
  size_t width = png_get_image_width(png_ptr, info_ptr);
  size_t height = png_get_image_height(png_ptr, info_ptr);
  int color_type = png_get_color_type(png_ptr, info_ptr);
  const bool isGray = color_type == PNG_COLOR_TYPE_GRAY;
  const size_t numChannels = isGray ? 1 : 3;
  const bool hasAlpha = color_type == PNG_COLOR_TYPE_RGB_ALPHA;
  assert(png_get_bit_depth(png_ptr, info_ptr) == 8 && "Invalid image");
  png_set_interlace_handling(png_ptr);
  png_read_update_info(png_ptr, info_ptr);
  std::vector<png_bytep> rows(height);
  for (size_t y = 0; y < height; y++)
    rows[y] = (png_byte *)malloc(png_get_rowbytes(png_ptr, info_ptr));
  png_read_image(png_ptr, rows.data());
  png_read_end(png_ptr, info_ptr);
  imageDims[0] = width;
  imageDims[1] = height;
  imageDims[2] = numChannels;
  imageT = static_cast<float *>(calloc(1, width * height * numChannels * sizeof(float)));
  const double scale = (hi - lo) / 255.0;  // double as in the single mains (the same float values)
  for (size_t r = 0; r < height; r++)
    for (size_t c = 0; c < width; c++) {
      png_byte *p = &rows[r][c * (hasAlpha ? numChannels + 1 : numChannels)];
      // the same element order as the single bundle's main (getXYZ(dims, row, col, z)): z = RGB index, or
      // 2 - RGB index with MODEL_PNG_REVERSE (the mains differ: Resnet50 keeps RGB, VGG16 / Resnet18 reverse it)
      for (size_t z = 0; z < numChannels; z++)
        imageT[(r * height + c) * numChannels + z] =
            float(p[isGray ? 0 : MODEL_PNG_REVERSE ? numChannels - 1 - z : z]) * scale + lo;
    }
  for (size_t y = 0; y < height; y++)
    free(rows[y]);
  png_destroy_read_struct(&png_ptr, &info_ptr, (png_infopp)NULL);
  fclose(fp);
  if (!getenv("EVAL_LIST")) printf("Loaded image: %s\n", filename);
  return false;
}

/// The input tensor as the single bundles' mains make it: BGR, N x C x H x W (MODEL_NHWC: N x H x W x C)
static float *loadInput(const char *filename) {
  const size_t H = MODEL_HEIGHT, W = MODEL_WIDTH;
  float *imageT = nullptr;
  size_t dims[3];
  bool failed = readPngImage(filename, MODEL_RANGE_LO, MODEL_RANGE_HI, imageT, dims);
  assert(!failed && dims[0] == W && dims[1] == H && "Error reading input image.");
  (void)failed;
  float *t = static_cast<float *>(malloc(3 * H * W * sizeof(float)));
  for (size_t z = 0; z < 3; z++)
    for (size_t y = 0; y < H; y++)
      for (size_t x = 0; x < W; x++) {
        // single mains: resultT[getXYZW(dims, 0, 2 - z, x, y)] = imageT[getXYZ(dims, x, y, z)]
        float v = imageT[(x * H + y) * 3 + z];
        if (MODEL_NHWC)
          t[(x * W + y) * 3 + (2 - z)] = v;
        else
          t[((2 - z) * H + x) * W + y] = v;
      }
  free(imageT);
#ifdef MODEL_MEAN
  //#### JO added (2026-11-09): per channel (x - mean) / std after the [lo, hi] mapping (DeiT: ImageNet mean / std, NCHW)
  {
    const float mean[3] = MODEL_MEAN, sd[3] = MODEL_STD;
    for (size_t c = 0; c < 3; c++)
      for (size_t i = 0; i < H * W; i++) {
        float &v = MODEL_NHWC ? t[i * 3 + c] : t[c * H * W + i];
        v = (v - mean[c]) / sd[c];
      }
  }
#endif
  return t;
}

//#### JO added (2026-11-25): int8 input (vtalib-zcu102-systolic VTA_INPUT_INT8, docs/evta_dpu_compare.md 6.10). An int8 input
//#### (<name>.i8, made by INPUT_INT8_SAVE: InputI8Header, then N x C x H x W bytes in the bundle input's layout) is handed to
//#### vtalib (VTASetInputInt8) and the bundle's conv0 pack reads it instead of quantizing the float input.
//#### JO changed (2026-11-25): int8 is the default. For an image <dir>/<name>.png the main looks for <name>.i8 in
//#### $INPUT_INT8_DIR, then in <dir>, then in ./i8 (the executable's Dir: run_prof.sh runs there); none found: the float
//#### image (INPUT_INT8=0: always float). The float image is passed too: an .i8 made with another quantize than the
//#### bundle's (another bundle) is reported once by vtalib and the float image is used. A path that is itself an .i8 has
//#### no float image (exit 6 on a mismatch).
struct Input {
  void *data;        // the float image (nullptr: an .i8 path)
  size_t bytes;      // its bytes
  bool i8, hasFloat;  // hasFloat: the copies' input variables hold the float image (data may be freed by then)
  int8_t *i8data;    // the int8 input (i8)
  std::string from;  // what was loaded, for the log
#ifdef VTA_INPUT_INT8
  VTAInputInt8 d;
#endif
};
static bool isI8Path(const char *p) {
  const size_t n = strlen(p);
  return n > 3 && !strcmp(p + n - 3, ".i8");
}
#ifdef VTA_INPUT_INT8
struct InputI8Header {
  char magic[8];   // "EVTAI8" + 2 zero bytes
  VTAInputInt8 d;
};
// reads an .i8 into x (false: no such file); a file that is not an int8 input of this model's size exits
static bool readI8(const char *filename, Input &x) {
  FILE *fp = fopen(filename, "rb");
  if (!fp)
    return false;
  InputI8Header h;
  if (fread(&h, sizeof(h), 1, fp) != 1 || memcmp(h.magic, "EVTAI8\0\0", 8)) {
    fprintf(stderr, "%s: not an int8 input (INPUT_INT8_SAVE)\n", filename);
    exit(1);
  }
  const size_t n = (size_t)h.d.N * h.d.C * h.d.H * h.d.W;
  if (n != 3 * MODEL_HEIGHT * MODEL_WIDTH) {
    fprintf(stderr, "%s: %zu bytes, the model input has %d\n", filename, n, 3 * MODEL_HEIGHT * MODEL_WIDTH);
    exit(1);
  }
  x.i8data = static_cast<int8_t *>(malloc(n));
  if (fread(x.i8data, n, 1, fp) != 1) {
    fprintf(stderr, "%s: short file\n", filename);
    exit(1);
  }
  fclose(fp);
  x.i8 = true;
  x.d = h.d;
  x.from = filename;
  return true;
}
#endif
static Input loadAny(const char *filename) {
  Input x = {nullptr, 3 * MODEL_HEIGHT * MODEL_WIDTH * sizeof(float), false, false, nullptr, filename};
  const bool quiet = getenv("EVAL_LIST") != nullptr;
  if (isI8Path(filename)) {
#ifdef VTA_INPUT_INT8
    if (!readI8(filename, x)) {
      fprintf(stderr, "cannot open %s\n", filename);
      exit(1);
    }
    if (!quiet) printf("Input: int8 %s (scale %g offset %d), no float image\n", filename, x.d.scale, x.d.offset);
    return x;
#else
    fprintf(stderr, "%s: int8 inputs need vtalib-zcu102-systolic (VTA_INPUT_INT8)\n", filename);
    exit(1);
#endif
  }
  x.data = loadInput(filename);
  x.hasFloat = true;
#ifdef VTA_INPUT_INT8
  const char *e = getenv("INPUT_INT8");
  //#### JO added (2026-11-27): INPUT_PIPELINE=1 quantizes the float image in a second thread (no .i8 read)
  const char *pl = getenv("INPUT_PIPELINE");
  if (pl && atoi(pl) > 0) {
    if (!quiet)
      printf("Input: float %s, INPUT_PIPELINE=%d: quantized %s\n", filename, atoi(pl),
             atoi(pl) == 2 ? "in the copy's EVTA waits" : "in a second thread per copy");
    return x;
  }
  if (!e || atoi(e) != 0) {
    const std::string p = filename;
    const size_t sl = p.rfind('/'), b = sl == std::string::npos ? 0 : sl + 1, dot = p.rfind('.');
    const std::string name = p.substr(b, (dot == std::string::npos || dot < b ? p.size() : dot) - b) + ".i8";
    std::vector<std::string> cand;
    if (const char *dir = getenv("INPUT_INT8_DIR"))
      cand.push_back(std::string(dir) + "/" + name);
    cand.push_back(p.substr(0, b) + name);
    cand.push_back("i8/" + name);
    for (auto &c : cand)
      if (readI8(c.c_str(), x))
        break;
  }
  if (!quiet) {
    if (x.i8)
      printf("Input: int8 %s (scale %g offset %d; float image %s kept for a mismatch)\n", x.from.c_str(), x.d.scale,
             x.d.offset, filename);
    else
      printf("Input: float %s (%s)\n", filename,
             e && atoi(e) == 0 ? "INPUT_INT8=0" : "no <name>.i8 in $INPUT_INT8_DIR, the image's dir or ./i8");
  }
#endif
  return x;
}
// the input into a copy's input variable: the float image if there is one (the int8 input goes to vtalib)
static void copyInput(const Input &x, uint8_t *dst) {
  if (x.data)
    memcpy(dst, x.data, x.bytes);
}
static void freeInput(Input &x) {
  free(x.data);
  free(x.i8data);
  x.data = nullptr;
  x.i8data = nullptr;
}
static void setInputMode(const Input &x) {
#ifdef VTA_INPUT_INT8
  VTASetInputInt8(x.i8 ? &x.d : nullptr, x.i8data, x.hasFloat);
#else
  (void)x;
#endif
}

static const SymbolTableEntry &getMutableWeightVar(const BundleConfig &config, const char *name) {
  for (unsigned i = 0; i < config.numSymbols; ++i)
    if (!strncmp(config.symbolTable[i].name, name, strlen(name)))
      return config.symbolTable[i];
  printf("Expected to find variable '%s'\n", name);
  exit(1);
}

static void *alignedAlloc(const BundleConfig &config, size_t size) {
  void *ptr = nullptr;
  if (posix_memalign(&ptr, config.alignment, size) != 0)
    exit(1);
  memset(ptr, 0, size);
  return ptr;
}

static uint8_t *initConstantWeights(const char *weightsFileName, const BundleConfig &config) {
  FILE *f = fopen(weightsFileName, "rb");
  if (!f) {
    fprintf(stderr, "Could not open the weights file: %s\n", weightsFileName);
    exit(1);
  }
  fseek(f, 0, SEEK_END);
  size_t fileSize = ftell(f);
  fseek(f, 0, SEEK_SET);
  assert(fileSize == config.constantWeightVarsMemSize && "Wrong weights file size");
  uint8_t *p = static_cast<uint8_t *>(alignedAlloc(config, fileSize));
  if (fread(p, fileSize, 1, f) != 1)
    perror("Could not read the weights file");
  else
    printf("Loaded weights of size: %lu from the file %s\n", fileSize, weightsFileName);
  fclose(f);
  return p;
}

/// argmax of the output (after a softmax when the graph has none, MODEL_SOFTMAX), its probability in *conf
static int classify(const BundleConfig &config, uint8_t *mutableWeightVars, float *conf) {
  const SymbolTableEntry &out = getMutableWeightVar(config, MODEL_OUTPUT);
  const float *r = (const float *)(mutableWeightVars + out.offset);
  const int n = (int)out.size;
  std::vector<float> p(r, r + n);
  if (MODEL_SOFTMAX) {
    float m = -INFINITY, sum = 0;
    for (float v : p) m = std::max(m, v);
    for (float &v : p) sum += (v = expf(v - m));
    for (float &v : p) v /= sum;
  }
  int best = 0;
  for (int i = 1; i < n; i++)
    if (p[i] > p[best]) best = i;
  *conf = p[best];
#ifdef MODEL_PRINT_TOP5
  //#### JO added (2026-11-09): the 5 largest outputs (before the softmax) once, to compare with image-classifier
  static bool printed = false;
  if (!printed) {
    printed = true;
    std::vector<int> ix(n);
    for (int i = 0; i < n; i++) ix[i] = i;
    std::partial_sort(ix.begin(), ix.begin() + 5, ix.end(), [&](int a, int b) { return r[a] > r[b]; });
    for (int k = 0; k < 5; k++) printf("  top%d: %d  %f\n", k + 1, ix[k], r[ix[k]]);
  }
#endif
  return best;
}

static int multiRuns() {
  const char *e = getenv("MULTI_RUNS");
  return e && atoi(e) > 0 ? atoi(e) : 25;
}

static pthread_barrier_t runBarrier, convBarrier;
static bool lockstepOn = false;
extern "C" void evta_lockstep_hook() {
  if (lockstepOn) pthread_barrier_wait(&convBarrier);
}

struct ThreadArg {
  int copy, runs, bad;
  uint8_t *constantWeights, *mutableWeights, *activations;
  double t1, tEnd;   //#### JO added (2026-11-24): start of run 2 and end of the last run (ms, MULTI_FREE throughput)
  const Input *input;   //#### JO added (2026-11-25)
  const float *plSrc;   //#### JO added (2026-11-27): INPUT_PIPELINE: this copy's float image (nullptr: off)
};

//#### JO added (2026-11-24): MULTI_FREE=1: the copies do not wait for each other before each inference (only once before
//#### the first), as independent streams (DPU "multi thread"); MULTI_STAGGER_US=<us>: copy j starts j * us later.
//#### main prints the throughput from the start of everyone's 2nd run to the last end: images / time.
static bool multiFree() {
  const char *e = getenv("MULTI_FREE");
  return e && atoi(e) > 0;
}
static double nowMs() {
  timeval t;
  gettimeofday(&t, NULL);
  return t.tv_sec * 1000.0 + t.tv_usec / 1000.0;
}

#ifdef VTA_INPUT_INT8
//#### JO added (2026-11-27): INPUT_PIPELINE=1: the input quantize of the next image runs in a second thread per copy while the
//#### copy's EVTA calls run (as a camera / decode pipeline would hand the next image over). Run 0 is float (the bundle's pack
//#### records the quantize), run 1's int8 input is made before the run (outside the timed runs 1 ..), then before run k the
//#### copy takes buffer k & 1 (waiting if the thread is not done: that wait is in the time) and asks for run k + 1 in the other
//#### buffer. The thread quantizes the same float image every time (VTAInputQuantize, the bytes of the float pack) from its
//#### own copy of it. Same output bytes as float and .i8; the float image stays in the bundle input (hasFloat).
struct QuantPipe {
  QuantPipe(const float *src, size_t n) : src(src) {
    buf[0].resize(n);
    buf[1].resize(n);
    th = std::thread([this] { loop(); });
  }
  ~QuantPipe() {
    {
      std::lock_guard<std::mutex> g(mu);
      stop = true;
    }
    cv.notify_all();
    th.join();
  }
  void request(int k) {
    {
      std::lock_guard<std::mutex> g(mu);
      want = k;
    }
    cv.notify_all();
  }
  const int8_t *wait(int k) {
    const double t0 = nowMs();
    std::unique_lock<std::mutex> l(mu);
    cv.wait(l, [&] { return done >= k; });
    waitMs += nowMs() - t0;
    return buf[k & 1].data();
  }
  void loop() {
    std::unique_lock<std::mutex> l(mu);
    for (;;) {
      cv.wait(l, [&] { return stop || want > done; });
      if (stop)
        return;
      const int k = want;
      l.unlock();
      const double t0 = nowMs();
      VTAInputQuantize(src, buf[k & 1].data());
      const double t1 = nowMs();
      l.lock();
      quantMs += t1 - t0;
      count++;
      done = k;
      cv.notify_all();
    }
  }
  const float *src;
  std::vector<int8_t> buf[2];
  std::mutex mu;
  std::condition_variable cv;
  std::thread th;
  int want = -1, done = -1, count = 0;
  bool stop = false;
  double quantMs = 0, waitMs = 0;   // the thread's quantize time, the copy's wait for it (runs 2 ..)
};
//#### JO added (2026-11-27): INPUT_PIPELINE=2: no second thread (with 4 copies on 4 cores the threads took the copies' cores:
//#### slower than float). The copy quantizes the next image itself, one input row per poll of its EVTA run waits
//#### (VTASetWaitWork, pynq driver); rows left when the run ends are done before the next run (in the time).
struct WaitQuant {
  WaitQuant(const float *src, size_t n, int rows) : src(src), rows(rows) {
    buf[0].resize(n);
    buf[1].resize(n);
  }
  static int step(void *a) {
    WaitQuant *w = static_cast<WaitQuant *>(a);
    if (w->next >= w->rows)
      return 0;
    VTAInputQuantizeRows(w->src, w->buf[w->k & 1].data(), w->next, w->next + 1);
    return ++w->next < w->rows;
  }
  void start(int kk) {
    k = kk;
    next = 0;
    VTASetWaitWork(step, this);
  }
  const int8_t *finish() {
    VTASetWaitWork(nullptr, nullptr);
    if (next < rows) {
      const double t0 = nowMs();
      left += rows - next;
      VTAInputQuantizeRows(src, buf[k & 1].data(), next, rows);
      next = rows;
      finishMs += nowMs() - t0;
    }
    return buf[k & 1].data();
  }
  const float *src;
  std::vector<int8_t> buf[2];
  int rows, next = 0, k = -1, left = 0;   // left: rows quantized after the run instead of in its waits (runs 2 ..)
  double finishMs = 0;
};
#endif

static void *threadMain(void *a) {
  ThreadArg *t = static_cast<ThreadArg *>(a);
  ModelCopy &m = copies[t->copy];
  m.initRuntime();
  m.load(t->constantWeights);
  float conf = 0;
  int idx = -1;
  const bool freeRun = multiFree();
  const char *sg = getenv("MULTI_STAGGER_US");
  const int stagger = sg ? atoi(sg) : 0;
  setInputMode(*t->input);   //#### JO added (2026-11-25): int8 input
#ifdef VTA_INPUT_INT8
  QuantPipe *pipe = nullptr;
  WaitQuant *wq = nullptr;
  VTAInputInt8 pd;
  bool pl = t->plSrc != nullptr;
  const int plMode = pl ? atoi(getenv("INPUT_PIPELINE")) : 0;
#endif
  for (int run = 0; run < t->runs; run++) {
#ifdef VTA_INPUT_INT8
    if (pl && run >= 1) {
      if (!pipe && !wq) {   // the quantize is known after the first (float) run; run 1's input made here, before the timing
        if (!VTAInputInt8Info(&pd)) {
          printf("INPUT_PIPELINE: this bundle has no conv0 pack (preprocess_conv0pack_stream), float input\n");
          pl = false;
        } else if (plMode == 2) {
          wq = new WaitQuant(t->plSrc, (size_t)pd.N * pd.C * pd.H * pd.W, pd.N * pd.H);
          wq->start(1);
          wq->finish();
          wq->left = 0, wq->finishMs = 0;
        } else {
          pipe = new QuantPipe(t->plSrc, (size_t)pd.N * pd.C * pd.H * pd.W);
          pipe->request(1);
          pipe->wait(1);
          pipe->waitMs = 0;
        }
      }
      if (wq) {
        if (run > 1)
          wq->finish();   // image run, started in the waits of run - 1
        VTASetInputInt8(&pd, wq->buf[run & 1].data(), 1);
        if (run + 1 < t->runs)
          wq->start(run + 1);
      }
      if (pipe) {
        VTASetInputInt8(&pd, pipe->wait(run), 1);
        if (run + 1 < t->runs)
          pipe->request(run + 1);
      }
    }
#endif
#ifndef MULTI_SINGLE
    //pthread_barrier_wait(&runBarrier);
    if (!freeRun || run == 0)
      pthread_barrier_wait(&runBarrier);
    if (run == 0 && stagger > 0)
      usleep(stagger * t->copy);
#endif
    if (run == 1)
      t->t1 = nowMs();
    m.run(t->constantWeights, t->mutableWeights, t->activations);
    idx = classify(*m.config, t->mutableWeights, &conf);
    if (idx != MODEL_EXPECT) {
      // keep going: a thread that left would hang the others at the barrier
      std::cout << "copy " << t->copy << " run " << run << ": the result should be " << MODEL_EXPECT << ": " << idx << "\n";
      t->bad++;
    }
  }
  t->tEnd = nowMs();
  printf("copy %d: %d runs, Result: %d Confidence: %f, %d wrong\n", t->copy, t->runs, idx, conf, t->bad);
#ifdef VTA_INPUT_INT8
  if (wq) {
    VTASetWaitWork(nullptr, nullptr);
    printf("INPUT_PIPELINE=2 copy %d: %d images quantized in the EVTA waits; %d of their %d rows after the run (%.3f ms in all)\n",
           t->copy, t->runs - 2, wq->left, (t->runs - 2) * wq->rows, wq->finishMs);
    delete wq;
  }
  if (pipe) {
    const int c = pipe->count;
    const double q = pipe->quantMs, w = pipe->waitMs;
    delete pipe;
    printf("INPUT_PIPELINE copy %d: %d quantizes in the second thread, %.3f ms each; the copy waited %.3f ms in runs 2 .. %d\n",
           t->copy, c, c ? q / c : 0.0, w, t->runs - 1);
  }
#endif
  m.destroy();
  m.destroyRuntime();
  return nullptr;
}

#ifdef VTA_INPUT_INT8
//#### JO added (2026-11-25): INPUT_INT8_SAVE=<dir>: copy 0 runs the first image once as float (vtalib records the bundle's
//#### quantize), then every image (the arguments, or the EVAL_LIST images) is quantized by vtalib (VTAInputQuantize, the
//#### bytes the bundle's pack makes) and written to <dir>/<name>.i8; with EVAL_LIST also <dir>/eval_list.txt (the .i8 paths
//#### and the labels, for EVAL_LIST). Then exits. INPUT_INT8_SAVE=i8 in the executable's Dir makes the files the default
//#### int8 input of that Dir's runs (loadAny looks in ./i8).
static int saveInt8Inputs(const char *dir) {
  std::vector<std::pair<std::string, int>> list;
  if (const char *el = getenv("EVAL_LIST")) {
    FILE *lf = fopen(el, "r");
    char path[4096];
    int label;
    while (lf && fscanf(lf, "%4095s %d", path, &label) == 2)
      list.push_back({path, label});
    if (lf) fclose(lf);
  } else {
    for (auto &p : inputImageFilenames)
      list.push_back({p, -1});
  }
  if (list.empty() || isI8Path(list[0].first.c_str())) {
    printf("INPUT_INT8_SAVE: no .png images\n");
    return 1;
  }
  ModelCopy &m = copies[0];
  const BundleConfig &c = *m.config;
  uint8_t *cw = initConstantWeights(m.weights, c);
  uint8_t *mw = static_cast<uint8_t *>(alignedAlloc(c, c.mutableWeightVarsMemSize));
  uint8_t *act = static_cast<uint8_t *>(alignedAlloc(c, c.activationsMemSize));
  const SymbolTableEntry &in = getMutableWeightVar(c, MODEL_INPUT);
  m.initRuntime();
  m.load(cw);
  float *x = loadInput(list[0].first.c_str());
  memcpy(mw + in.offset, x, 3 * MODEL_HEIGHT * MODEL_WIDTH * sizeof(float));
  free(x);
  m.run(cw, mw, act);
  InputI8Header h;
  memset(&h, 0, sizeof(h));
  memcpy(h.magic, "EVTAI8", 6);
  if (!VTAInputInt8Info(&h.d)) {
    printf("INPUT_INT8_SAVE: this bundle has no conv0 pack (preprocess_conv0pack_stream), no int8 input\n");
    return 1;
  }
  const size_t n = (size_t)h.d.N * h.d.C * h.d.H * h.d.W;
  std::vector<int8_t> q(n);
  mkdir(dir, 0775);   // one level; an existing dir is fine
  FILE *lo = nullptr;
  if (getenv("EVAL_LIST"))
    lo = fopen((std::string(dir) + "/eval_list.txt").c_str(), "w");
  int done = 0;
  for (auto &e : list) {
    const std::string &p = e.first;
    const size_t sl = p.rfind('/'), b = sl == std::string::npos ? 0 : sl + 1, dot = p.rfind('.');
    const std::string out = std::string(dir) + "/" + p.substr(b, (dot == std::string::npos || dot < b ? p.size() : dot) - b) + ".i8";
    x = loadInput(p.c_str());
    VTAInputQuantize(x, q.data());
    free(x);
    FILE *fo = fopen(out.c_str(), "wb");
    if (!fo || fwrite(&h, sizeof(h), 1, fo) != 1 || fwrite(q.data(), n, 1, fo) != 1) {
      printf("INPUT_INT8_SAVE: cannot write %s\n", out.c_str());
      return 1;
    }
    fclose(fo);
    if (lo) fprintf(lo, "%s %d\n", out.c_str(), e.second);
    done++;
  }
  if (lo) fclose(lo);
  printf("INPUT_INT8_SAVE %d images to %s: N %d C %d H %d W %d nchw %d scale %g offset %d lut %08x\n", done, dir, h.d.N,
         h.d.C, h.d.H, h.d.W, h.d.nchw, h.d.scale, h.d.offset, h.d.lutHash);
  m.destroy();
  m.destroyRuntime();
  free(cw);
  free(mw);
  free(act);
  return 0;
}
#endif

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++)
    inputImageFilenames.push_back(argv[i]);
#ifdef VTA_INPUT_INT8
  if (const char *sd = getenv("INPUT_INT8_SAVE"))
    return saveInt8Inputs(sd);
#endif
  if (inputImageFilenames.empty() && !getenv("EVAL_LIST")) {
    printf("usage: %s <image.png>\n", argv[0]);
    return 1;
  }
#ifndef MODEL_LABEL_OFFSET
#define MODEL_LABEL_OFFSET 0
#endif
#ifdef MULTI_SINGLE
  //#### JO added (2026-11-10): accuracy over a labeled image set. EVAL_LIST=<file> with one "<image.png> <class>" per line
  //#### (vtabundles_xcmpl/imagenet_eval/): copy 0 loads its weights once and classifies every image; prints one
  //#### "EV <n> <pred> <class>" line per image (EVAL_QUIET=1: only every 500th) and the top-1 count at the end.
  if (const char *el = getenv("EVAL_LIST")) {
    FILE *lf = fopen(el, "r");
    if (!lf) {
      printf("cannot open EVAL_LIST %s\n", el);
      return 1;
    }
    const bool quiet = getenv("EVAL_QUIET") && atoi(getenv("EVAL_QUIET")) > 0;
    ModelCopy &m = copies[0];
    const BundleConfig &c = *m.config;
    uint8_t *cw = initConstantWeights(m.weights, c);
    uint8_t *mw = static_cast<uint8_t *>(alignedAlloc(c, c.mutableWeightVarsMemSize));
    uint8_t *act = static_cast<uint8_t *>(alignedAlloc(c, c.activationsMemSize));
    const SymbolTableEntry &in = getMutableWeightVar(c, MODEL_INPUT);
    m.initRuntime();
    m.load(cw);
    char path[4096];
    int label, n = 0, ok = 0;
    timeval t1, t2;
    gettimeofday(&t1, NULL);
    while (fscanf(lf, "%4095s %d", path, &label) == 2) {
      //#### JO changed (2026-11-25): .png (float) or .i8 (int8) per line
      Input x = loadAny(path);
      copyInput(x, mw + in.offset);
      setInputMode(x);
      m.run(cw, mw, act);
      float conf = 0;
      const int pred = classify(c, mw, &conf);
      //#### JO added (2026-11-22): MODEL_LABEL_OFFSET (multi_model.h) = model class - list class (MobileNetV2: 1, class 0 is
      //#### the background); the EV line prints the list class
      ok += pred == label + MODEL_LABEL_OFFSET;
      n++;
      if (!quiet || n % 500 == 0)
        printf("EV %d %d %d  top1 %d/%d\n", n, pred, label, ok, n);
      //#### JO added (2026-11-16): EVAL_HASH=1 prints a hash (FNV-1a) of the output's bytes per image, to compare two
      //#### builds byte for byte over an image set (prune_rows_onnx.py)
      static const bool hashOut = getenv("EVAL_HASH") && atoi(getenv("EVAL_HASH")) > 0;
      if (hashOut) {
        const SymbolTableEntry &out = getMutableWeightVar(c, MODEL_OUTPUT);
        const uint8_t *ob = mw + out.offset;
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < out.size * sizeof(float); i++) h = (h ^ ob[i]) * 1099511628211ull;
        printf("EH %d %016llx\n", n, (unsigned long long)h);
      }
      freeInput(x);
    }
    gettimeofday(&t2, NULL);
    fclose(lf);
    printf("EVAL top1 %d / %d = %.2f %%  (%.1f s)\n", ok, n, n ? 100.0 * ok / n : 0.0,
           (t2.tv_sec - t1.tv_sec) + (t2.tv_usec - t1.tv_usec) / 1e6);
    m.destroy();
    m.destroyRuntime();
    return 0;
  }
#endif
  Input input = loadAny(inputImageFilenames[0].c_str());
#ifdef MULTI_SINGLE
  //#### JO changed (2026-11-10): with BUNDLE_RUNS=<n> (run_prof.sh -n, the _timed / _prof builds: the timed_bundle.py wrapper runs
  //#### the first call n - 1 times) two calls here, so n inferences in all (it was 4 x MULTI_RUNS + n - 2 = 123 for n = 25)
  // const int runs = 4 * multiRuns();
  const char *br = getenv("BUNDLE_RUNS");
  const int runs = br && atoi(br) > 2 ? 2 : 4 * multiRuns();
  std::vector<int> sel = {0};
#else
  //#### MULTI_MASK=<bits>: only these copies (copy i = EVTA i, e.g. 6 = EVTAs 1 and 2 at once), default all
  const char *mk = getenv("MULTI_MASK");
  const int mask = mk && *mk ? (int)strtol(mk, nullptr, 0) : (1 << nCopies) - 1, runs = multiRuns();
  std::vector<int> sel;
  for (int i = 0; i < nCopies; i++)
    if (mask >> i & 1) sel.push_back(i);
  printf("MULTI_MASK 0x%x: %zu copies\n", mask, sel.size());
#endif
  const int n = (int)sel.size();
  std::vector<ThreadArg> args(n);
  for (int j = 0; j < n; j++) {
    const int i = sel[j];
    const BundleConfig &c = *copies[i].config;
    args[j] = {i, runs, 0, initConstantWeights(copies[i].weights, c),
               static_cast<uint8_t *>(alignedAlloc(c, c.mutableWeightVarsMemSize)),
               static_cast<uint8_t *>(alignedAlloc(c, c.activationsMemSize)), 0, 0, &input, nullptr};
    const SymbolTableEntry &in = getMutableWeightVar(c, MODEL_INPUT);
    copyInput(input, args[j].mutableWeights + in.offset);
#ifdef VTA_INPUT_INT8
    const char *pl = getenv("INPUT_PIPELINE");
    if (pl && atoi(pl) > 0 && input.data) {   //#### JO added (2026-11-27): each copy's thread quantizes its own copy
      float *f = static_cast<float *>(malloc(input.bytes));
      memcpy(f, input.data, input.bytes);
      args[j].plSrc = f;
    }
#endif
  }
  free(input.data);   // the int8 input stays: vtalib reads it at every run (freed at the end)
  input.data = nullptr;
  pthread_barrier_init(&runBarrier, NULL, n);
  const char *e = getenv("MULTI_LOCKSTEP");
  lockstepOn = n > 1 && e && atoi(e) > 0;
  pthread_barrier_init(&convBarrier, NULL, n);
  if (lockstepOn)
    std::cout << "MULTI_LOCKSTEP: the " << n << " copies start every EVTA call together\n";
  timeval t1, t2;
  gettimeofday(&t1, NULL);
  std::vector<pthread_t> th(n);
  for (int i = 0; i < n; i++)
    pthread_create(&th[i], NULL, threadMain, &args[i]);
  int bad = 0;
  for (int i = 0; i < n; i++) {
    pthread_join(th[i], NULL);
    bad += args[i].bad;
  }
  gettimeofday(&t2, NULL);
  std::cout << "Inference time: " << (t2.tv_sec - t1.tv_sec) * 1000.0 + (t2.tv_usec - t1.tv_usec) / 1000.0 << "ms\n";
  //#### JO added (2026-11-24): throughput over runs 2..runs of every copy (MULTI_FREE / MULTI_STAGGER_US compare)
  if (runs > 1) {
    double a0 = 1e300, a1 = 0;
    for (auto &a : args) a0 = std::min(a0, a.t1), a1 = std::max(a1, a.tEnd);
    const int imgs = n * (runs - 1);
    printf("THROUGHPUT %d copies%s stagger %s us: %d images in %.3f ms = %.1f images/s (%.3f ms per image per copy)\n", n,
           multiFree() ? " free" : " barrier", getenv("MULTI_STAGGER_US") ? getenv("MULTI_STAGGER_US") : "0", imgs,
           a1 - a0, imgs * 1000.0 / (a1 - a0), (a1 - a0) * n / imgs);
  }
  for (auto &a : args) {
    free(a.activations);
    free(a.constantWeights);
    free(a.mutableWeights);
    free(const_cast<float *>(a.plSrc));
  }
  freeInput(input);
  return bad != 0;
}
