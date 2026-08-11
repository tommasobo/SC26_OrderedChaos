#!/usr/bin/env bash
#SBATCH --job-name=oc_camera128_screen
#SBATCH --account=a-g200
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=256
#SBATCH --mem=230G
#SBATCH --time=01:30:00
#SBATCH --partition=debug
#SBATCH --output=slurm-%x-%j.out
#SBATCH --error=slurm-%x-%j.err

set -euo pipefail
cd "${SLURM_SUBMIT_DIR}"

PYTHON="${ORDEREDCHAOS_PYTHON:-/iopsstor/scratch/cscs/btommaso/SC26_reviewer_scratch_final_99457a1/orderedchaos_ae/venv/bin/python}"
ROOT="${SCRATCH}/orderedchaos-host-only-camera128-screen-${SLURM_JOB_ID}"
RAW="${ROOT}/raw"
ANALYSIS="${ROOT}/analysis"
MEASUREMENTS="${ROOT}/measurements"
mkdir -p "${ANALYSIS}" "${MEASUREMENTS}"

cmake --build build-host-only --parallel 32
BINARY="${SLURM_SUBMIT_DIR}/build-host-only/htsim_uec"

"${PYTHON}" artifact/scripts/run_simulation_matrix.py \
    --binary "${BINARY}" \
    --result camera_ready_incast --variant camera_workload_screen \
    --output-root "${RAW}/camera_screen" --workers 48 \
    > "${MEASUREMENTS}/camera_screen.out" \
    2> "${MEASUREMENTS}/camera_screen.err" &
camera_pid=$!

"${PYTHON}" artifact/scripts/run_simulation_matrix.py \
    --binary "${BINARY}" \
    --result camera_ready_incast --variant promoted_probe_ablation \
    --output-root "${RAW}/promotion" --workers 48 \
    > "${MEASUREMENTS}/promotion.out" \
    2> "${MEASUREMENTS}/promotion.err" &
promotion_pid=$!

"${PYTHON}" artifact/scripts/run_simulation_matrix.py \
    --binary "${BINARY}" \
    --result camera_ready_probe_ablation --variant paper_configuration \
    --output-root "${RAW}/probe_compounding" --workers 96 \
    > "${MEASUREMENTS}/probe_compounding.out" \
    2> "${MEASUREMENTS}/probe_compounding.err" &
compounding_pid=$!

wait "${camera_pid}"
wait "${promotion_pid}"
wait "${compounding_pid}"

"${PYTHON}" artifact/scripts/plot_probe_compounding.py \
    --run-root "${RAW}/probe_compounding/camera_ready_probe_ablation/paper_configuration" \
    --output "${ANALYSIS}/probe_compounding" --drop-rate 0.005 \
    --workload "128-node 8-MiB tornado" --expected-flows 128

printf 'PASS\n' > "${ROOT}/PASS"
printf '%s\n' "${ROOT}"
