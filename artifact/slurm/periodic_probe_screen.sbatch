#!/usr/bin/env bash
#SBATCH --job-name=oc_periodic_screen
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
ROOT="${SCRATCH}/orderedchaos-host-only-periodic-screen-${SLURM_JOB_ID}"
RAW="${ROOT}/raw"
INPUTS="${ROOT}/inputs"
MEASUREMENTS="${ROOT}/measurements"
mkdir -p "${INPUTS}" "${MEASUREMENTS}"

cmake --build build-host-only --parallel 32

"${PYTHON}" artifact/scripts/generate_incast_matrix.py \
    --output "${INPUTS}/incast_128_4000000_32.cm" --nodes 128 \
    --senders 32 --receiver 0 --flow-bytes 4000000
"${PYTHON}" artifact/scripts/generate_incast_matrix.py \
    --output "${INPUTS}/incast_128_4000000_64.cm" --nodes 128 \
    --senders 64 --receiver 0 --flow-bytes 4000000
"${PYTHON}" artifact/scripts/generate_incast_matrix.py \
    --output "${INPUTS}/incast_128_4000000_127.cm" --nodes 128 \
    --senders 127 --receiver 0 --flow-bytes 4000000

algorithms="pfld_tail_only,pfld_probe_rtt_no_rtx,pfld_probe16_no_rtx,pfld_probe8_no_rtx,pfld_probe4_no_rtx,pfld_probe1_no_rtx"
pids=()
for senders in 32 64 127; do
    "${PYTHON}" artifact/scripts/run_simulation_matrix.py \
        --binary "${BINARY}" --result camera_ready_probe_ablation \
        --variant paper_configuration --seeds 1-5 --algorithms "${algorithms}" \
        --traffic-matrix "${INPUTS}/incast_128_4000000_${senders}.cm" \
        --output-root "${RAW}/incast${senders}" --workers 30 \
        > "${MEASUREMENTS}/incast${senders}.out" \
        2> "${MEASUREMENTS}/incast${senders}.err" &
    pids+=("$!")
done

for pid in "${pids[@]}"; do
    wait "${pid}"
done
printf 'PASS\n' > "${ROOT}/PASS"
printf '%s\n' "${ROOT}"
