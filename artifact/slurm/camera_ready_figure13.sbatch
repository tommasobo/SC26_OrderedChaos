#!/usr/bin/env bash
#SBATCH --job-name=oc_cr_fig13
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=64
#SBATCH --mem=128G
#SBATCH --time=01:30:00
#SBATCH --output=slurm-%x-%j.out
#SBATCH --error=slurm-%x-%j.err

set -euo pipefail

cd "${SLURM_SUBMIT_DIR}"
LOCAL_MODE="${ORDEREDCHAOS_LOCAL:-0}"
WORKERS="${ORDEREDCHAOS_WORKERS:-8}"
cmake --build build --parallel "${WORKERS}"
ROOT="${SCRATCH}/orderedchaos_ae/raw/camera-ready-figure13-${SLURM_JOB_ID}"
MEASURE="${SCRATCH}/orderedchaos_ae/measurements/camera-ready-figure13-${SLURM_JOB_ID}"
mkdir -p "${MEASURE}"

pids=()
status=0
wait_batch() {
    local pid
    for pid in "${pids[@]}"; do
        wait "${pid}" || status=1
    done
    pids=()
}

run_case() {
    local variant=$1
    local seed=$2
    local drop=$3
    local slug="${variant}-seed${seed}-drop${drop}"
    local drop_dir="${drop}"
    /usr/bin/time -v -o "${MEASURE}/${slug}.time" \
        python3 artifact/scripts/run_sensitivity_matrix.py \
        --variant "${variant}" --seed "${seed}" --drop-rate "${drop}" \
        --output-root "${ROOT}/${variant}/seed${seed}/drop${drop_dir}" \
        --workers "${WORKERS}" \
        > "${MEASURE}/${slug}.out" 2> "${MEASURE}/${slug}.err"
}

for variant in paper_symmetric paper_asymmetric; do
    for seed in 1 25 42; do
        for drop in 0; do
            if [[ "${LOCAL_MODE}" == "1" ]]; then
                run_case "${variant}" "${seed}" "${drop}" || status=1
            else
                run_case "${variant}" "${seed}" "${drop}" &
                pids+=("$!")
                if (( ${#pids[@]} == 8 )); then
                    wait_batch
                fi
            fi
        done
    done
done
if [[ "${LOCAL_MODE}" != "1" ]]; then
    wait_batch
fi
if (( status == 0 )); then
    ANALYSIS="${SCRATCH}/orderedchaos_ae/analysis/camera-ready-figure13-${SLURM_JOB_ID}"
    /usr/bin/time -v -o "${MEASURE}/analysis.time" \
        python3 artifact/scripts/analyze_figure13_camera_ready.py \
        --raw-root "${ROOT}" --output "${ANALYSIS}"
    python3 artifact/scripts/plot_figure13_final.py \
        --input "${ANALYSIS}/paper_symmetric_drop_0.csv" \
        --output "${ANALYSIS}/final/figure13a" \
        --executed-drop-rate 0 \
        --topology-note "64-node symmetric configuration"
    python3 artifact/scripts/plot_figure13_final.py \
        --input "${ANALYSIS}/paper_asymmetric_drop_0.csv" \
        --output "${ANALYSIS}/final/figure13b" \
        --executed-drop-rate 0 \
        --topology-note "one-direction asymmetric configuration"
fi
exit "${status}"
