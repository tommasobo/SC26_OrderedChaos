#!/usr/bin/env bash
#SBATCH --job-name=oc_figure1b
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=24
#SBATCH --mem=128G
#SBATCH --time=01:30:00
#SBATCH --output=slurm-%x-%j.out
#SBATCH --error=slurm-%x-%j.err

set -euo pipefail
cd "${SLURM_SUBMIT_DIR}"
PYTHON="${ORDEREDCHAOS_PYTHON:-${SCRATCH}/orderedchaos_ae/venv/bin/python}"
if [[ ! -x "${PYTHON}" ]]; then
    echo "Missing artifact Python: ${PYTHON}" >&2
    exit 2
fi

ROOT="${SCRATCH}/orderedchaos_ae/raw/figure1b-full-${SLURM_JOB_ID}"
MEASURE="${SCRATCH}/orderedchaos_ae/measurements/figure1b-full-${SLURM_JOB_ID}"
WORKERS="${ORDEREDCHAOS_WORKERS:-24}"
mkdir -p "${ROOT}" "${MEASURE}"

/usr/bin/time -v -o "${MEASURE}/figure1b.time" \
    "${PYTHON}" artifact/scripts/run_figure1b.py \
    --output-root "${ROOT}/figure1b-seeds-1-20" \
    --positive-drop-rate 0.00005 --seeds 1-20 --workers "${WORKERS}" \
    --timeout-seconds 60 \
    > "${MEASURE}/figure1b.out" 2> "${MEASURE}/figure1b.err"

"${PYTHON}" artifact/scripts/run_figure1b.py \
    --output-root "${ROOT}/figure1b-pooled" \
    --raw-source-root "${ROOT}/figure1b-seeds-1-20" \
    --positive-drop-rate 0.00005 --seeds 1-20
