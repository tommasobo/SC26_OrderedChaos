#!/usr/bin/env bash
#SBATCH --job-name=oc_probe_followups
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=32
#SBATCH --mem=128G
#SBATCH --time=02:00:00
#SBATCH --output=slurm-%x-%j.out
#SBATCH --error=slurm-%x-%j.err

set -euo pipefail
cd "${SLURM_SUBMIT_DIR}"

PYTHON="${ORDEREDCHAOS_PYTHON:-${SCRATCH}/orderedchaos_ae/venv/bin/python}"
if [[ ! -x "${PYTHON}" ]]; then
    echo "Missing artifact Python: ${PYTHON}" >&2
    exit 2
fi

ROOT="${SCRATCH}/orderedchaos_ae/raw/camera-probe-followups-${SLURM_JOB_ID}"
ANALYSIS="${SCRATCH}/orderedchaos_ae/analysis/camera-probe-followups-${SLURM_JOB_ID}"
MEASURE="${SCRATCH}/orderedchaos_ae/measurements/camera-probe-followups-${SLURM_JOB_ID}"
WORKERS="${ORDEREDCHAOS_WORKERS:-${SLURM_CPUS_PER_TASK:-16}}"
BINARY="${ORDEREDCHAOS_BINARY:?Set ORDEREDCHAOS_BINARY}"
mkdir -p "${ANALYSIS}" "${MEASURE}"

/usr/bin/time -v -o "${MEASURE}/probe_compounding_simulation.time" \
    "${PYTHON}" artifact/scripts/run_simulation_matrix.py \
    --binary "${BINARY}" \
    --result camera_ready_probe_ablation --variant paper_configuration \
    --output-root "${ROOT}/probe_compounding" --workers "${WORKERS}" \
    > "${MEASURE}/probe_compounding_simulation.out" \
    2> "${MEASURE}/probe_compounding_simulation.err"

/usr/bin/time -v -o "${MEASURE}/probe_compounding_plot.time" \
    "${PYTHON}" artifact/scripts/plot_probe_compounding.py \
    --run-root "${ROOT}/probe_compounding/camera_ready_probe_ablation/paper_configuration" \
    --output "${ANALYSIS}/probe_compounding" --drop-rate 0.005 \
    --workload "128-node 8-MiB tornado" --expected-flows 128 \
    > "${MEASURE}/probe_compounding_plot.out" \
    2> "${MEASURE}/probe_compounding_plot.err"

/usr/bin/time -v -o "${MEASURE}/periodic_probe_stress_simulation.time" \
    "${PYTHON}" artifact/scripts/run_simulation_matrix.py \
    --binary "${BINARY}" \
    --result camera_ready_periodic_probe_stress --variant stress_configuration \
    --output-root "${ROOT}/periodic_probe_stress" --workers "${WORKERS}" \
    > "${MEASURE}/periodic_probe_stress_simulation.out" \
    2> "${MEASURE}/periodic_probe_stress_simulation.err"

/usr/bin/time -v -o "${MEASURE}/periodic_probe_stress_plot.time" \
    "${PYTHON}" artifact/scripts/plot_periodic_probe_stress.py \
    --run-root "${ROOT}/periodic_probe_stress/camera_ready_periodic_probe_stress/stress_configuration" \
    --output "${ANALYSIS}/periodic_probe_stress" --drop-rate 0.005 \
    --workload "32-to-1 incast, 4 MiB per sender" --expected-flows 32 \
    > "${MEASURE}/periodic_probe_stress_plot.out" \
    2> "${MEASURE}/periodic_probe_stress_plot.err"

/usr/bin/time -v -o "${MEASURE}/probe_ablation_combined_plot.time" \
    "${PYTHON}" artifact/scripts/plot_probe_ablation_combined.py \
    --compounding-summary \
    "${ANALYSIS}/probe_compounding/camera_ready_probe_compounding_summary.csv" \
    --stress-summary \
    "${ANALYSIS}/periodic_probe_stress/camera_ready_periodic_probe_stress_summary.csv" \
    --output "${ANALYSIS}/probe_ablation" --drop-rate 0.005 \
    > "${MEASURE}/probe_ablation_combined_plot.out" \
    2> "${MEASURE}/probe_ablation_combined_plot.err"

printf 'PASS\n' > "${ANALYSIS}/PASS"
