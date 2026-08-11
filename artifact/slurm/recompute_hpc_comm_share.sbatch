#!/usr/bin/env bash
#SBATCH --job-name=oc_ae_commshare
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=16G
#SBATCH --time=01:30:00
#SBATCH --output=slurm-%x-%j.out
#SBATCH --error=slurm-%x-%j.err

set -euo pipefail

cd "${SLURM_SUBMIT_DIR}"
OUT="${SCRATCH}/orderedchaos_ae/analysis/hpc-comm-share-${SLURM_JOB_ID}"
MEASURE="${SCRATCH}/orderedchaos_ae/measurements/hpc-comm-share-${SLURM_JOB_ID}.time"
mkdir -p "${OUT}" "$(dirname "${MEASURE}")"

if [[ ! -f "${SCRATCH}/orderedchaos_ae/inputs/hpc/.READY" ]]; then
    printf '%s\n' \
        "Communication-share analysis skipped because the checksum-verified HPC binary traces are unavailable." \
        > "${OUT}/SKIPPED.txt"
    cat "${OUT}/SKIPPED.txt"
    exit 0
fi

/usr/bin/time -v -o "${MEASURE}" \
    python3 scripts/validate_fig10_comm_share.py \
    --proxy-csv "${OUT}/no_cached_proxy.csv" \
    --out-dir "${OUT}" \
    --local-traces-root "${SCRATCH}/orderedchaos_ae/inputs/hpc" \
    --workloads icon_32,icon_64,lammps_32,lammps_64,milc_32,milc_64
