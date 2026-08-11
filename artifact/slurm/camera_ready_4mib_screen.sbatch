#!/usr/bin/env bash
#SBATCH --job-name=oc_camera4m_screen
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
BINARY="${SLURM_SUBMIT_DIR}/build-host-only/htsim_uec"
ROOT="${SCRATCH}/orderedchaos-host-only-camera4m-screen-${SLURM_JOB_ID}"
RAW="${ROOT}/raw"
INPUTS="${ROOT}/inputs"
MEASUREMENTS="${ROOT}/measurements"
mkdir -p "${INPUTS}" "${MEASUREMENTS}"

cmake --build build-host-only --parallel 32

pids=()
for active in 8 16 32; do
    matrix="${INPUTS}/alltoall_128_4000000_active${active}.cm"
    "${PYTHON}" artifact/scripts/generate_alltoall_matrix.py \
        --output "${matrix}" --nodes 128 --flow-bytes 4000000 \
        --concurrency-per-source 1 --active-sources "${active}" \
        --destination-schedule round-robin --seed 20260810
    "${PYTHON}" artifact/scripts/run_simulation_matrix.py \
        --binary "${BINARY}" --result camera_ready_incast \
        --variant camera_workload_screen --traffic-matrix "${matrix}" \
        --output-root "${RAW}/active${active}" --workers 32 \
        > "${MEASUREMENTS}/active${active}.out" \
        2> "${MEASUREMENTS}/active${active}.err" &
    pids+=("$!")
done

for pid in "${pids[@]}"; do
    wait "${pid}"
done
printf 'PASS\n' > "${ROOT}/PASS"
printf '%s\n' "${ROOT}"
