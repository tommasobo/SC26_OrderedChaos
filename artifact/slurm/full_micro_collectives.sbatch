#!/usr/bin/env bash
#SBATCH --job-name=oc_ae_micro_full
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=128
#SBATCH --mem=128G
#SBATCH --time=02:00:00
#SBATCH --output=slurm-%x-%j.out
#SBATCH --error=slurm-%x-%j.err

set -euo pipefail

cd "${SLURM_SUBMIT_DIR}"
PYTHON="${ORDEREDCHAOS_PYTHON:-${SCRATCH}/orderedchaos_ae/venv/bin/python}"
if [[ ! -x "${PYTHON}" ]]; then
    echo "Missing artifact Python: ${PYTHON}" >&2
    exit 2
fi
RING_INPUT="${SCRATCH}/orderedchaos_ae/inputs/generated/ring_all_reduce_128_500000.cm"
if [[ ! -s "${RING_INPUT}" ]]; then
    mkdir -p "$(dirname "${RING_INPUT}")"
    "${PYTHON}" artifact/scripts/generate_ring_matrix.py \
        --output "${RING_INPUT}" --flow-bytes 500000
fi
ROOT="${SCRATCH}/orderedchaos_ae/raw/micro-full-${SLURM_JOB_ID}"
MEASURE="${SCRATCH}/orderedchaos_ae/measurements/micro-full-${SLURM_JOB_ID}"
mkdir -p "${MEASURE}"

results=(figure7a figure7b)
variants=(paper_configuration paper_configuration)
WORKERS="${ORDEREDCHAOS_WORKERS:-48}"
BINARY="${ORDEREDCHAOS_BINARY:?Set ORDEREDCHAOS_BINARY}"
local_mode="${ORDEREDCHAOS_LOCAL:-0}"
pids=()
run_result() {
    local result=$1
    local variant=$2
    /usr/bin/time -v -o "${MEASURE}/${result}-${variant}.time" \
        "${PYTHON}" artifact/scripts/run_simulation_matrix.py \
        --binary "${BINARY}" \
        --result "${result}" --variant "${variant}" \
        --output-root "${ROOT}" --workers "${WORKERS}" \
        > "${MEASURE}/${result}-${variant}.out" \
        2> "${MEASURE}/${result}-${variant}.err"
}

for index in "${!results[@]}"; do
    result="${results[index]}"
    variant="${variants[index]}"
    if [[ "${local_mode}" == "1" ]]; then
        run_result "${result}" "${variant}"
    else
        run_result "${result}" "${variant}" &
        pids+=("$!")
    fi
done

status=0
if [[ "${local_mode}" != "1" ]]; then
    for pid in "${pids[@]}"; do
        wait "${pid}" || status=1
    done
fi
exit "${status}"
