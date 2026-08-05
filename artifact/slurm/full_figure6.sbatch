#!/usr/bin/env bash
#SBATCH --job-name=oc_ae_fig6_full
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=128
#SBATCH --mem=64G
#SBATCH --time=01:30:00
#SBATCH --output=slurm-%x-%j.out
#SBATCH --error=slurm-%x-%j.err

set -euo pipefail

cd "${SLURM_SUBMIT_DIR}"
ROOT="${SCRATCH}/orderedchaos_ae/raw/figure6-full-${SLURM_JOB_ID}"
MEASURE="${SCRATCH}/orderedchaos_ae/measurements/figure6-full-${SLURM_JOB_ID}"
WORKERS="${ORDEREDCHAOS_WORKERS:-64}"
BINARY="${ORDEREDCHAOS_BINARY:?Set ORDEREDCHAOS_BINARY}"
mkdir -p "${MEASURE}"

/usr/bin/time -v -o "${MEASURE}/paper_configuration.time" \
    python3 scripts/regenerate_pfld_probe_headroom_multiseed.py \
    --binary "${BINARY}" \
    --num-seeds 50 --jobs "${WORKERS}" --drop-rate 0.00005 \
    --sim-out-root "${ROOT}/paper_configuration/raw" \
    --figure-dir "${ROOT}/paper_configuration/plots" \
    --prefix "figure6_paper_configuration" \
    > "${MEASURE}/paper_configuration.out" 2> "${MEASURE}/paper_configuration.err"
