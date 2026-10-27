#!/bin/bash
# #### JO added (2026-10-27): download the git-ignored references of this dir again (see README.md).
cd "$(dirname "$0")"
for u in 2409.03508 2309.16158 1911.09925 1807.04188 1912.06526 2510.08137; do [ -f arxiv_$u.pdf ] || curl -sfL -o arxiv_$u.pdf https://arxiv.org/pdf/$u; done
echo "UG579 v1.11: download from docs.amd.com (search 'UG579') into ug579_v1.11.pdf"
ls -la *.pdf
