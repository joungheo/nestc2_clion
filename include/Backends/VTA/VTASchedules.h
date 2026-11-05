/*****************************************************************************
 *
 * Copyright Next-Generation System Software Research Group, All rights
 *reserved. Future Computing Research Division, Artificial Intelligence Reserch
 *Laboratory Electronics and Telecommunications Research Institute (ETRI)
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
struct ConvolutionTuneElement {
  unsigned_t inputN_;
  unsigned_t inputH_;
  unsigned_t inputW_;
  unsigned_t inputC_;
  unsigned_t filterN_;
  unsigned_t filterH_;
  unsigned_t filterW_;
  unsigned_t filterC_;
  unsigned_t stride_;
  unsigned_t pad_;
  unsigned_t nVirtualThread_;
  unsigned_t tileHSize_;
  unsigned_t tileWSize_;

  bool isMatch(unsigned_t inputN, unsigned_t inputH, unsigned_t inputW,
               unsigned_t inputC, unsigned_t filterN, unsigned_t filterH,
               unsigned_t filterW, unsigned_t filterC, unsigned_t stride,
               unsigned_t pad) {
    if (inputN_ == inputN && inputH_ == inputH && inputW_ == inputW &&
        inputC_ == inputC && filterN_ == filterN && filterH_ == filterH &&
        filterW_ == filterW && filterC_ == filterC && stride_ == stride &&
        pad_ == pad)
      return true;
    return false;
  }
};

struct ConvolutionTune {
  std::vector<ConvolutionTuneElement> ConvolutionTune_;
};

struct ConvolutionTune convTune;
convTune.ConvolutionTune_.clear();

// inputN, inputH, inputW, inputC, filterN, filterH, filterW, filterC, stride,
// pad, nVirtualThread, tileHSize, tileWSize
//#### JO added (2026-10-29): the image conv0 (3 channels zero-filled to 16, generateFirstBNNVTAConvolutionCall);
//#### its default 1, 8, 8 took 12.81 ms on the board (380 MHz), 2, 8, 16 takes 9.48 ms (same output).
//#### Input tile 21 x 37 = 777 entries fits one half of the inp SRAM (1024) of the multi-EVTA buffers too;
//#### 14 x 14 (9.30 ms) needs 33 x 33 = 1089, and tiles that do not divide 112 changed the result (16 x 11).
convTune.ConvolutionTune_.push_back({1, 224, 224, 16, 64, 7, 7, 16, 2, 3, 2, 8,
                                     16});
//#### JO added (2026-11-02): the same conv0 over its packed input (VTASave.cpp -vta-conv0-pack: 4 x 1 / stride 1 over
//#### 115 x 112 x 48): input tile 19 x 16 x 3 blocks = 912 entries (half of the multi-EVTA inp SRAM is 1024)
convTune.ConvolutionTune_.push_back({1, 115, 112, 48, 64, 4, 1, 48, 1, 0, 2, 16,
                                     16});
convTune.ConvolutionTune_.push_back({1, 56, 56, 64, 64, 3, 3, 64, 1, 1, 2, 8,
                                     56});
convTune.ConvolutionTune_.push_back({1, 56, 56, 64, 128, 1, 1, 64, 2, 0, 2, 7,
                                     28});
convTune.ConvolutionTune_.push_back({1, 56, 56, 64, 128, 3, 3, 64, 2, 1, 2, 8,
                                     28});
convTune.ConvolutionTune_.push_back({1, 28, 28, 128, 128, 3, 3, 128, 1, 1, 2,
                                     14, 28});
convTune.ConvolutionTune_.push_back({1, 28, 28, 128, 256, 1, 1, 128, 2, 0, 2, 7,
                                     14});
convTune.ConvolutionTune_.push_back({1, 28, 28, 128, 256, 3, 3, 128, 2, 1, 2,
                                     14, 14});
convTune.ConvolutionTune_.push_back({1, 14, 14, 256, 256, 3, 3, 256, 1, 1, 2,
                                     14, 14});
convTune.ConvolutionTune_.push_back({1, 14, 14, 256, 512, 3, 3, 256, 2, 0, 2, 7,
                                     7});
convTune.ConvolutionTune_.push_back({1, 14, 14, 256, 512, 1, 1, 256, 2, 0, 2, 7,
                                     7});
convTune.ConvolutionTune_.push_back({1, 14, 14, 256, 512, 3, 3, 256, 2, 1, 2, 7,
                                     7});
convTune.ConvolutionTune_.push_back({1, 7, 7, 512, 512, 3, 3, 512, 1, 1, 2, 7,
                                     7});
//#### JO added (2026-11-05): Resnet50 and VGG16 (CIFAR-10) shapes, measured on the board (systolic _rq bitstream, single EVTA):
//#### the bundle schedule replaced at run time (vtalib-zcu102-systolic EVTA_SCHED) by candidates nVirtualThread 1 / 2 x output
//#### tiles that divide the output, per-conv time from _sysprof, fastest kept; layer dumps byte-identical to the default
//#### schedule (1, 14, 14). Resnet50 EVTA 115.3 -> 78.1 ms, VGG16 15.2 -> 13.3 ms (docs/evta_runtime_compare.md 7.19).
//#### Not for outputs smaller than MIN_H_SIZE / MIN_W_SIZE (7, vtalib pads the tile to 7 x 7): nVirtualThread 2 there changes
//#### the result (VGG16 4 x 4 and 2 x 2 convs, every tile tried), nVirtualThread 1 with smaller tiles is right but not faster.
convTune.ConvolutionTune_.push_back({1, 56, 56, 64, 64, 1, 1, 64, 1, 0, 2, 8, 56});
convTune.ConvolutionTune_.push_back({1, 56, 56, 64, 256, 1, 1, 64, 1, 0, 2, 8, 56});
convTune.ConvolutionTune_.push_back({1, 56, 56, 256, 64, 1, 1, 256, 1, 0, 2, 8, 56});
convTune.ConvolutionTune_.push_back({1, 56, 56, 256, 128, 1, 1, 256, 1, 0, 2, 8, 56});
convTune.ConvolutionTune_.push_back({1, 56, 56, 128, 128, 3, 3, 128, 2, 1, 2, 7, 28});
convTune.ConvolutionTune_.push_back({1, 28, 28, 128, 512, 1, 1, 128, 1, 0, 2, 7, 28});
convTune.ConvolutionTune_.push_back({1, 56, 56, 256, 512, 1, 1, 256, 2, 0, 2, 7, 28});
convTune.ConvolutionTune_.push_back({1, 28, 28, 512, 128, 1, 1, 512, 1, 0, 2, 7, 28});
convTune.ConvolutionTune_.push_back({1, 28, 28, 512, 256, 1, 1, 512, 1, 0, 2, 7, 28});
convTune.ConvolutionTune_.push_back({1, 28, 28, 256, 256, 3, 3, 256, 2, 1, 2, 7, 14});
convTune.ConvolutionTune_.push_back({1, 14, 14, 256, 1024, 1, 1, 256, 1, 0, 2, 7, 14});
convTune.ConvolutionTune_.push_back({1, 28, 28, 512, 1024, 1, 1, 512, 2, 0, 2, 7, 14});
convTune.ConvolutionTune_.push_back({1, 14, 14, 1024, 256, 1, 1, 1024, 1, 0, 2, 7, 14});
convTune.ConvolutionTune_.push_back({1, 14, 14, 1024, 512, 1, 1, 1024, 1, 0, 2, 7, 14});
convTune.ConvolutionTune_.push_back({1, 14, 14, 512, 512, 3, 3, 512, 2, 1, 2, 7, 7});
convTune.ConvolutionTune_.push_back({1, 7, 7, 512, 2048, 1, 1, 512, 1, 0, 2, 7, 7});
convTune.ConvolutionTune_.push_back({1, 14, 14, 1024, 2048, 1, 1, 1024, 2, 0, 2, 7, 7});
convTune.ConvolutionTune_.push_back({1, 7, 7, 2048, 512, 1, 1, 2048, 1, 0, 2, 7, 7});
convTune.ConvolutionTune_.push_back({1, 34, 32, 16, 64, 3, 1, 16, 1, 0, 2, 16, 32});
convTune.ConvolutionTune_.push_back({1, 32, 32, 64, 64, 3, 3, 64, 1, 1, 2, 8, 32});
convTune.ConvolutionTune_.push_back({1, 16, 16, 64, 128, 3, 3, 64, 1, 1, 2, 16, 16});
convTune.ConvolutionTune_.push_back({1, 16, 16, 128, 128, 3, 3, 128, 1, 1, 2, 16, 16});
convTune.ConvolutionTune_.push_back({1, 8, 8, 128, 256, 3, 3, 128, 1, 1, 2, 8, 8});
convTune.ConvolutionTune_.push_back({1, 8, 8, 256, 256, 3, 3, 256, 1, 1, 2, 8, 8});
