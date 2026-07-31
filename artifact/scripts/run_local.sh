#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 || "$1" != "short" && "$1" != "full" ]]; then
    echo "Usage: artifact/scripts/run_local.sh short|full" >&2
    exit 2
fi
mode=$1

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd -- "${script_dir}/../.." && pwd)"
cd "${repo}"

jobs="${ORDEREDCHAOS_JOBS:-2}"
if [[ ! "${jobs}" =~ ^[1-9][0-9]*$ ]]; then
    echo "ORDEREDCHAOS_JOBS must be a positive integer." >&2
    exit 2
fi

run_id="${ORDEREDCHAOS_RUN_ID:-local-$(date -u +%Y%m%d-%H%M%S)-$$}"
export ORDEREDCHAOS_EXECUTION_MODE=local
export ORDEREDCHAOS_LOCAL=1
export ORDEREDCHAOS_RESULTS_DIR="${ORDEREDCHAOS_RESULTS_DIR:-${repo}/results}"
export ORDEREDCHAOS_WORKERS="${jobs}"
export SLURM_CPUS_PER_TASK="${jobs}"
export SLURM_SUBMIT_DIR="${repo}"
mkdir -p "${ORDEREDCHAOS_RESULTS_DIR}"

run_step() {
    local id=$1
    local script=$2
    shift 2
    printf '\n[%s] %s\n' "${id}" "${script}"
    SLURM_JOB_ID="${id}" bash "${script}" "$@"
}

build_id="${run_id}-build"
run_step "${build_id}" artifact/slurm/build_and_smoke.sbatch

if [[ "${mode}" == "short" ]]; then
    quick_id="${run_id}-short"
    run_step "${quick_id}" artifact/slurm/reviewer_quick_subset.sbatch
    printf '\nShort local reproduction passed.\nResults: %s/short-%s\n' \
        "${ORDEREDCHAOS_RESULTS_DIR}" "${quick_id}"
    exit 0
fi

fetch_id="${run_id}-fetch"
analytical_id="${run_id}-analytical"
figure1a_id="${run_id}-figure1a"
figure1b_id="${run_id}-figure1b"
figure6_id="${run_id}-figure6"
figure6_plot_id="${run_id}-figure6-plot"
micro_id="${run_id}-micro"
micro_plot_id="${run_id}-micro-plot"
figure10_id="${run_id}-figure10"
figure9_id="${run_id}-figure9"
figure13_id="${run_id}-figure13"
figure8_id="${run_id}-figure8"
source_id="${run_id}-source"
figure11_id="${run_id}-figure11"
figure11_plot_id="${run_id}-figure11-plot"
comm_id="${run_id}-comm"
figure12_id="${run_id}-figure12"
camera_id="${run_id}-camera"
camera_plot_id="${run_id}-camera-plot"
collect_id="${run_id}-collect"

run_step "${fetch_id}" artifact/slurm/fetch_inputs.sbatch
run_step "${analytical_id}" artifact/slurm/analytical.sbatch
run_step "${figure1a_id}" artifact/slurm/full_figure1a.sbatch
run_step "${figure1b_id}" artifact/slurm/full_figure1b.sbatch
run_step "${figure6_id}" artifact/slurm/full_figure6.sbatch
run_step "${figure6_plot_id}" artifact/slurm/analyze_figure6.sbatch "${figure6_id}"
run_step "${micro_id}" artifact/slurm/full_micro_collectives.sbatch
run_step "${micro_plot_id}" artifact/slurm/analyze_micro.sbatch "${micro_id}"
run_step "${figure10_id}" artifact/slurm/full_figure10.sbatch
run_step "${figure9_id}" artifact/slurm/full_figure9.sbatch
run_step "${figure13_id}" artifact/slurm/camera_ready_figure13.sbatch
run_step "${figure8_id}" artifact/slurm/full_figure8.sbatch
run_step "${source_id}" artifact/slurm/build_source_snapshots.sbatch

export FIG11_JOB="${figure11_id}"
run_step "${figure11_id}" artifact/slurm/figure11_trace.sbatch
run_step "${figure11_plot_id}" artifact/slurm/analyze_figure11.sbatch

run_step "${comm_id}" artifact/slurm/recompute_hpc_comm_share.sbatch
export COMM_JOB_ID="${comm_id}"
run_step "${figure12_id}" artifact/slurm/figure12_trace.sbatch

run_step "${camera_id}" artifact/slurm/camera_ready_new_images.sbatch
export RAW_JOB_ID="${camera_id}"
run_step "${camera_plot_id}" artifact/slurm/analyze_camera_ready_new_images.sbatch

export ANALYTICAL_JOB="${analytical_id}"
export FIG1A_JOB="${figure1a_id}"
export FIG1B_JOB="${figure1b_id}"
export FIG6_JOB="${figure6_id}"
export MICRO_JOB="${micro_id}"
export MICRO_PLOT="${micro_plot_id}"
export FIG10_JOB="${figure10_id}"
export FIG9_JOB="${figure9_id}"
export FIG13_JOB="${figure13_id}"
export FIG8_JOB="${figure8_id}"
export FIG11_PLOT="${figure11_plot_id}"
export FIG12_JOB="${figure12_id}"
export CAMERA_JOB="${camera_id}"
export CAMERA_PLOT="${camera_plot_id}"
run_step "${collect_id}" artifact/slurm/collect_full_results.sbatch

printf '\nFull local reproduction passed.\nResults: %s/full-%s\n' \
    "${ORDEREDCHAOS_RESULTS_DIR}" "${collect_id}"
