#!/usr/bin/env bash
#SBATCH --job-name=oc_host_camera_final
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
BINARY="${ORDEREDCHAOS_BINARY:-${SLURM_SUBMIT_DIR}/build-host-only/htsim_uec}"
ROOT="${SCRATCH}/orderedchaos-host-only-camera-final-${SLURM_JOB_ID}"
RAW="${ROOT}/raw"
ANALYSIS="${ROOT}/analysis"
MEASUREMENTS="${ROOT}/measurements"
mkdir -p "${ANALYSIS}" "${MEASUREMENTS}"

cmake --build build-host-only --parallel 32
ctest --test-dir build-host-only --output-on-failure

/usr/bin/time -v -o "${MEASUREMENTS}/camera_ready_image2.time" \
    "${PYTHON}" artifact/scripts/run_simulation_matrix.py \
    --binary "${BINARY}" \
    --result camera_ready_incast --variant paper_configuration \
    --output-root "${RAW}/camera_ready_image2" --workers 96 \
    > "${MEASUREMENTS}/camera_ready_image2.out" \
    2> "${MEASUREMENTS}/camera_ready_image2.err" &
camera_pid=$!

/usr/bin/time -v -o "${MEASUREMENTS}/probe_ablation.time" \
    "${PYTHON}" artifact/scripts/run_simulation_matrix.py \
    --binary "${BINARY}" \
    --result camera_ready_probe_ablation --variant paper_configuration \
    --output-root "${RAW}/probe_ablation" --workers 96 \
    > "${MEASUREMENTS}/probe_ablation.out" \
    2> "${MEASUREMENTS}/probe_ablation.err" &
ablation_pid=$!

wait "${camera_pid}"
wait "${ablation_pid}"

"${PYTHON}" artifact/scripts/plot_camera_ready_incast.py \
    --run-root "${RAW}/camera_ready_image2/camera_ready_incast/paper_configuration" \
    --output "${ANALYSIS}/camera_ready_image2" --drop-rate 0.005 \
    --workload "128-node all-to-all sweep (32 active senders), 4 MiB" \
    --expected-flows 16256

"${PYTHON}" artifact/scripts/plot_probe_compounding.py \
    --run-root "${RAW}/probe_ablation/camera_ready_probe_ablation/paper_configuration" \
    --output "${ANALYSIS}/probe_ablation" --drop-rate 0.005 \
    --workload "128-node 8-MiB tornado" --expected-flows 128

printf 'PASS\n' > "${ROOT}/PASS"
printf '%s\n' "${ROOT}"
