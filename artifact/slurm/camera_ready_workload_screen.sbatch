#!/usr/bin/env bash
#SBATCH --job-name=oc_camera_screen
#SBATCH --account=a-g200
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=64
#SBATCH --mem=180G
#SBATCH --time=01:30:00
#SBATCH --partition=debug
#SBATCH --output=slurm-%x-%j.out
#SBATCH --error=slurm-%x-%j.err

set -euo pipefail
cd "${SLURM_SUBMIT_DIR}"

PYTHON="${ORDEREDCHAOS_PYTHON:-/iopsstor/scratch/cscs/btommaso/SC26_reviewer_scratch_final_99457a1/orderedchaos_ae/venv/bin/python}"
ROOT="${SCRATCH}/orderedchaos-host-only-camera-screen-${SLURM_JOB_ID}"

cmake --build build-host-only --parallel 32
BINARY="${SLURM_SUBMIT_DIR}/build-host-only/htsim_uec"

run_candidate() {
    local result="$1"
    "${PYTHON}" artifact/scripts/run_simulation_matrix.py \
        --binary "${BINARY}" \
        --result "${result}" --variant camera_workload_screen \
        --output-root "${ROOT}/${result}" --workers 8
}

run_candidate figure7a &
pid7a=$!
run_candidate figure7b &
pid7b=$!
run_candidate figure10 &
pid10=$!
run_candidate camera_ready_incast &
pid_camera=$!

wait "${pid7a}"
wait "${pid7b}"
wait "${pid10}"
wait "${pid_camera}"

printf 'PASS\n' > "${ROOT}/PASS"
printf '%s\n' "${ROOT}"
