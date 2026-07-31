#!/usr/bin/env bash
#SBATCH --job-name=oc_figure9
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=64G
#SBATCH --time=00:30:00
#SBATCH --output=slurm-%x-%j.out
#SBATCH --error=slurm-%x-%j.err

set -euo pipefail
cd "${SLURM_SUBMIT_DIR}"

ROOT="${SCRATCH}/orderedchaos_ae/raw/figure9-full-${SLURM_JOB_ID}"
MEASURE="${SCRATCH}/orderedchaos_ae/measurements/figure9-full-${SLURM_JOB_ID}"
WORKERS="${ORDEREDCHAOS_WORKERS:-2}"
mkdir -p "${ROOT}" "${MEASURE}"

/usr/bin/time -v -o "${MEASURE}/simulation.time" \
    python3 artifact/scripts/run_simulation_matrix.py \
    --result figure9 --variant paper_configuration \
    --output-root "${ROOT}" --workers "${WORKERS}" \
    > "${MEASURE}/simulation.out" 2> "${MEASURE}/simulation.err"

RUN="${ROOT}/figure9/paper_configuration/drop_5e-05"
/usr/bin/time -v -o "${MEASURE}/plot.time" \
    python3 artifact/scripts/plot_figure9.py \
    --rss-log "${RUN}/rss_seed1_trim_off.txt" \
    --pfld-log "${RUN}/pfld_probe1_seed1_trim_off.txt" \
    --output "${ROOT}/analysis" --display-mode bins \
    --time-bin-us 20 --fct-bin-us 1 --marker-size 6 --alpha 0.42
