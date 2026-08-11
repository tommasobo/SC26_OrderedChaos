#!/usr/bin/env bash
#SBATCH --job-name=oc_collect
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=8G
#SBATCH --time=00:15:00
#SBATCH --output=slurm-%x-%j.out
#SBATCH --error=slurm-%x-%j.err

set -euo pipefail
cd "${SLURM_SUBMIT_DIR}"

required_variables=(
    ANALYTICAL_JOB FIG1A_JOB FIG1B_JOB FIG6_JOB MICRO_JOB MICRO_PLOT
    FIG10_JOB FIG9_JOB FIG13_JOB FIG8_JOB FIG11_PLOT FIG12_JOB
    CAMERA_JOB CAMERA_PLOT CAMERA_INCAST_JOB CAMERA_FOLLOWUPS_JOB
)
for variable in "${required_variables[@]}"; do
    if [[ -z "${!variable:-}" ]]; then
        echo "Missing job identifier: ${variable}" >&2
        exit 2
    fi
done

RESULTS_BASE="${ORDEREDCHAOS_RESULTS_DIR:-${SCRATCH}/orderedchaos_ae/results}"
RESULTS="${RESULTS_BASE}/full-${SLURM_JOB_ID}"
EXTRA_FIGURES="${RESULTS}/ExtraFigures"
if [[ -e "${RESULTS}" ]]; then
    echo "Refusing existing result directory: ${RESULTS}" >&2
    exit 2
fi
mkdir -p "${RESULTS}" "${EXTRA_FIGURES}"
SKIPPED_SUMMARY="${RESULTS}/SKIPPED_RESULTS.txt"

copy_pair() {
    local source_base=$1
    local output_stem=$2
    local destination="${3:-${RESULTS}}"
    local extension
    for extension in pdf png; do
        test -s "${source_base}.${extension}"
        cp "${source_base}.${extension}" "${destination}/${output_stem}.${extension}"
    done
}

copy_optional_pair() {
    local source_base=$1
    local output_stem=$2
    local skip_file=$3
    if [[ -s "${source_base}.pdf" && -s "${source_base}.png" ]]; then
        copy_pair "${source_base}" "${output_stem}"
        return
    fi
    if [[ -s "${skip_file}" ]]; then
        printf '%s: ' "${output_stem}" >> "${SKIPPED_SUMMARY}"
        cat "${skip_file}" >> "${SKIPPED_SUMMARY}"
        cp "${skip_file}" "${RESULTS}/${output_stem}_SKIPPED.txt"
        return
    fi
    printf 'Missing output and skip record for %s\n' "${output_stem}" >&2
    return 1
}

copy_pair \
    "${SCRATCH}/orderedchaos_ae/raw/figure1a-full-${FIG1A_JOB}/figure1a" \
    "Figure_1A"
copy_pair \
    "${SCRATCH}/orderedchaos_ae/raw/figure1b-full-${FIG1B_JOB}/figure1b-pooled/figure1b_multiseed" \
    "Figure_1B"
copy_pair \
    "${SCRATCH}/orderedchaos_ae/analysis/analytical-${ANALYTICAL_JOB}/memory-claim-implied-4000b/figure5_memory" \
    "Figure_5"
copy_pair \
    "${SCRATCH}/orderedchaos_ae/analysis/figure6-full-${FIG6_JOB}/paper_configuration/pfld_proactive_errorbars_50seeds" \
    "Figure_6A"
copy_pair \
    "${SCRATCH}/orderedchaos_ae/analysis/figure6-full-${FIG6_JOB}/paper_configuration/pfld_headroom_errorbars_50seeds" \
    "Figure_6B"
copy_pair \
    "${SCRATCH}/orderedchaos_ae/analysis/micro-full-${MICRO_JOB}-analysis-${MICRO_PLOT}/figure7a/paper_configuration/fresh_plot" \
    "Figure_7A"
copy_pair \
    "${SCRATCH}/orderedchaos_ae/analysis/micro-full-${MICRO_JOB}-analysis-${MICRO_PLOT}/figure7b/paper_configuration/fresh_plot" \
    "Figure_7B"
copy_pair \
    "${SCRATCH}/orderedchaos_ae/raw/figure8-full-${FIG8_JOB}/figure8/paper_configuration/fresh_plot" \
    "Figure_8"
copy_pair \
    "${SCRATCH}/orderedchaos_ae/raw/figure9-full-${FIG9_JOB}/analysis/figure9" \
    "Figure_9"
copy_pair \
    "${SCRATCH}/orderedchaos_ae/raw/figure10-full-${FIG10_JOB}/figure10/paper_configuration/figure10" \
    "Figure_10"
