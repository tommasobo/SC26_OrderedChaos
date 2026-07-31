#!/usr/bin/env bash
#SBATCH --job-name=oc_reviewer_quick
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=64
#SBATCH --mem=128G
#SBATCH --time=01:00:00
#SBATCH --output=slurm-%x-%j.out
#SBATCH --error=slurm-%x-%j.err

set -euo pipefail
cd "${SLURM_SUBMIT_DIR}"
PYTHON="${ORDEREDCHAOS_PYTHON:-${SCRATCH}/orderedchaos_ae/venv/bin/python}"
if [[ ! -x "${PYTHON}" ]]; then
    echo "Missing artifact Python: ${PYTHON}" >&2
    exit 2
fi

ROOT="${SCRATCH}/orderedchaos_ae/reviewer-quick-${SLURM_JOB_ID}"
RAW="${ROOT}/raw"
PLOTS="${ROOT}/plots"
MEASURE="${ROOT}/measurements"
mkdir -p "${RAW}" "${PLOTS}" "${MEASURE}"

local_mode="${ORDEREDCHAOS_LOCAL:-0}"
workers="${ORDEREDCHAOS_WORKERS:-12}"
figure1b_seeds="1-3"
figure6_seeds=4
if [[ "${local_mode}" == "1" ]]; then
    figure1b_seeds="1-2"
    figure6_seeds=2
fi

pids=()
launch() {
    if [[ "${local_mode}" == "1" ]]; then
        "$@"
    else
        "$@" &
        pids+=("$!")
    fi
}

run_figure1b() {
    /usr/bin/time -v -o "${MEASURE}/figure1b.time" \
        "${PYTHON}" artifact/scripts/run_figure1b.py \
        --output-root "${RAW}/figure1b" --positive-drop-rate 0.00005 \
        --seeds "${figure1b_seeds}" --workers "${workers}" --timeout-seconds 60 \
        > "${MEASURE}/figure1b.out" 2> "${MEASURE}/figure1b.err"
}
launch run_figure1b

run_figure6() {
    /usr/bin/time -v -o "${MEASURE}/figure6.time" \
        "${PYTHON}" scripts/regenerate_pfld_probe_headroom_multiseed.py \
        --num-seeds "${figure6_seeds}" --jobs "${workers}" --drop-rate 0.00005 \
        --sim-out-root "${RAW}/figure6" --figure-dir "${PLOTS}/figure6" \
        --prefix reviewer_quick \
        > "${MEASURE}/figure6.out" 2> "${MEASURE}/figure6.err"
}
launch run_figure6

for result in figure7a figure7b; do
    run_collective() {
        local result=$1
        /usr/bin/time -v -o "${MEASURE}/${result}.time" \
            "${PYTHON}" artifact/scripts/run_simulation_matrix.py \
            --result "${result}" --variant paper_configuration --seeds 1 \
            --output-root "${RAW}" --workers "${workers}" \
            > "${MEASURE}/${result}.out" 2> "${MEASURE}/${result}.err"
    }
    launch run_collective "${result}"
done

run_figure10() {
    /usr/bin/time -v -o "${MEASURE}/figure10.time" \
        "${PYTHON}" artifact/scripts/run_simulation_matrix.py \
        --result figure10 --variant paper_configuration --seeds 1 \
        --output-root "${RAW}" --workers "${workers}" \
        > "${MEASURE}/figure10.out" 2> "${MEASURE}/figure10.err"
}
launch run_figure10

status=0
if [[ "${local_mode}" != "1" ]]; then
    for pid in "${pids[@]}"; do
        wait "${pid}" || status=1
    done
fi
if [[ "${status}" -ne 0 ]]; then
    exit "${status}"
fi

"${PYTHON}" artifact/scripts/generate_memory_results.py \
    --output "${PLOTS}/figure5" --mtu-bytes 4096

"${PYTHON}" nsdi_plotting.py "${RAW}/figure7a/paper_configuration" \
    --csv fresh_summary.csv --output fresh_plot.png --no-show \
    --separate-y --ideal-fct 172
"${PYTHON}" nsdi_plotting.py "${RAW}/figure7b/paper_configuration" \
    --csv fresh_summary.csv --output fresh_plot.png --no-show \
    --separate-y --ideal-fct 92
"${PYTHON}" nsdi_plotting.py "${RAW}/figure10/paper_configuration" \
    --csv fresh_summary.csv --output fresh_plot.png --no-show \
    --only-max --padding --separate-y --y-label "Collective Completion Time"

cp "${RAW}/figure1b/figure1b_multiseed."{png,pdf} "${PLOTS}/"
for result in figure7a figure7b; do
    mkdir -p "${PLOTS}/${result}"
    cp "${RAW}/${result}/paper_configuration/fresh_plot."{png,pdf} "${PLOTS}/${result}/"
done
mkdir -p "${PLOTS}/figure10"
cp "${RAW}/figure10/paper_configuration/fresh_plot."{png,pdf} "${PLOTS}/figure10/"

find "${RAW}" -name run_status.csv -print -exec awk -F, \
    'NR > 1 && $0 !~ /,pass,/ && $0 !~ /,resumed-pass,/ {bad=1} END {exit bad}' {} \;
printf 'REVIEWER QUICK SUBSET PASS\n' > "${ROOT}/PASS"

RESULTS_BASE="${ORDEREDCHAOS_RESULTS_DIR:-${SCRATCH}/orderedchaos_ae/results}"
RESULTS="${RESULTS_BASE}/short-${SLURM_JOB_ID}"
if [[ -e "${RESULTS}" ]]; then
    echo "Refusing existing result directory: ${RESULTS}" >&2
    exit 2
fi
mkdir -p "${RESULTS}"

copy_pair() {
    local source_base=$1
    local output_stem=$2
    local extension
    for extension in pdf png; do
        test -s "${source_base}.${extension}"
        cp "${source_base}.${extension}" "${RESULTS}/${output_stem}.${extension}"
    done
}

copy_pair "${PLOTS}/figure1b_multiseed" "Figure_1B"
copy_pair "${PLOTS}/figure6/pfld_proactive_errorbars_${figure6_seeds}seeds" "Figure_6A"
copy_pair "${PLOTS}/figure6/pfld_headroom_errorbars_${figure6_seeds}seeds" "Figure_6B"
copy_pair "${PLOTS}/figure7a/fresh_plot" "Figure_7A"
copy_pair "${PLOTS}/figure7b/fresh_plot" "Figure_7B"
copy_pair "${PLOTS}/figure10/fresh_plot" "Figure_10"
copy_pair "${PLOTS}/figure5/figure5_memory" "Figure_5"
printf 'PASS\n' > "${RESULTS}/PASS"
printf 'Execution: %s\nRun: %s\n' \
    "${ORDEREDCHAOS_EXECUTION_MODE:-slurm}" "${SLURM_JOB_ID}" \
    > "${RESULTS}/run.txt"

printf 'Results: %s\n' "${ROOT}"
printf 'Named plots: %s\n' "${RESULTS}"
