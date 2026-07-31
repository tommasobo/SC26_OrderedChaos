#!/usr/bin/env bash
#SBATCH --job-name=oc_fig11_analyze
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=64G
#SBATCH --time=00:45:00
#SBATCH --output=slurm-%x-%j.out
#SBATCH --error=slurm-%x-%j.err

set -euo pipefail
cd "${SLURM_SUBMIT_DIR}"
: "${FIG11_JOB:?Set FIG11_JOB to the completed Figure 11 job ID}"

OUTPUT="${SCRATCH}/orderedchaos_ae/analysis/figure11-${SLURM_JOB_ID}"
MEASURE="${SCRATCH}/orderedchaos_ae/measurements/figure11-analysis-${SLURM_JOB_ID}"
mkdir -p "${MEASURE}"
ROOT="${SCRATCH}/orderedchaos_ae/raw/figure11-trace-${FIG11_JOB}"

if [[ -s "${ROOT}/SKIPPED.txt" ]]; then
    mkdir -p "${OUTPUT}"
    cp "${ROOT}/SKIPPED.txt" "${OUTPUT}/SKIPPED.txt"
    cp "${ROOT}/SKIPPED.txt" "${MEASURE}/SKIPPED.txt"
    cat "${OUTPUT}/SKIPPED.txt"
    exit 0
fi

/usr/bin/time -v -o "${MEASURE}/analysis.time" \
    python3 artifact/scripts/analyze_figure11.py \
    --source-root "${ROOT}/source" \
    --rack-root "${ROOT}/rack" \
    --output "${OUTPUT}" \
    > "${MEASURE}/analysis.out" 2> "${MEASURE}/analysis.err"
