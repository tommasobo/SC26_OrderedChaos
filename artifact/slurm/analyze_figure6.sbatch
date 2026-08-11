#!/usr/bin/env bash
#SBATCH --job-name=oc_ae_fig6_plot
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=16G
#SBATCH --time=00:15:00
#SBATCH --output=slurm-%x-%j.out
#SBATCH --error=slurm-%x-%j.err

set -euo pipefail

FIG6_JOB_ID="${1:?Usage: sbatch analyze_figure6.sbatch FIG6_JOB_ID}"
cd "${SLURM_SUBMIT_DIR}"
RAW="${SCRATCH}/orderedchaos_ae/raw/figure6-full-${FIG6_JOB_ID}"
OUT="${SCRATCH}/orderedchaos_ae/analysis/figure6-full-${FIG6_JOB_ID}"
mkdir -p "${OUT}"
for variant in paper_configuration; do
    prefix="figure6_${variant}"
    mkdir -p "${OUT}/${variant}"
    cp "${RAW}/${variant}/plots/${prefix}_summary.csv" \
       "${RAW}/${variant}/plots/${prefix}_raw.csv" \
       "${RAW}/${variant}/plots/pfld_headroom_errorbars_50seeds.png" \
       "${RAW}/${variant}/plots/pfld_headroom_errorbars_50seeds.pdf" \
       "${RAW}/${variant}/plots/pfld_proactive_errorbars_50seeds.png" \
       "${RAW}/${variant}/plots/pfld_proactive_errorbars_50seeds.pdf" \
       "${OUT}/${variant}/"
done
