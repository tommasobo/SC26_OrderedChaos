#!/usr/bin/env bash
#SBATCH --job-name=oc_ae_micro_plot
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=64G
#SBATCH --time=00:45:00
#SBATCH --output=slurm-%x-%j.out
#SBATCH --error=slurm-%x-%j.err

set -euo pipefail

MICRO_JOB_ID="${1:?Usage: sbatch analyze_micro.sbatch MICRO_JOB_ID}"
cd "${SLURM_SUBMIT_DIR}"
PYTHON="${ORDEREDCHAOS_PYTHON:-${SCRATCH}/orderedchaos_ae/venv/bin/python}"
if [[ ! -x "${PYTHON}" ]]; then
    echo "Missing artifact Python: ${PYTHON}" >&2
    exit 2
fi
RAW="${SCRATCH}/orderedchaos_ae/raw/micro-full-${MICRO_JOB_ID}"
OUT="${SCRATCH}/orderedchaos_ae/analysis/micro-full-${MICRO_JOB_ID}-analysis-${SLURM_JOB_ID}"
MEASURE="${SCRATCH}/orderedchaos_ae/measurements/micro-analysis-${MICRO_JOB_ID}-${SLURM_JOB_ID}"
mkdir -p "${OUT}" "${MEASURE}"
export MPLBACKEND=Agg

analyze() {
    local result="$1"
    local variant="$2"
    shift 2
    local root="${RAW}/${result}/${variant}"
    /usr/bin/time -v -o "${MEASURE}/${result}-${variant}.time" \
        "${PYTHON}" nsdi_plotting.py "${root}" \
        --csv fresh_summary.csv --output fresh_plot.png --no-show "$@" \
        > "${MEASURE}/${result}-${variant}.out" \
        2> "${MEASURE}/${result}-${variant}.err"
    mkdir -p "${OUT}/${result}/${variant}"
    cp "${root}/fresh_summary.csv" "${root}/fresh_plot.png" "${root}/fresh_plot.pdf" \
        "${OUT}/${result}/${variant}/"
}

analyze figure7a paper_configuration --separate-y --annot-stats --ideal-fct 172
analyze figure7b paper_configuration --separate-y --annot-stats --ideal-fct 92
