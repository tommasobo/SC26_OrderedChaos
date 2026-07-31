#!/usr/bin/env bash
#SBATCH --job-name=oc_camera_analyze
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=128G
#SBATCH --time=01:00:00
#SBATCH --output=slurm-%x-%j.out
#SBATCH --error=slurm-%x-%j.err

set -euo pipefail
cd "${SLURM_SUBMIT_DIR}"
: "${RAW_JOB_ID:?Set RAW_JOB_ID to the Camera Ready New Images Slurm job ID}"
PYTHON="${ORDEREDCHAOS_PYTHON:-${SCRATCH}/orderedchaos_ae/venv/bin/python}"

ROOT="${SCRATCH}/orderedchaos_ae/raw/camera-ready-new-images-${RAW_JOB_ID}"
ANALYSIS_ROOT="${SCRATCH}/orderedchaos_ae/analysis/camera-ready-new-images-${RAW_JOB_ID}-analysis-${SLURM_JOB_ID}"
OUTPUT="${ANALYSIS_ROOT}/figure_axis"
MEASURE="${SCRATCH}/orderedchaos_ae/measurements/camera-ready-new-images-analysis-${SLURM_JOB_ID}"
if [[ -e "${ANALYSIS_ROOT}" ]]; then
    echo "Refusing existing analysis directory: ${ANALYSIS_ROOT}" >&2
    exit 2
fi
mkdir -p "${ANALYSIS_ROOT}" "${MEASURE}"

/usr/bin/time -v -o "${MEASURE}/analysis.time" \
    "${PYTHON}" artifact/scripts/analyze_camera_ready_new_images.py \
    --source-root "${ROOT}/source" --rack-root "${ROOT}/rack" \
    --variant paper_configuration --output "${OUTPUT}" \
    --executed-rto-us 172.439 --fct-rate 0.005 \
    --rto-rate 0.00005 --pfld-profile pfld_probe16

printf '%s\n' \
    "Camera Ready New Images analysis: ${ANALYSIS_ROOT}" \
    "Plots: ${OUTPUT}"
