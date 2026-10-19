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

#define VTAMAIN

#include "main_template.h"

#ifdef VTAMAIN
#include "VTARuntime.h"
#endif

#include "p0.h"
#include "p1.h"
#include "p2.h"
#include "p3.h"
#include "p4.h"
#include "p5.h"
#include "p6.h"
#include "p7.h"
#include "p8.h"
#include "p9.h"
#include "p10.h"
#include "p11.h"
#include "p12.h"
#include "p13.h"
#include "p14.h"
#include "p15.h"
#include "p16.h"
#include "p17.h"

#include <time.h>

int main(int argc, char **argv) {
	parseCommandLineOptions(argc, argv);

#ifdef VTAMAIN
	initVTARuntime();
#endif
	//======== initialize variables =======//

	uint8_t *constantWeightVarsAddr0 =
		initConstantWeights("p0.weights.bin", p0_config);
	uint8_t *mutableWeightVarsAddr0 = initMutableWeightVars(p0_config, "data");
	uint8_t *activationsAddr0 = initActivations(p0_config);

	uint8_t *constantWeightVarsAddr1 =
		initConstantWeights("p1.weights.bin", p1_config);
	uint8_t *activationsAddr1 = initActivations(p1_config);

	uint8_t *constantWeightVarsAddr2 =
		initConstantWeights("p2.weights.bin", p2_config);
	uint8_t *activationsAddr2 = initActivations(p2_config);

	uint8_t *constantWeightVarsAddr3 =
		initConstantWeights("p3.weights.bin", p3_config);
	uint8_t *activationsAddr3 = initActivations(p3_config);

	uint8_t *constantWeightVarsAddr4 =
		initConstantWeights("p4.weights.bin", p4_config);
	uint8_t *activationsAddr4 = initActivations(p4_config);

	uint8_t *constantWeightVarsAddr5 =
		initConstantWeights("p5.weights.bin", p5_config);
	uint8_t *activationsAddr5 = initActivations(p5_config);

	uint8_t *constantWeightVarsAddr6 =
		initConstantWeights("p6.weights.bin", p6_config);
	uint8_t *activationsAddr6 = initActivations(p6_config);

	uint8_t *constantWeightVarsAddr7 =
		initConstantWeights("p7.weights.bin", p7_config);
	uint8_t *activationsAddr7 = initActivations(p7_config);

	uint8_t *constantWeightVarsAddr8 =
		initConstantWeights("p8.weights.bin", p8_config);
	uint8_t *activationsAddr8 = initActivations(p8_config);

	uint8_t *constantWeightVarsAddr9 =
		initConstantWeights("p9.weights.bin", p9_config);
	uint8_t *activationsAddr9 = initActivations(p9_config);

	uint8_t *constantWeightVarsAddr10 =
		initConstantWeights("p10.weights.bin", p10_config);
	uint8_t *activationsAddr10 = initActivations(p10_config);

	uint8_t *constantWeightVarsAddr11 =
		initConstantWeights("p11.weights.bin", p11_config);
	uint8_t *activationsAddr11 = initActivations(p11_config);

	uint8_t *constantWeightVarsAddr12 =
		initConstantWeights("p12.weights.bin", p12_config);
	uint8_t *activationsAddr12 = initActivations(p12_config);

	uint8_t *constantWeightVarsAddr13 =
		initConstantWeights("p13.weights.bin", p13_config);
	uint8_t *activationsAddr13 = initActivations(p13_config);

	uint8_t *constantWeightVarsAddr14 =
		initConstantWeights("p14.weights.bin", p14_config);
	uint8_t *activationsAddr14 = initActivations(p14_config);

	uint8_t *constantWeightVarsAddr15 =
		initConstantWeights("p15.weights.bin", p15_config);
	uint8_t *activationsAddr15 = initActivations(p15_config);

	uint8_t *constantWeightVarsAddr16 =
		initConstantWeights("p16.weights.bin", p16_config);
	uint8_t *activationsAddr16 = initActivations(p16_config);

	uint8_t *constantWeightVarsAddr17 =
		initConstantWeights("p17.weights.bin", p17_config);
	uint8_t *activationsAddr17 = initActivations(p17_config);

	//======== initialize mutableWeightVarsAddr =======//

	uint8_t *mutableWeightVarsAddr1 =  allocateMutableWeightVars(p1_config);
	uint8_t *mutableWeightVarsAddr2 =  allocateMutableWeightVars(p2_config);
	uint8_t *mutableWeightVarsAddr3 =  allocateMutableWeightVars(p3_config);
	uint8_t *mutableWeightVarsAddr4 =  allocateMutableWeightVars(p4_config);
	uint8_t *mutableWeightVarsAddr5 =  allocateMutableWeightVars(p5_config);
	uint8_t *mutableWeightVarsAddr6 =  allocateMutableWeightVars(p6_config);
	uint8_t *mutableWeightVarsAddr7 =  allocateMutableWeightVars(p7_config);
	uint8_t *mutableWeightVarsAddr8 =  allocateMutableWeightVars(p8_config);
	uint8_t *mutableWeightVarsAddr9 =  allocateMutableWeightVars(p9_config);
	uint8_t *mutableWeightVarsAddr10 =  allocateMutableWeightVars(p10_config);
	uint8_t *mutableWeightVarsAddr11 =  allocateMutableWeightVars(p11_config);
	uint8_t *mutableWeightVarsAddr12 =  allocateMutableWeightVars(p12_config);
	uint8_t *mutableWeightVarsAddr13 =  allocateMutableWeightVars(p13_config);
	uint8_t *mutableWeightVarsAddr14 =  allocateMutableWeightVars(p14_config);
	uint8_t *mutableWeightVarsAddr15 =  allocateMutableWeightVars(p15_config);
	uint8_t *mutableWeightVarsAddr16 =  allocateMutableWeightVars(p16_config);
	uint8_t *mutableWeightVarsAddr17 =  allocateMutableWeightVars(p17_config);

	//#### JO added: p0 is a VTA partition now (NESTOptimalFusedPlan_p0VTA.yaml)
	p0_load_module(constantWeightVarsAddr0);
	p1_load_module(constantWeightVarsAddr1);
	p3_load_module(constantWeightVarsAddr3);
	p6_load_module(constantWeightVarsAddr6);
	p8_load_module(constantWeightVarsAddr8);
	p10_load_module(constantWeightVarsAddr10);
	p12_load_module(constantWeightVarsAddr12);
	p14_load_module(constantWeightVarsAddr14);
	p16_load_module(constantWeightVarsAddr16);

	//======== multiple partitions =======//

	//#### JO changed: clock() is CPU time of all threads (OpenMP) -> wall clock; run all partitions twice
	//#### (warm-up + measured)
	// clock_t start_total = 0;
	// start_total = clock();
	for (int run = 1; run <= 2; run++) {
	timespec start_total, now_total;
	clock_gettime(CLOCK_MONOTONIC, &start_total);
	//#### JO added: per-partition wall time (VTA partitions: p0 p1 p3 p6 p8 p10 p12 p14 p16, the rest CPU/LLVM)
	double part_ms[18] = {0};
	//#### JO changed: time each partition
	// p0(constantWeightVarsAddr0, mutableWeightVarsAddr0, activationsAddr0);
	{ timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
	  p0(constantWeightVarsAddr0, mutableWeightVarsAddr0, activationsAddr0);
	  clock_gettime(CLOCK_MONOTONIC, &b); part_ms[0] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) * 1e-6; }

	float* resultVar0 = getInferenceResultVar(p0_config, mutableWeightVarsAddr0, "resnetv10_pool0_fwd__1");
	copyMutableWeightVarsWithoutAlloc(p1_config, mutableWeightVarsAddr1, "resnetv10_pool0_fwd__1", resultVar0);

	//#### JO changed: time each partition
	// p1(constantWeightVarsAddr1, mutableWeightVarsAddr1, activationsAddr1);
	{ timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
	  p1(constantWeightVarsAddr1, mutableWeightVarsAddr1, activationsAddr1);
	  clock_gettime(CLOCK_MONOTONIC, &b); part_ms[1] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) * 1e-6; }

	float* resultVar1 = getInferenceResultVar(p1_config, mutableWeightVarsAddr1, "resnetv10_stage1_conv1_fwd__2");
	copyMutableWeightVarsWithoutAlloc(p2_config, mutableWeightVarsAddr2, "resnetv10_pool0_fwd__1", resultVar0);
	copyMutableWeightVarsWithoutAlloc(p2_config, mutableWeightVarsAddr2, "resnetv10_stage1_conv1_fwd__2", resultVar1);

	//#### JO changed: time each partition
	// p2(constantWeightVarsAddr2, mutableWeightVarsAddr2, activationsAddr2);
	{ timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
	  p2(constantWeightVarsAddr2, mutableWeightVarsAddr2, activationsAddr2);
	  clock_gettime(CLOCK_MONOTONIC, &b); part_ms[2] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) * 1e-6; }

	float* resultVar2 = getInferenceResultVar(p2_config, mutableWeightVarsAddr2, "resnetv10_stage1_activation0__1");
	copyMutableWeightVarsWithoutAlloc(p3_config, mutableWeightVarsAddr3, "resnetv10_stage1_activation0__1", resultVar2);

	//#### JO changed: time each partition
	// p3(constantWeightVarsAddr3, mutableWeightVarsAddr3, activationsAddr3);
	{ timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
	  p3(constantWeightVarsAddr3, mutableWeightVarsAddr3, activationsAddr3);
	  clock_gettime(CLOCK_MONOTONIC, &b); part_ms[3] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) * 1e-6; }

	float* resultVar3 = getInferenceResultVar(p3_config, mutableWeightVarsAddr3, "resnetv10_stage1_conv3_fwd__2");
	copyMutableWeightVarsWithoutAlloc(p4_config, mutableWeightVarsAddr4, "resnetv10_stage1_activation0__1", resultVar2);
	copyMutableWeightVarsWithoutAlloc(p4_config, mutableWeightVarsAddr4, "resnetv10_stage1_conv3_fwd__2", resultVar3);

	//#### JO changed: time each partition
	// p4(constantWeightVarsAddr4, mutableWeightVarsAddr4, activationsAddr4);
	{ timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
	  p4(constantWeightVarsAddr4, mutableWeightVarsAddr4, activationsAddr4);
	  clock_gettime(CLOCK_MONOTONIC, &b); part_ms[4] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) * 1e-6; }

	float* resultVar4 = getInferenceResultVar(p4_config, mutableWeightVarsAddr4, "resnetv10_stage1_activation1__1");
	copyMutableWeightVarsWithoutAlloc(p5_config, mutableWeightVarsAddr5, "resnetv10_stage1_activation1__1", resultVar4);

	//#### JO changed: time each partition
	// p5(constantWeightVarsAddr5, mutableWeightVarsAddr5, activationsAddr5);
	{ timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
	  p5(constantWeightVarsAddr5, mutableWeightVarsAddr5, activationsAddr5);
	  clock_gettime(CLOCK_MONOTONIC, &b); part_ms[5] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) * 1e-6; }

	float* resultVar5 = getInferenceResultVar(p5_config, mutableWeightVarsAddr5, "resnetv10_stage2_conv2_fwd__2");
	copyMutableWeightVarsWithoutAlloc(p6_config, mutableWeightVarsAddr6, "resnetv10_stage1_activation1__1", resultVar4);

	//#### JO changed: time each partition
	// p6(constantWeightVarsAddr6, mutableWeightVarsAddr6, activationsAddr6);
	{ timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
	  p6(constantWeightVarsAddr6, mutableWeightVarsAddr6, activationsAddr6);
	  clock_gettime(CLOCK_MONOTONIC, &b); part_ms[6] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) * 1e-6; }

	float* resultVar6 = getInferenceResultVar(p6_config, mutableWeightVarsAddr6, "resnetv10_stage2_conv1_fwd__2");
	copyMutableWeightVarsWithoutAlloc(p7_config, mutableWeightVarsAddr7, "resnetv10_stage2_conv2_fwd__2", resultVar5);
	copyMutableWeightVarsWithoutAlloc(p7_config, mutableWeightVarsAddr7, "resnetv10_stage2_conv1_fwd__2", resultVar6);

	//#### JO changed: time each partition
	// p7(constantWeightVarsAddr7, mutableWeightVarsAddr7, activationsAddr7);
	{ timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
	  p7(constantWeightVarsAddr7, mutableWeightVarsAddr7, activationsAddr7);
	  clock_gettime(CLOCK_MONOTONIC, &b); part_ms[7] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) * 1e-6; }

	float* resultVar7 = getInferenceResultVar(p7_config, mutableWeightVarsAddr7, "resnetv10_stage2_activation0__1");
	copyMutableWeightVarsWithoutAlloc(p8_config, mutableWeightVarsAddr8, "resnetv10_stage2_activation0__1", resultVar7);

	//#### JO changed: time each partition
	// p8(constantWeightVarsAddr8, mutableWeightVarsAddr8, activationsAddr8);
	{ timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
	  p8(constantWeightVarsAddr8, mutableWeightVarsAddr8, activationsAddr8);
	  clock_gettime(CLOCK_MONOTONIC, &b); part_ms[8] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) * 1e-6; }

	float* resultVar8 = getInferenceResultVar(p8_config, mutableWeightVarsAddr8, "resnetv10_stage2_conv4_fwd__2");
	copyMutableWeightVarsWithoutAlloc(p9_config, mutableWeightVarsAddr9, "resnetv10_stage2_activation0__1", resultVar7);
	copyMutableWeightVarsWithoutAlloc(p9_config, mutableWeightVarsAddr9, "resnetv10_stage2_conv4_fwd__2", resultVar8);

	//#### JO changed: time each partition
	// p9(constantWeightVarsAddr9, mutableWeightVarsAddr9, activationsAddr9);
	{ timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
	  p9(constantWeightVarsAddr9, mutableWeightVarsAddr9, activationsAddr9);
	  clock_gettime(CLOCK_MONOTONIC, &b); part_ms[9] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) * 1e-6; }

	float* resultVar9 = getInferenceResultVar(p9_config, mutableWeightVarsAddr9, "resnetv10_stage2_activation1__1");
	copyMutableWeightVarsWithoutAlloc(p10_config, mutableWeightVarsAddr10, "resnetv10_stage2_activation1__1", resultVar9);

	//#### JO changed: time each partition
	// p10(constantWeightVarsAddr10, mutableWeightVarsAddr10, activationsAddr10);
	{ timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
	  p10(constantWeightVarsAddr10, mutableWeightVarsAddr10, activationsAddr10);
	  clock_gettime(CLOCK_MONOTONIC, &b); part_ms[10] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) * 1e-6; }

	float* resultVar10_1 = getInferenceResultVar(p10_config, mutableWeightVarsAddr10, "resnetv10_stage3_conv2_fwd__2");
	float* resultVar10_2 = getInferenceResultVar(p10_config, mutableWeightVarsAddr10, "resnetv10_stage3_conv1_fwd__2");
	copyMutableWeightVarsWithoutAlloc(p11_config, mutableWeightVarsAddr11, "resnetv10_stage3_conv2_fwd__2", resultVar10_1);
	copyMutableWeightVarsWithoutAlloc(p11_config, mutableWeightVarsAddr11, "resnetv10_stage3_conv1_fwd__2", resultVar10_2);

	//#### JO changed: time each partition
	// p11(constantWeightVarsAddr11, mutableWeightVarsAddr11, activationsAddr11);
	{ timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
	  p11(constantWeightVarsAddr11, mutableWeightVarsAddr11, activationsAddr11);
	  clock_gettime(CLOCK_MONOTONIC, &b); part_ms[11] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) * 1e-6; }

	float* resultVar11 = getInferenceResultVar(p11_config, mutableWeightVarsAddr11, "resnetv10_stage3_activation0__1");
	copyMutableWeightVarsWithoutAlloc(p12_config, mutableWeightVarsAddr12, "resnetv10_stage3_activation0__1", resultVar11);

	//#### JO changed: time each partition
	// p12(constantWeightVarsAddr12, mutableWeightVarsAddr12, activationsAddr12);
	{ timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
	  p12(constantWeightVarsAddr12, mutableWeightVarsAddr12, activationsAddr12);
	  clock_gettime(CLOCK_MONOTONIC, &b); part_ms[12] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) * 1e-6; }

	float* resultVar12 = getInferenceResultVar(p12_config, mutableWeightVarsAddr12, "resnetv10_stage3_conv4_fwd__2");
	copyMutableWeightVarsWithoutAlloc(p13_config, mutableWeightVarsAddr13, "resnetv10_stage3_activation0__1", resultVar11);
	copyMutableWeightVarsWithoutAlloc(p13_config, mutableWeightVarsAddr13, "resnetv10_stage3_conv4_fwd__2", resultVar12);

	//#### JO changed: time each partition
	// p13(constantWeightVarsAddr13, mutableWeightVarsAddr13, activationsAddr13);
	{ timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
	  p13(constantWeightVarsAddr13, mutableWeightVarsAddr13, activationsAddr13);
	  clock_gettime(CLOCK_MONOTONIC, &b); part_ms[13] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) * 1e-6; }

	float* resultVar13 = getInferenceResultVar(p13_config, mutableWeightVarsAddr13, "resnetv10_stage3_activation1__1");
	copyMutableWeightVarsWithoutAlloc(p14_config, mutableWeightVarsAddr14, "resnetv10_stage3_activation1__1", resultVar13);

	//#### JO changed: time each partition
	// p14(constantWeightVarsAddr14, mutableWeightVarsAddr14, activationsAddr14);
	{ timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
	  p14(constantWeightVarsAddr14, mutableWeightVarsAddr14, activationsAddr14);
	  clock_gettime(CLOCK_MONOTONIC, &b); part_ms[14] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) * 1e-6; }

	float* resultVar14_1 = getInferenceResultVar(p14_config, mutableWeightVarsAddr14, "resnetv10_stage4_conv2_fwd__2");
	float* resultVar14_2 = getInferenceResultVar(p14_config, mutableWeightVarsAddr14, "resnetv10_stage4_conv1_fwd__2");
	copyMutableWeightVarsWithoutAlloc(p15_config, mutableWeightVarsAddr15, "resnetv10_stage4_conv2_fwd__2", resultVar14_1);
	copyMutableWeightVarsWithoutAlloc(p15_config, mutableWeightVarsAddr15, "resnetv10_stage4_conv1_fwd__2", resultVar14_2);

	//#### JO changed: time each partition
	// p15(constantWeightVarsAddr15, mutableWeightVarsAddr15, activationsAddr15);
	{ timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
	  p15(constantWeightVarsAddr15, mutableWeightVarsAddr15, activationsAddr15);
	  clock_gettime(CLOCK_MONOTONIC, &b); part_ms[15] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) * 1e-6; }

	float* resultVar15 = getInferenceResultVar(p15_config, mutableWeightVarsAddr15, "resnetv10_stage4_activation0__1");
	copyMutableWeightVarsWithoutAlloc(p16_config, mutableWeightVarsAddr16, "resnetv10_stage4_activation0__1", resultVar15);

	//#### JO changed: time each partition
	// p16(constantWeightVarsAddr16, mutableWeightVarsAddr16, activationsAddr16);
	{ timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
	  p16(constantWeightVarsAddr16, mutableWeightVarsAddr16, activationsAddr16);
	  clock_gettime(CLOCK_MONOTONIC, &b); part_ms[16] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) * 1e-6; }

	float* resultVar16 = getInferenceResultVar(p16_config, mutableWeightVarsAddr16, "resnetv10_stage4_conv4_fwd__2");
	copyMutableWeightVarsWithoutAlloc(p17_config, mutableWeightVarsAddr17, "resnetv10_stage4_activation0__1", resultVar15);
	copyMutableWeightVarsWithoutAlloc(p17_config, mutableWeightVarsAddr17, "resnetv10_stage4_conv4_fwd__2", resultVar16);

	//#### JO changed: time each partition
	// p17(constantWeightVarsAddr17, mutableWeightVarsAddr17, activationsAddr17);
	{ timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
	  p17(constantWeightVarsAddr17, mutableWeightVarsAddr17, activationsAddr17);
	  clock_gettime(CLOCK_MONOTONIC, &b); part_ms[17] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) * 1e-6; }

	//#### JO changed: wall clock, end of the run loop
	// clock_t now_total;
	// now_total = clock();
	// unsigned long inftime_total = (unsigned long)(now_total - start_total);
	// printf("\n====> [Total inference time] %lu microsec.\n", inftime_total);
	clock_gettime(CLOCK_MONOTONIC, &now_total);
	unsigned long inftime_total = (unsigned long)((now_total.tv_sec - start_total.tv_sec) * 1000000L +
	                                              (now_total.tv_nsec - start_total.tv_nsec) / 1000);
	printf("\n====> [Total inference time] %lu microsec.\n", inftime_total);
	printf("run %d (%s): inference %.3f ms\n", run, run == 1 ? "warm-up" : "measured", inftime_total / 1000.0);
	//#### JO added: per-partition times; the rest of the total is the copies between partitions
	//#### JO changed: p0 is VTA now
	// { const int isVTA[18] = {0,1,0,1,0,0,1,0,1,0,1,0,1,0,1,0,1,0};
	{ const int isVTA[18] = {1,1,0,1,0,0,1,0,1,0,1,0,1,0,1,0,1,0};
	  double cpu = 0, vta = 0;
	  for (int i = 0; i < 18; i++) { printf("  p%-2d %s %9.3f ms\n", i, isVTA[i] ? "VTA" : "CPU", part_ms[i]); (isVTA[i] ? vta : cpu) += part_ms[i]; }
	  printf("  CPU partitions %.3f ms, VTA partitions %.3f ms, copies between partitions %.3f ms\n",
	         cpu, vta, inftime_total / 1000.0 - cpu - vta); }
	}

	// Report the results.
	printf("====== final result of this neural net ========\n");
	dumpInferenceResults(p17_config, mutableWeightVarsAddr17, "resnetv10_dense0_fwd");

	//#### JO added: p0 is a VTA partition now
	p0_destroy_module();
	p1_destroy_module();
	p3_destroy_module();
	p6_destroy_module();
	p8_destroy_module();
	p10_destroy_module();
	p12_destroy_module();
	p14_destroy_module();
	p16_destroy_module();

	// Free all resources.
	free(activationsAddr0);
	free(constantWeightVarsAddr0);
	free(mutableWeightVarsAddr0);

	free(activationsAddr1);
	free(constantWeightVarsAddr1);
	free(mutableWeightVarsAddr1);

	free(activationsAddr2);
	free(constantWeightVarsAddr2);
	free(mutableWeightVarsAddr2);

	free(activationsAddr3);
	free(constantWeightVarsAddr3);
	free(mutableWeightVarsAddr3);

	free(activationsAddr4);
	free(constantWeightVarsAddr4);
	free(mutableWeightVarsAddr4);

	free(activationsAddr5);
	free(constantWeightVarsAddr5);
	free(mutableWeightVarsAddr5);

	free(activationsAddr6);
	free(constantWeightVarsAddr6);
	free(mutableWeightVarsAddr6);

	free(activationsAddr7);
	free(constantWeightVarsAddr7);
	free(mutableWeightVarsAddr7);

	free(activationsAddr8);
	free(constantWeightVarsAddr8);
	free(mutableWeightVarsAddr8);

	free(activationsAddr9);
	free(constantWeightVarsAddr9);
	free(mutableWeightVarsAddr9);

	free(activationsAddr10);
	free(constantWeightVarsAddr10);
	free(mutableWeightVarsAddr10);

	free(activationsAddr11);
	free(constantWeightVarsAddr11);
	free(mutableWeightVarsAddr11);

	free(activationsAddr12);
	free(constantWeightVarsAddr12);
	free(mutableWeightVarsAddr12);

	free(activationsAddr13);
	free(constantWeightVarsAddr13);
	free(mutableWeightVarsAddr13);

	free(activationsAddr14);
	free(constantWeightVarsAddr14);
	free(mutableWeightVarsAddr14);

	free(activationsAddr15);
	free(constantWeightVarsAddr15);
	free(mutableWeightVarsAddr15);

	free(activationsAddr16);
	free(constantWeightVarsAddr16);
	free(mutableWeightVarsAddr16);

	free(activationsAddr17);
	free(constantWeightVarsAddr17);
	free(mutableWeightVarsAddr17);

#ifdef VTAMAIN
	destroyVTARuntime();
#endif
}

