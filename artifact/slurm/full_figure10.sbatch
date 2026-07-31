#!/usr/bin/env bash
#SBATCH --job-name=oc_figure10
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=32
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

ROOT="${SCRATCH}/orderedchaos_ae/raw/figure10-full-${SLURM_JOB_ID}"
MEASURE="${SCRATCH}/orderedchaos_ae/measurements/figure10-full-${SLURM_JOB_ID}"
WORKERS="${ORDEREDCHAOS_WORKERS:-24}"
mkdir -p "${ROOT}" "${MEASURE}"

/usr/bin/time -v -o "${MEASURE}/simulation.time" \
    "${PYTHON}" artifact/scripts/run_simulation_matrix.py \
    --result figure10 --variant paper_configuration --seeds 1-12 \
    --output-root "${ROOT}" --workers "${WORKERS}" \
    > "${MEASURE}/simulation.out" 2> "${MEASURE}/simulation.err"

RUN="${ROOT}/figure10/paper_configuration"
"${PYTHON}" nsdi_plotting.py "${RUN}" --csv fresh_summary.csv \
    --output figure10.png --no-show --only-max --padding --separate-y \
    --annot-stats --y-label "Collective Completion Time"