copy_optional_pair \
    "${SCRATCH}/orderedchaos_ae/analysis/figure11-${FIG11_PLOT}/figure11" \
    "Figure_11" \
    "${SCRATCH}/orderedchaos_ae/analysis/figure11-${FIG11_PLOT}/SKIPPED.txt"
copy_optional_pair \
    "${SCRATCH}/orderedchaos_ae/raw/figure12-full-${FIG12_JOB}/analysis/plots/hpc_full_run_paper_speedup_wide" \
    "Figure_12" \
    "${SCRATCH}/orderedchaos_ae/raw/figure12-full-${FIG12_JOB}/SKIPPED.txt"
copy_pair \
    "${SCRATCH}/orderedchaos_ae/analysis/camera-ready-figure13-${FIG13_JOB}/final/figure13a/figure13" \
    "Figure_13A"
copy_pair \
    "${SCRATCH}/orderedchaos_ae/analysis/camera-ready-figure13-${FIG13_JOB}/final/figure13b/figure13" \
    "Figure_13B"
copy_pair \
    "${SCRATCH}/orderedchaos_ae/analysis/analytical-${ANALYTICAL_JOB}/figure14-plot-only/figure14a_probe_overhead" \
    "Figure_14A"
copy_pair \
    "${SCRATCH}/orderedchaos_ae/analysis/analytical-${ANALYTICAL_JOB}/figure14-plot-only/figure14b_probe_fct" \
    "Figure_14B"
copy_pair \
    "${SCRATCH}/orderedchaos_ae/analysis/camera-ready-new-images-${CAMERA_JOB}-analysis-${CAMERA_PLOT}/figure_axis/camera_ready_new_image1" \
    "Camera_Ready_New_Image_1"
copy_pair \
    "${SCRATCH}/orderedchaos_ae/analysis/camera-ready-incast-${CAMERA_INCAST_JOB}/camera_ready_new_image2" \
    "Camera_Ready_New_Image_2"
copy_pair \
    "${SCRATCH}/orderedchaos_ae/analysis/camera-ready-incast-${CAMERA_INCAST_JOB}/camera_ready_new_image2_linear" \
    "Camera_Ready_New_Image_2_Linear"
copy_pair \
    "${SCRATCH}/orderedchaos_ae/analysis/camera-host-followups-${CAMERA_FOLLOWUPS_JOB}/probe_ablation/camera_ready_probe_ablation" \
    "Camera_Ready_Probe_Ablation" \
    "${EXTRA_FIGURES}"
cp \
    artifact/data/camera_ready_probe_ablation_caption.tex \
    "${EXTRA_FIGURES}/Camera_Ready_Probe_Ablation_caption.tex"

cp \
    "${SCRATCH}/orderedchaos_ae/analysis/analytical-${ANALYTICAL_JOB}/memory-claim-implied-4000b/table1_formulas.csv" \
    "${RESULTS}/Table_I.csv"

{
    printf 'execution=%s\n' "${ORDEREDCHAOS_EXECUTION_MODE:-slurm}"
    printf 'analytical=%s\n' "${ANALYTICAL_JOB}"
    printf 'figure1a=%s\n' "${FIG1A_JOB}"
    printf 'figure1b=%s\n' "${FIG1B_JOB}"
    printf 'figure6=%s\n' "${FIG6_JOB}"
    printf 'micro=%s\n' "${MICRO_JOB}"
    printf 'micro_analysis=%s\n' "${MICRO_PLOT}"
    printf 'figure10=%s\n' "${FIG10_JOB}"
    printf 'figure9=%s\n' "${FIG9_JOB}"
    printf 'figure13=%s\n' "${FIG13_JOB}"
    printf 'figure8=%s\n' "${FIG8_JOB}"
    printf 'figure11_analysis=%s\n' "${FIG11_PLOT}"
    printf 'figure12=%s\n' "${FIG12_JOB}"
    printf 'camera_ready=%s\n' "${CAMERA_JOB}"
    printf 'camera_ready_analysis=%s\n' "${CAMERA_PLOT}"
    printf 'camera_ready_incast=%s\n' "${CAMERA_INCAST_JOB}"
    printf 'camera_host_followups=%s\n' "${CAMERA_FOLLOWUPS_JOB}"
} > "${RESULTS}/run.txt"

if [[ -s "${SKIPPED_SUMMARY}" ]]; then
    if [[ -s "${SCRATCH}/orderedchaos_ae/inputs/trace_input_status.tsv" ]]; then
        cp "${SCRATCH}/orderedchaos_ae/inputs/trace_input_status.tsv" \
            "${RESULTS}/Trace_Input_Status.tsv"
    fi
    printf 'Trace-dependent results skipped:\n'
    cat "${SKIPPED_SUMMARY}"
else
    rm -f "${SKIPPED_SUMMARY}"
fi
printf 'PASS\n' > "${RESULTS}/PASS"
printf 'Named plots: %s\n' "${RESULTS}"
