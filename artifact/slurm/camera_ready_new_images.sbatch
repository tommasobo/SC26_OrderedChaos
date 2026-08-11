#!/usr/bin/env bash
#SBATCH --job-name=oc_camera_images
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=12
#SBATCH --mem=128G
#SBATCH --time=00:45:00
#SBATCH --output=slurm-%x-%j.out
#SBATCH --error=slurm-%x-%j.err

set -euo pipefail
cd "${SLURM_SUBMIT_DIR}"
PYTHON="${ORDEREDCHAOS_PYTHON:-${SCRATCH}/orderedchaos_ae/venv/bin/python}"
if [[ ! -x "${PYTHON}" ]]; then
    echo "Missing artifact Python: ${PYTHON}" >&2
    exit 2
fi

ROOT="${SCRATCH}/orderedchaos_ae/raw/camera-ready-new-images-${SLURM_JOB_ID}"
MEASURE="${SCRATCH}/orderedchaos_ae/measurements/camera-ready-new-images-${SLURM_JOB_ID}"
LOCAL_MODE="${ORDEREDCHAOS_LOCAL:-0}"
SOURCE_WORKERS="${ORDEREDCHAOS_WORKERS:-4}"
RACK_WORKERS=2
BINARY="${ORDEREDCHAOS_BINARY:?Set ORDEREDCHAOS_BINARY}"
if [[ "${LOCAL_MODE}" == "1" ]]; then
    RACK_WORKERS=1
fi
mkdir -p "${ROOT}" "${MEASURE}"

pids=()
run_source() {
    /usr/bin/time -v -o "${MEASURE}/source.time" \
        "${PYTHON}" artifact/scripts/run_simulation_matrix.py \
        --binary "${BINARY}" --result camera_ready_new_images \
        --variant paper_configuration \
        --algorithms ecmp,flowbender,flowlet,oblivious,reps,rss,pfld \
        --output-root "${ROOT}/source" --workers "${SOURCE_WORKERS}" \
        > "${MEASURE}/source.out" 2> "${MEASURE}/source.err"
}

run_rack() {
    /usr/bin/time -v -o "${MEASURE}/rack.time" \
        "${PYTHON}" artifact/scripts/run_simulation_matrix.py \
        --binary "${BINARY}" \
        --result camera_ready_new_images --variant paper_configuration \
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
