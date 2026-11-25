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
 */

/*!
 * \file vta_test.cpp
 * \brief Simulation tests for the VTA design.
 */

#include <stdio.h>
#include <stdlib.h>
#include <iostream>

#include "../src/vta.h"
#include "../../../tests/hardware/common/test_lib.h"

#if defined(VTA_ALUX) || defined(VTA_ALU_MUL)
//#### JO added (2026-11-11, ALU extension): sim/alux_test.cc
int alux_test(unsigned seed, int ninsn);
#endif

#ifdef VTA_DWU
//#### JO added (2026-11-24, docs/evta_dw_unit.md): sim/dwu_test.cc, the depthwise window mode
int dwu_test(unsigned seed, int ho, int wo, int s);
#endif

int main(void) {
#if DEBUG == 1
    printParameters();
#endif

    int status = 0;

    // Run ALU test (vector-scalar operators)
    status |= alu_test(VTA_ALU_OPCODE_MIN, true, VTA_BLOCK_OUT, 128, true);
    status |= alu_test(VTA_ALU_OPCODE_MIN, true, VTA_BLOCK_OUT, 128, false);
    status |= alu_test(VTA_ALU_OPCODE_MAX, true, VTA_BLOCK_OUT, 128, true);
    status |= alu_test(VTA_ALU_OPCODE_MAX, true, VTA_BLOCK_OUT, 128, false);
    status |= alu_test(VTA_ALU_OPCODE_ADD, true, VTA_BLOCK_OUT, 128, true);
    status |= alu_test(VTA_ALU_OPCODE_ADD, true, VTA_BLOCK_OUT, 128, false);
    status |= alu_test(VTA_ALU_OPCODE_SHR, true, VTA_BLOCK_OUT, 128, true);
    status |= alu_test(VTA_ALU_OPCODE_SHR, true, VTA_BLOCK_OUT, 128, false);

    // Run ALU test (vector-vector operators)
    status |= alu_test(VTA_ALU_OPCODE_MIN, false, VTA_BLOCK_OUT, 128, true);
    status |= alu_test(VTA_ALU_OPCODE_MIN, false, VTA_BLOCK_OUT, 128, false);
    status |= alu_test(VTA_ALU_OPCODE_MAX, false, VTA_BLOCK_OUT, 128, true);
    status |= alu_test(VTA_ALU_OPCODE_MAX, false, VTA_BLOCK_OUT, 128, false);
    status |= alu_test(VTA_ALU_OPCODE_ADD, false, VTA_BLOCK_OUT, 128, true);
    status |= alu_test(VTA_ALU_OPCODE_ADD, false, VTA_BLOCK_OUT, 128, false);
    status |= alu_test(VTA_ALU_OPCODE_SHR, false, VTA_BLOCK_OUT, 128, true);
    status |= alu_test(VTA_ALU_OPCODE_SHR, false, VTA_BLOCK_OUT, 128, false);

#if defined(VTA_ALUX) || defined(VTA_ALU_MUL)   //#### JO (2026-11-23): also the MUL-only build
    //#### JO added (2026-11-11): random extended ALU programs against the reference model (VTA_ALUX_SEEDS, default 20)
    {
      const char *e = getenv("VTA_ALUX_SEEDS");
      int seeds = e ? atoi(e) : 20;
      for (int s = 1; s <= seeds; s++) status |= alux_test(s, 48);
    }
#endif

#ifdef VTA_DWU
    {
      static const int cs[][3] = {{1, 1, 1}, {2, 3, 1}, {4, 56, 1}, {14, 14, 1}, {7, 7, 2}, {3, 28, 2}, {1, 5, 2}, {5, 9, 1}};
      unsigned seed = 1;
      for (auto &c : cs) status |= dwu_test(seed++, c[0], c[1], c[2]);
    }
#endif

    // Run blocked GEMM test
    status |= blocked_gemm_test(256, 256, VTA_BLOCK_OUT*4, false, 2);
    status |= blocked_gemm_test(256, 256, VTA_BLOCK_OUT*4, false, 1);

    // Simple GEMM unit test
    status |= gemm_test(4 * VTA_BATCH, 4 * VTA_BLOCK_OUT, 4 * VTA_BLOCK_IN, false);

    return status;
}
