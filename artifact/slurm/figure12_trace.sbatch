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

SOURCE="${SCRATCH}/orderedchaos_ae/worktrees/trace_source"
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
BUILD_JOBS="${ORDEREDCHAOS_WORKERS:-16}"
CC_BIN="${ORDEREDCHAOS_CC:-$(command -v gcc-12 || command -v gcc)}"
CXX_BIN="${ORDEREDCHAOS_CXX:-$(command -v g++-12 || command -v g++)}"
COMM_JOB_ID="${COMM_JOB_ID:?Set COMM_JOB_ID to the communication-share analysis job ID}"
COMM_SHARE_CSV="${SCRATCH}/orderedchaos_ae/analysis/hpc-comm-share-${COMM_JOB_ID}/comm_share_per_workload.csv"
if [[ ! -s "${COMM_SHARE_CSV}" ]]; then
    echo "Missing freshly generated communication-share input: ${COMM_SHARE_CSV}" >&2
    exit 2
fi
git -C "${SOURCE}" diff --binary > "${MEASURE}/source_compatibility.patch"
cat "${SOURCE}/.orderedchaos_source_id" > "${MEASURE}/source_identifier.txt"

if [[ ! -x "${SOURCE}/build/htsim_uec" ]]; then
    /usr/bin/time -v -o "${MEASURE}/configure.time" \
        cmake -S "${SOURCE}/sim" -B "${SOURCE}/build" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="${CC_BIN}" -DCMAKE_CXX_COMPILER="${CXX_BIN}"
    /usr/bin/time -v -o "${MEASURE}/build.time" \
        cmake --build "${SOURCE}/build" --parallel "${BUILD_JOBS}"
fi

pids=()
run_source_workload() {
    local workload=$1
    local result="figure12_${workload}"
    /usr/bin/time -v -o "${MEASURE}/${workload}.time" \
        "${PYTHON}" artifact/scripts/run_simulation_matrix.py \
        --binary "${SOURCE}/build/htsim_uec" \
        --result "${result}" --variant paper_configuration \
        --output-root "${ROOT}" --workers "${WORKERS}" \
        --algorithms ecmp,oblivious,reps,rss,pfld \
        > "${MEASURE}/${workload}.out" 2> "${MEASURE}/${workload}.err"
}

for workload in icon_32 icon_64 lammps_32 lammps_64 milc_32 milc_64; do
    if [[ "${LOCAL_MODE}" == "1" ]]; then
        run_source_workload "${workload}"
    else
        run_source_workload "${workload}" &
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

# RACK-TLP uses the current validated implementation for every workload.
CURRENT_RACK_ROOT="${ROOT}/current-rack"
rack_pids=()
run_rack_workload() {
    local workload=$1
    local result="figure12_${workload}"
    /usr/bin/time -v -o "${MEASURE}/${workload}-current-rack.time" \
        "${PYTHON}" artifact/scripts/run_simulation_matrix.py \
        --result "${result}" --variant paper_configuration \
        --algorithms rss_rack_tlp \
        --output-root "${CURRENT_RACK_ROOT}" \
        --workers 1 --timeout-seconds 600 \
        > "${MEASURE}/${workload}-current-rack.out" \
        2> "${MEASURE}/${workload}-current-rack.err"
}

for workload in icon_32 icon_64 lammps_32 lammps_64 milc_32 milc_64; do
    if [[ "${LOCAL_MODE}" == "1" ]]; then
        run_rack_workload "${workload}"
    else
        run_rack_workload "${workload}" &
        rack_pids+=("$!")
    fi
done

status=0
if [[ "${LOCAL_MODE}" != "1" ]]; then
    for pid in "${rack_pids[@]}"; do
        wait "${pid}" || status=1
    done
fi
if [[ "${status}" -ne 0 ]]; then
    exit "${status}"
fi

variant=paper_configuration
drop=0.005
run_name=rss_rack_tlp_seed1_trim_off_drop_0.005
for workload in icon_32 icon_64 lammps_32 lammps_64 milc_32 milc_64; do
    result="figure12_${workload}"
    mkdir -p "${ROOT}/${result}/${variant}/drop_${drop}" \
        "${ROOT}/${result}/${variant}/runs"
    cp -al "${CURRENT_RACK_ROOT}/${result}/${variant}/drop_${drop}/rss_rack_tlp_seed1_trim_off.txt" \
        "${ROOT}/${result}/${variant}/drop_${drop}/"
    cp -al "${CURRENT_RACK_ROOT}/${result}/${variant}/runs/${run_name}" \
        "${ROOT}/${result}/${variant}/runs/"
    "${PYTHON}" artifact/scripts/replace_status_row.py \
        --base "${ROOT}/${result}/${variant}/run_status.csv" \
        --replacement "${CURRENT_RACK_ROOT}/${result}/${variant}/run_status.csv" \
        --algorithm rss_rack_tlp --append-if-missing
    printf '%s\n' \
        "Paper methods: bundled trace-compatible source snapshot" \
        "RACK-TLP comparator: current validated binary" \
        "Result: completed measurement" \
        > "${ROOT}/${result}/${variant}/MIXED_BINARY_PROVENANCE.txt"
done

/usr/bin/time -v -o "${MEASURE}/analysis.time" \
"${PYTHON}" artifact/scripts/analyze_hpc_matrix.py \
    --raw-root "${ROOT}" --variant paper_configuration --output-root "${ROOT}/analysis" \
    --comm-share-csv "${COMM_SHARE_CSV}"
