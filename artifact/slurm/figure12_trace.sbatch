#!/usr/bin/env bash
#SBATCH --job-name=oc_figure12
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=48
#SBATCH --mem=128G
#SBATCH --time=01:30:00
#SBATCH --output=slurm-%x-%j.out
#SBATCH --error=slurm-%x-%j.err

set -euo pipefail
cd "${SLURM_SUBMIT_DIR}"
PYTHON="${ORDEREDCHAOS_PYTHON:-${SCRATCH}/orderedchaos_ae/venv/bin/python}"
if [[ ! -x "${PYTHON}" ]]; then
    echo "Missing artifact Python: ${PYTHON}" >&2
    exit 2
fi

ROOT="${SCRATCH}/orderedchaos_ae/raw/figure12-full-${SLURM_JOB_ID}"
MEASURE="${SCRATCH}/orderedchaos_ae/measurements/figure12-full-${SLURM_JOB_ID}"
mkdir -p "${ROOT}" "${MEASURE}"

if [[ ! -f "${SCRATCH}/orderedchaos_ae/inputs/hpc/.READY" ]]; then
    printf '%s\n' \
        "Figure 12 skipped because the checksum-verified HPC binary traces are unavailable." \
        > "${ROOT}/SKIPPED.txt"
    cp "${ROOT}/SKIPPED.txt" "${MEASURE}/SKIPPED.txt"
    if [[ -s "${SCRATCH}/orderedchaos_ae/inputs/trace_input_status.tsv" ]]; then
        cp "${SCRATCH}/orderedchaos_ae/inputs/trace_input_status.tsv" \
            "${MEASURE}/trace_input_status.tsv"
    fi
    cat "${ROOT}/SKIPPED.txt"
    exit 0
fi

LOCAL_MODE="${ORDEREDCHAOS_LOCAL:-0}"
WORKERS="${ORDEREDCHAOS_WORKERS:-6}"
BINARY="${ORDEREDCHAOS_BINARY:?Set ORDEREDCHAOS_BINARY}"
COMM_JOB_ID="${COMM_JOB_ID:?Set COMM_JOB_ID to the communication-share analysis job ID}"
COMM_SHARE_CSV="${SCRATCH}/orderedchaos_ae/analysis/hpc-comm-share-${COMM_JOB_ID}/comm_share_per_workload.csv"
if [[ ! -s "${COMM_SHARE_CSV}" ]]; then
    echo "Missing freshly generated communication-share input: ${COMM_SHARE_CSV}" >&2
    exit 2
fi
pids=()
run_workload() {
    local workload=$1
    local result="figure12_${workload}"
    /usr/bin/time -v -o "${MEASURE}/${workload}.time" \
        "${PYTHON}" artifact/scripts/run_simulation_matrix.py \
        --binary "${BINARY}" \
        --result "${result}" --variant paper_configuration \
        --output-root "${ROOT}" --workers "${WORKERS}" \
        --algorithms ecmp,oblivious,reps,rss,pfld,rss_rack_tlp \
        > "${MEASURE}/${workload}.out" 2> "${MEASURE}/${workload}.err"
}

for workload in icon_32 icon_64 lammps_32 lammps_64 milc_32 milc_64; do
    if [[ "${LOCAL_MODE}" == "1" ]]; then
        run_workload "${workload}"
    else
        run_workload "${workload}" &
        pids+=("$!")
    fi
done

status=0
if [[ "${LOCAL_MODE}" != "1" ]]; then
    for pid in "${pids[@]}"; do
        wait "${pid}" || status=1
    done
fi
if [[ "${status}" -ne 0 ]]; then
    exit "${status}"
fi

/usr/bin/time -v -o "${MEASURE}/analysis.time" \
"${PYTHON}" artifact/scripts/analyze_hpc_matrix.py \
    --raw-root "${ROOT}" --variant paper_configuration --output-root "${ROOT}/analysis" \
    --comm-share-csv "${COMM_SHARE_CSV}"
