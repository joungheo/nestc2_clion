# docs/ref — systolic 전환 조사에 쓴 외부 자료

분석은 `docs/evta_systolic.md` §9. 받은 날짜 2026-10-27.

이 저장소는 public이다. **재배포 권한이 없는 파일(arXiv 기본 라이선스 논문, AMD 문서)은 이 dir에 받아 두되 git에서 제외한다**(`.gitignore`).
다른 PC에서는 아래 URL로 다시 받는다(`fetch.sh`).

| 파일 | 내용 | 출처 | 라이선스 | git |
|---|---|---|---|---|
| `gemmini/*.scala`, `README.md`, `LICENSE` | Gemmini PE, Tile, Mesh, MeshWithDelays, AccumulatorMem, Configs, ExecuteController | https://github.com/ucb-bar/gemmini (master) | BSD-3 | 포함 |
| `tensil/*.tarch`, `README.md`, `LICENSE` | Tensil 아키텍처 파일(ultra96v2, zcu104, pynqz1) | https://github.com/tensil-ai/tensil (main) | Apache-2.0 | 포함 |
| `autosa/README.md`, `LICENSE` | AutoSA README | https://github.com/UCLA-VAST/AutoSA | MIT | 포함 |
| `gemm_hls/README.md`, `LICENSE.md` | gemm_hls README | https://github.com/spcl/gemm_hls | BSD-3 | 포함 |
| `arxiv_2510.08137.pdf` | Petropoulos & Antonakopoulos, A Scalable FPGA Architecture With Adaptive Memory Utilization for GEMM-Based Operations | https://arxiv.org/abs/2510.08137 | CC-BY 4.0 | 포함 |
| `arxiv_2409.03508.pdf` | Li et al., Revealing Untapped DSP Optimization Potentials for FPGA-Based Systolic Matrix Engines | https://arxiv.org/abs/2409.03508 | arXiv non-exclusive | 제외 |
| `arxiv_2309.16158.pdf` | FireFly v2 | https://arxiv.org/abs/2309.16158 | arXiv non-exclusive | 제외 |
| `arxiv_1911.09925.pdf` | Gemmini (DAC'21) | https://arxiv.org/abs/1911.09925 | arXiv non-exclusive | 제외 |
| `arxiv_1807.04188.pdf` | VTA: A Hardware-Software Blueprint for Flexible Deep Learning Specialization | https://arxiv.org/abs/1807.04188 | arXiv non-exclusive | 제외 |
| `arxiv_1912.06526.pdf` | de Fine Licht et al., Flexible Communication Avoiding Matrix Multiplication on FPGA with HLS | https://arxiv.org/abs/1912.06526 | arXiv non-exclusive | 제외 |
| `ug579_v1.11.pdf` | UG579 UltraScale Architecture DSP Slice v1.11 | AMD (docs.amd.com) | AMD 저작권 | 제외 |
| `web_notes.md` | WP486, PG338 Clocking, 받지 못한 문서의 상태 | 각 URL | 발췌·요약 | 포함 |

텍스트로 검색할 때: `pdftotext -layout <x>.pdf -`.
