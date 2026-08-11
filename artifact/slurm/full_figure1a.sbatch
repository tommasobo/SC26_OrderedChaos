#!/usr/bin/env bash
#SBATCH --job-name=oc_ae_fig1a_full
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --time=01:30:00
#SBATCH --output=slurm-%x-%j.out
#SBATCH --error=slurm-%x-%j.err

set -euo pipefail

cd "${SLURM_SUBMIT_DIR}"
ROOT="${SCRATCH}/orderedchaos_ae/raw/figure1a-full-${SLURM_JOB_ID}"
MEASURE="${SCRATCH}/orderedchaos_ae/measurements/figure1a-full-${SLURM_JOB_ID}.time"
WORKERS="${ORDEREDCHAOS_WORKERS:-2}"
BINARY="${ORDEREDCHAOS_BINARY:?Set ORDEREDCHAOS_BINARY}"
/usr/bin/time -v -o "${MEASURE}" \
    python3 artifact/scripts/run_figure1a.py \
    --binary "${BINARY}" --output-root "${ROOT}" --workers "${WORKERS}"
