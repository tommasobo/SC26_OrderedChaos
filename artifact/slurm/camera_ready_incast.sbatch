#!/usr/bin/env bash
#SBATCH --job-name=oc_camera_incast
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=32
#SBATCH --mem=128G
#SBATCH --time=06:00:00
#SBATCH --output=slurm-%x-%j.out
#SBATCH --error=slurm-%x-%j.err

set -euo pipefail
cd "${SLURM_SUBMIT_DIR}"

PYTHON="${ORDEREDCHAOS_PYTHON:-${SCRATCH}/orderedchaos_ae/venv/bin/python}"
if [[ ! -x "${PYTHON}" ]]; then
    echo "Missing artifact Python: ${PYTHON}" >&2
    exit 2
fi

ROOT="${SCRATCH}/orderedchaos_ae/raw/camera-ready-incast-${SLURM_JOB_ID}"
ANALYSIS="${SCRATCH}/orderedchaos_ae/analysis/camera-ready-incast-${SLURM_JOB_ID}"
MEASURE="${SCRATCH}/orderedchaos_ae/measurements/camera-ready-incast-${SLURM_JOB_ID}"
WORKERS="${ORDEREDCHAOS_WORKERS:-${SLURM_CPUS_PER_TASK:-16}}"
BINARY="${ORDEREDCHAOS_BINARY:?Set ORDEREDCHAOS_BINARY}"
mkdir -p "${MEASURE}"

/usr/bin/time -v -o "${MEASURE}/simulation.time" \
    "${PYTHON}" artifact/scripts/run_simulation_matrix.py \
    --binary "${BINARY}" \
    --result camera_ready_incast --variant paper_configuration \
    --output-root "${ROOT}" --workers "${WORKERS}" \
    > "${MEASURE}/simulation.out" 2> "${MEASURE}/simulation.err"

/usr/bin/time -v -o "${MEASURE}/plot.time" \
    "${PYTHON}" artifact/scripts/plot_camera_ready_incast.py \
    --run-root "${ROOT}/camera_ready_incast/paper_configuration" \
    --output "${ANALYSIS}" --drop-rate 0.005 \
    --workload "128-node all-to-all sweep (32 active senders), 4 MiB" \
    --expected-flows 16256

printf 'PASS\n' > "${ANALYSIS}/PASS"
