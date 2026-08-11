#!/usr/bin/env bash
#SBATCH --job-name=oc_ae_analytical
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=1
#SBATCH --mem=4G
#SBATCH --time=00:15:00
#SBATCH --output=slurm-%x-%j.out
#SBATCH --error=slurm-%x-%j.err

set -euo pipefail

cd "${SLURM_SUBMIT_DIR}"
OUT="${SCRATCH}/orderedchaos_ae/analysis/analytical-${SLURM_JOB_ID}"
MEASURE="${SCRATCH}/orderedchaos_ae/measurements/analytical-${SLURM_JOB_ID}"
mkdir -p "${MEASURE}"

/usr/bin/time -v -o "${MEASURE}/memory-literal-4kib.time" \
    python3 artifact/scripts/generate_memory_results.py \
    --output "${OUT}/memory-literal-4kib" --mtu-bytes 4096
/usr/bin/time -v -o "${MEASURE}/memory-claim-implied-4000b.time" \
    python3 artifact/scripts/generate_memory_results.py \
    --output "${OUT}/memory-claim-implied-4000b" --mtu-bytes 4000
/usr/bin/time -v -o "${MEASURE}/figure14-plot-only.time" \
    python3 artifact/scripts/plot_figure14_summary.py \
    --output "${OUT}/figure14-plot-only"
