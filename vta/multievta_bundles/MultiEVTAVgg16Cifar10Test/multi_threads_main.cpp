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

//#### JO added (2026-11-06): one bundle copy per EVTA in its own pthread for any image classifier (multi_model.h names
//#### the copies' entries, the input and the output, MultiEVTAResnet50Test / MultiEVTAVgg16Cifar10Test); the same as
//#### MultiEVTAResnet18Test/multi_threads_main.cpp: every copy runs MULTI_RUNS (default 25) inferences, the copies start
//#### each one together (barrier), MULTI_LOCKSTEP=1 also before every EVTA call (evta_lockstep_hook, timed builds).
//#### single_thread_main.cpp includes this file with MULTI_SINGLE: copy 0 alone, 4 x MULTI_RUNS inferences.
//#### The copies' weights are the same bytes; load_module shares their VTA buffers (vtalib VTAConstBufferAlloc).

#include <sys/time.h>
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
  const float scale = (hi - lo) / 255.0f;
  for (size_t r = 0; r < height; r++)
    for (size_t c = 0; c < width; c++) {
      png_byte *p = &rows[r][c * (hasAlpha ? numChannels + 1 : numChannels)];
      // the same element order as the single bundles' mains (getXYZ(dims, row, col, z), z = 2 - RGB index)
      for (size_t z = 0; z < numChannels; z++)
        imageT[(r * height + c) * numChannels + z] = float(p[isGray ? 0 : numChannels - 1 - z]) * scale + lo;
    }
  for (size_t y = 0; y < height; y++)
    free(rows[y]);
  png_destroy_read_struct(&png_ptr, &info_ptr, (png_infopp)NULL);
  fclose(fp);
  printf("Loaded image: %s\n", filename);
  return false;
}

/// The input tensor as the single bundles' mains make it: BGR, N x C x H x W (MODEL_NHWC: N x H x W x C)
static float *loadInput() {
  const size_t H = MODEL_HEIGHT, W = MODEL_WIDTH;
  float *imageT = nullptr;
  size_t dims[3];
  bool failed = readPngImage(inputImageFilenames[0].c_str(), MODEL_RANGE_LO, MODEL_RANGE_HI, imageT, dims);
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
  return t;
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
};

static void *threadMain(void *a) {
  ThreadArg *t = static_cast<ThreadArg *>(a);
  ModelCopy &m = copies[t->copy];
  m.initRuntime();
  m.load(t->constantWeights);
  float conf = 0;
  int idx = -1;
  for (int run = 0; run < t->runs; run++) {
#ifndef MULTI_SINGLE
    pthread_barrier_wait(&runBarrier);
#endif
    m.run(t->constantWeights, t->mutableWeights, t->activations);
    idx = classify(*m.config, t->mutableWeights, &conf);
    if (idx != MODEL_EXPECT) {
      // keep going: a thread that left would hang the others at the barrier
      std::cout << "copy " << t->copy << " run " << run << ": the result should be " << MODEL_EXPECT << ": " << idx << "\n";
      t->bad++;
    }
  }
  printf("copy %d: %d runs, Result: %d Confidence: %f, %d wrong\n", t->copy, t->runs, idx, conf, t->bad);
  m.destroy();
  m.destroyRuntime();
  return nullptr;
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++)
    inputImageFilenames.push_back(argv[i]);
  if (inputImageFilenames.empty()) {
    printf("usage: %s <image.png>\n", argv[0]);
    return 1;
  }
  float *input = loadInput();
#ifdef MULTI_SINGLE
  const int n = 1, runs = 4 * multiRuns();
#else
  const int n = nCopies, runs = multiRuns();
#endif
  std::vector<ThreadArg> args(n);
  for (int i = 0; i < n; i++) {
    const BundleConfig &c = *copies[i].config;
    args[i] = {i, runs, 0, initConstantWeights(copies[i].weights, c),
               static_cast<uint8_t *>(alignedAlloc(c, c.mutableWeightVarsMemSize)),
               static_cast<uint8_t *>(alignedAlloc(c, c.activationsMemSize))};
    const SymbolTableEntry &in = getMutableWeightVar(c, MODEL_INPUT);
    memcpy(args[i].mutableWeights + in.offset, input, 3 * MODEL_HEIGHT * MODEL_WIDTH * sizeof(float));
  }
  free(input);
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
  for (auto &a : args) {
    free(a.activations);
    free(a.constantWeights);
    free(a.mutableWeights);
  }
  return bad != 0;
}
