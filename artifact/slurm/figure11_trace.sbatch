#!/usr/bin/env bash
#SBATCH --job-name=oc_figure11
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=48
#SBATCH --mem=192G
#SBATCH --time=01:30:00
#SBATCH --output=slurm-%x-%j.out
#SBATCH --error=slurm-%x-%j.err

set -euo pipefail
cd "${SLURM_SUBMIT_DIR}"

SOURCE="${SCRATCH}/orderedchaos_ae/worktrees/trace_source/build/htsim_uec"
ROOT="${SCRATCH}/orderedchaos_ae/raw/figure11-trace-${SLURM_JOB_ID}"
MEASURE="${SCRATCH}/orderedchaos_ae/measurements/figure11-trace-${SLURM_JOB_ID}"
LOCAL_MODE="${ORDEREDCHAOS_LOCAL:-0}"
SOURCE_WORKERS="${ORDEREDCHAOS_WORKERS:-7}"
RACK_WORKERS=2
if [[ "${LOCAL_MODE}" == "1" ]]; then
    RACK_WORKERS=1
fi
mkdir -p "${ROOT}" "${MEASURE}"

if [[ ! -f "${SCRATCH}/orderedchaos_ae/inputs/llama7b_n4/.READY" ]]; then
    printf '%s\n' \
        "Figure 11 skipped because the checksum-verified LLaMA binary trace is unavailable." \
        > "${ROOT}/SKIPPED.txt"
    cp "${ROOT}/SKIPPED.txt" "${MEASURE}/SKIPPED.txt"
    if [[ -s "${SCRATCH}/orderedchaos_ae/inputs/trace_input_status.tsv" ]]; then
        cp "${SCRATCH}/orderedchaos_ae/inputs/trace_input_status.tsv" \
            "${MEASURE}/trace_input_status.tsv"
    fi
    cat "${ROOT}/SKIPPED.txt"
    exit 0
fi

pids=()
run_source() {
    /usr/bin/time -v -o "${MEASURE}/source.time" \
        python3 artifact/scripts/run_simulation_matrix.py \
        --binary "${SOURCE}" --result figure11 --variant paper_configuration \
        --algorithms ecmp,flowbender,flowlet,oblivious,reps,rss,rss1 \
        --output-root "${ROOT}/source" --workers "${SOURCE_WORKERS}" \
        > "${MEASURE}/source.out" 2> "${MEASURE}/source.err"
}

run_rack() {
    /usr/bin/time -v -o "${MEASURE}/rack.time" \
        python3 artifact/scripts/run_simulation_matrix.py \
        --result figure11 --variant paper_configuration \
        --algorithms rss_rack_tlp --output-root "${ROOT}/rack" \
        --workers "${RACK_WORKERS}" \
        > "${MEASURE}/rack.out" 2> "${MEASURE}/rack.err"
}

status=0
if [[ "${LOCAL_MODE}" == "1" ]]; then
    run_source || status=1
    run_rack || status=1
else
    run_source &
    pids+=("$!")
    run_rack &
    pids+=("$!")
    for pid in "${pids[@]}"; do
        wait "${pid}" || status=1
    done
fi
exit "${status}"
