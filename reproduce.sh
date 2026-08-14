#!/usr/bin/env bash

set -euo pipefail

usage() {
    printf '%s\n' \
        "Usage: ./reproduce.sh short|full [--slurm]" \
        "" \
        "The default backend runs locally with laptop-safe parallelism." \
        "Use --slurm to submit the optional Slurm workflow." \
        "" \
        "Local options:" \
        "  ORDEREDCHAOS_JOBS       Parallel simulator processes, default: 2" \
        "  ORDEREDCHAOS_WORKDIR    Generated-data root, default: .orderedchaos-work" \
        "" \
        "Slurm options:" \
        "  SCRATCH                         High-capacity generated-data root" \
        "  ORDEREDCHAOS_SLURM_ACCOUNT      Optional account passed to sbatch" \
        "  ORDEREDCHAOS_SLURM_PARTITION    Optional partition passed to sbatch"
}

if [[ $# -lt 1 || $# -gt 2 || "$1" != "short" && "$1" != "full" ]]; then
    usage >&2
    exit 2
fi
mode=$1
backend=local
if [[ $# -eq 2 ]]; then
    if [[ "$2" != "--slurm" ]]; then
        usage >&2
        exit 2
    fi
    backend=slurm
fi

repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "${repo}"

if [[ "${backend}" == "local" ]]; then
    export SCRATCH="${ORDEREDCHAOS_WORKDIR:-${repo}/.orderedchaos-work}"
else
    if [[ -z "${SCRATCH:-}" ]]; then
        echo "Set SCRATCH to a high-capacity filesystem for Slurm runs." >&2
        exit 2
    fi
    if ! command -v sbatch >/dev/null 2>&1; then
        echo "Slurm sbatch is required when --slurm is selected." >&2
        exit 2
    fi
fi

mkdir -p "${SCRATCH}/orderedchaos_ae/tmp"
export TMPDIR="${SCRATCH}/orderedchaos_ae/tmp"

if [[ ! -x "${SCRATCH}/orderedchaos_ae/venv/bin/python" ]]; then
    artifact/scripts/setup_environment.sh --install-uv
fi
export ORDEREDCHAOS_PYTHON="${SCRATCH}/orderedchaos_ae/venv/bin/python"
export PATH="${SCRATCH}/orderedchaos_ae/venv/bin:${PATH}"

if [[ "${backend}" == "local" ]]; then
    exec artifact/scripts/run_local.sh "${mode}"
fi

submit() {
    local arg
    local dependency_arg
    local dependency_spec
    local id
    local output
    local retry_seconds
    local state
    local status
    local -a pending_ids
    local -a site_args
    local -a submit_args

    site_args=()
    if [[ -n "${ORDEREDCHAOS_SLURM_ACCOUNT:-}" ]]; then
        site_args+=("--account=${ORDEREDCHAOS_SLURM_ACCOUNT}")
    fi
    if [[ -n "${ORDEREDCHAOS_SLURM_PARTITION:-}" ]]; then
        site_args+=("--partition=${ORDEREDCHAOS_SLURM_PARTITION}")
    fi

    while true; do
        submit_args=()
        for arg in "$@"; do
            if [[ "${arg}" != --dependency=afterok:* ]]; then
                submit_args+=("${arg}")
                continue
            fi

            dependency_spec="${arg#--dependency=afterok:}"
            pending_ids=()
            while [[ -n "${dependency_spec}" ]]; do
                if [[ "${dependency_spec}" == *:* ]]; then
                    id="${dependency_spec%%:*}"
                    dependency_spec="${dependency_spec#*:}"
                else
                    id="${dependency_spec}"
                    dependency_spec=""
                fi
                state=$(sacct -j "${id}" -X -n --format=State%30 2>/dev/null \
                    | awk 'NF {print $1; exit}')
                state="${state%%+}"
                case "${state}" in
                    COMPLETED)
                        ;;
                    FAILED|CANCELLED|TIMEOUT|OUT_OF_MEMORY|NODE_FAIL|PREEMPTED|BOOT_FAIL|DEADLINE)
                        printf 'Required Slurm job %s ended in %s.\n' \
                            "${id}" "${state}" >&2
                        return 1
                        ;;
                    *)
                        pending_ids+=("${id}")
                        ;;
                esac
            done

            if (( ${#pending_ids[@]} > 0 )); then
                dependency_arg=$(IFS=:; printf '%s' "${pending_ids[*]}")
                submit_args+=("--dependency=afterok:${dependency_arg}")
            fi
        done

        if output=$(sbatch --parsable "${site_args[@]}" "${submit_args[@]}" 2>&1); then
            printf '%s\n' "${output}"
            return 0
        else
            status=$?
        fi
        if [[ "${output}" == *QOSMaxSubmitJobPerUserLimit* ]]; then
            retry_seconds="${SLURM_SUBMIT_RETRY_SECONDS:-15}"
            printf '%s\n' \
                "Slurm submission limit reached; retrying in ${retry_seconds} seconds." >&2
            sleep "${retry_seconds}"
            continue
        fi
        if [[ "${output}" == *"Job dependency problem"* ]]; then
            retry_seconds="${SLURM_SUBMIT_RETRY_SECONDS:-15}"
            printf '%s\n' \
                "Slurm dependency state is still propagating; retrying in ${retry_seconds} seconds." >&2
            sleep "${retry_seconds}"
            continue
        fi
        printf '%s\n' "${output}" >&2
        return "${status}"
    done
}

build_job=$(submit artifact/slurm/build_and_smoke.sbatch)
export ORDEREDCHAOS_BINARY="${SCRATCH}/orderedchaos_ae/builds/${build_job}/htsim_uec"

if [[ "${mode}" == "short" ]]; then
    quick_job=$(submit --dependency="afterok:${build_job}" \
        artifact/slurm/reviewer_quick_subset.sbatch)
    printf '%s\n' \
        "Short Slurm reproduction submitted." \
        "Build job: ${build_job}" \
        "Experiment job: ${quick_job}" \
        "Results after completion: ${SCRATCH}/orderedchaos_ae/results/short-${quick_job}"
    exit 0
fi

fetch_job=$(submit artifact/slurm/fetch_inputs.sbatch)
analytical_job=$(submit artifact/slurm/analytical.sbatch)
figure1a_job=$(submit --dependency="afterok:${build_job}" \
    artifact/slurm/full_figure1a.sbatch)
figure1b_job=$(submit --dependency="afterok:${build_job}" \
    artifact/slurm/full_figure1b.sbatch)
figure6_job=$(submit --dependency="afterok:${build_job}" \
    artifact/slurm/full_figure6.sbatch)
figure6_plot=$(submit --dependency="afterok:${figure6_job}" \
    artifact/slurm/analyze_figure6.sbatch "${figure6_job}")
micro_job=$(submit --dependency="afterok:${build_job}" \
    artifact/slurm/full_micro_collectives.sbatch)
micro_plot=$(submit --dependency="afterok:${micro_job}" \
    artifact/slurm/analyze_micro.sbatch "${micro_job}")
figure10_job=$(submit --dependency="afterok:${build_job}" \
    artifact/slurm/full_figure10.sbatch)
figure9_job=$(submit --dependency="afterok:${build_job}" \
    artifact/slurm/full_figure9.sbatch)
figure13_job=$(submit --dependency="afterok:${build_job}" \
    artifact/slurm/camera_ready_figure13.sbatch)
figure8_job=$(submit --dependency="afterok:${build_job}" \
    artifact/slurm/full_figure8.sbatch)

figure11_job=$(submit --dependency="afterok:${build_job}:${fetch_job}" \
    artifact/slurm/figure11_trace.sbatch)
figure11_plot=$(submit \
    --dependency="afterok:${figure11_job}" \
    --export="ALL,FIG11_JOB=${figure11_job}" \
    artifact/slurm/analyze_figure11.sbatch)

comm_job=$(submit --dependency="afterok:${fetch_job}" \
    artifact/slurm/recompute_hpc_comm_share.sbatch)
figure12_job=$(submit \
    --dependency="afterok:${build_job}:${fetch_job}:${comm_job}" \
    --export="ALL,COMM_JOB_ID=${comm_job}" \
    artifact/slurm/figure12_trace.sbatch)

camera_job=$(submit --dependency="afterok:${build_job}" \
    artifact/slurm/camera_ready_new_images.sbatch)
camera_plot=$(submit --dependency="afterok:${camera_job}" \
    --export="ALL,RAW_JOB_ID=${camera_job}" \
    artifact/slurm/analyze_camera_ready_new_images.sbatch)
camera_incast_job=$(submit --dependency="afterok:${build_job}" \
    artifact/slurm/camera_ready_incast.sbatch)
camera_followups_job=$(submit --dependency="afterok:${build_job}" \
    artifact/slurm/camera_ready_probe_followups.sbatch)

final_dependencies="${analytical_job}:${figure1a_job}:${figure1b_job}:${figure6_plot}"
final_dependencies+=":${micro_plot}:${figure10_job}:${figure9_job}:${figure13_job}:${figure8_job}"
final_dependencies+=":${figure11_plot}:${figure12_job}:${camera_plot}:${camera_incast_job}:${camera_followups_job}"
collect_job=$(submit \
    --dependency="afterok:${final_dependencies}" \
    --export="ALL,ANALYTICAL_JOB=${analytical_job},FIG1A_JOB=${figure1a_job},FIG1B_JOB=${figure1b_job},FIG6_JOB=${figure6_job},MICRO_JOB=${micro_job},MICRO_PLOT=${micro_plot},FIG10_JOB=${figure10_job},FIG9_JOB=${figure9_job},FIG13_JOB=${figure13_job},FIG8_JOB=${figure8_job},FIG11_PLOT=${figure11_plot},FIG12_JOB=${figure12_job},CAMERA_JOB=${camera_job},CAMERA_PLOT=${camera_plot},CAMERA_INCAST_JOB=${camera_incast_job},CAMERA_FOLLOWUPS_JOB=${camera_followups_job}" \
    artifact/slurm/collect_full_results.sbatch)

printf '%s\n' \
    "Full Slurm reproduction submitted." \
    "Build job: ${build_job}" \
    "Input job: ${fetch_job}" \
    "Final collection job: ${collect_job}" \
    "Results after completion: ${SCRATCH}/orderedchaos_ae/results/full-${collect_job}" \
    "Monitor: squeue -j ${collect_job}" \
    "Final status: sacct -j ${collect_job} --format=JobID,State,Elapsed,MaxRSS,ExitCode"
