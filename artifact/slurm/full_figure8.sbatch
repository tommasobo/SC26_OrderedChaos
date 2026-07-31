#!/usr/bin/env bash
#SBATCH --job-name=oc_figure8
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=16
#SBATCH --mem=96G
#SBATCH --time=01:30:00
#SBATCH --output=slurm-%x-%j.out
#SBATCH --error=slurm-%x-%j.err

set -euo pipefail
cd "${SLURM_SUBMIT_DIR}"
ROOT="${SCRATCH}/orderedchaos_ae/raw/figure8-full-${SLURM_JOB_ID}"
MEASURE="${SCRATCH}/orderedchaos_ae/measurements/figure8-full-${SLURM_JOB_ID}"
INPUT="${SCRATCH}/orderedchaos_ae/inputs/generated/ring_all_reduce_128_400000.cm"
WORKERS="${ORDEREDCHAOS_WORKERS:-8}"
mkdir -p "${MEASURE}" "$(dirname "${INPUT}")"
if [[ ! -f "${INPUT}" ]]; then
    python3 artifact/scripts/generate_ring_matrix.py --output "${INPUT}" --flow-bytes 400000
fi
/usr/bin/time -v -o "${MEASURE}/simulation.time" \
    python3 artifact/scripts/run_simulation_matrix.py \
    --result figure8 --variant paper_configuration \
    --output-root "${ROOT}" --workers "${WORKERS}" \
    > "${MEASURE}/simulation.out" 2> "${MEASURE}/simulation.err"

RUN="${ROOT}/figure8/paper_configuration"
/usr/bin/time -v -o "${MEASURE}/plot.time" \
    python3 nsdi_plotting.py "${RUN}" --csv fresh_summary.csv \
    --output fresh_plot.png --no-show --only-max --only-positive \
    --padding --separate-y --annot-stats --y-label "Collective Completion Time"
