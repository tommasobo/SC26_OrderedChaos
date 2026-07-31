#!/usr/bin/env bash
#SBATCH --job-name=oc_ae_fetch
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=4G
#SBATCH --time=01:00:00
#SBATCH --output=slurm-%x-%j.out
#SBATCH --error=slurm-%x-%j.err

set -euo pipefail

cd "${SLURM_SUBMIT_DIR}"
INPUT_ROOT="${SCRATCH}/orderedchaos_ae/inputs"
STATUS_FILE="${INPUT_ROOT}/trace_input_status.tsv"
TRACE_BASE_URL="${ORDEREDCHAOS_TRACE_BASE_URL:-http://storage2.spcl.ethz.ch/traces}"
FETCH_RETRIES="${ORDEREDCHAOS_FETCH_RETRIES:-5}"
FETCH_RETRY_DELAY="${ORDEREDCHAOS_FETCH_RETRY_DELAY:-5}"
mkdir -p "${INPUT_ROOT}/llama7b_n4" "${INPUT_ROOT}/hpc"
rm -f "${INPUT_ROOT}/llama7b_n4/.READY" "${INPUT_ROOT}/hpc/.READY"
printf 'family\tpath\tstatus\tdetail\n' > "${STATUS_FILE}"

fetch() {
    local family=$1
    local url=$2
    local output=$3
    local expected
    local observed
    local relative

    mkdir -p "$(dirname "${output}")"
    relative="./${output#${INPUT_ROOT}/}"
    expected=$(awk -v path="${relative}" '$2 == path {print $1}' \
        artifact/data/public_trace_sha256.txt)
    if [[ -z "${expected}" ]]; then
        printf '%s\t%s\tfailed\tmissing expected checksum\n' \
            "${family}" "${relative}" >> "${STATUS_FILE}"
        return 1
    fi

    if [[ -s "${output}" ]]; then
        observed=$(sha256sum "${output}" | awk '{print $1}')
        if [[ "${observed}" == "${expected}" ]]; then
            printf 'Already present and verified: %s\n' "${output}"
            printf '%s\t%s\tready\tverified existing file\n' \
                "${family}" "${relative}" >> "${STATUS_FILE}"
            return 0
        fi
        printf 'Discarding file with an invalid checksum: %s\n' "${output}" >&2
        rm -f "${output}"
    fi

    rm -f "${output}.part"
    if ! curl --fail --location \
        --retry "${FETCH_RETRIES}" --retry-delay "${FETCH_RETRY_DELAY}" \
        --output "${output}.part" "${url}"; then
        rm -f "${output}.part"
        printf '%s\t%s\tskipped\tdownload unavailable: %s\n' \
            "${family}" "${relative}" "${url}" >> "${STATUS_FILE}"
        return 1
    fi

    observed=$(sha256sum "${output}.part" | awk '{print $1}')
    if [[ "${observed}" != "${expected}" ]]; then
        rm -f "${output}.part"
        printf '%s\t%s\tskipped\tchecksum mismatch\n' \
            "${family}" "${relative}" >> "${STATUS_FILE}"
        return 1
    fi

    mv "${output}.part" "${output}"
    printf '%s\t%s\tready\tdownloaded and verified\n' \
        "${family}" "${relative}" >> "${STATUS_FILE}"
}

llama_ready=1
if ! fetch llama \
    "${TRACE_BASE_URL}/ai/llama/Llama7B_N4_GPU16_TP1_PP1_DP16_BS32/llama.bin" \
    "${INPUT_ROOT}/llama7b_n4/llama.bin"; then
    llama_ready=0
fi

hpc_ready=1
for workload in icon_32 icon_64 lammps_32 lammps_64 milc_32 milc_64; do
    app="${workload%_*}"
    if ! fetch hpc \
        "${TRACE_BASE_URL}/hpc/${app}/${workload}/${workload}.bin" \
        "${INPUT_ROOT}/hpc/${app}/${workload}/${workload}.bin"; then
        hpc_ready=0
    fi
done

(cd "${INPUT_ROOT}" && find . -type f -name '*.bin' -print0 \
    | sort -z \
    | xargs -0 -r sha256sum) \
    > "${INPUT_ROOT}/SHA256SUMS"

if [[ "${llama_ready}" == "1" ]]; then
    touch "${INPUT_ROOT}/llama7b_n4/.READY"
else
    printf '%s\n' \
        "LLaMA trace input is unavailable. Figure 11 will be skipped." >&2
fi
if [[ "${hpc_ready}" == "1" ]]; then
    touch "${INPUT_ROOT}/hpc/.READY"
else
    printf '%s\n' \
        "One or more HPC trace inputs are unavailable. Figure 12 will be skipped." >&2
fi

cat "${STATUS_FILE}"
du -sh "${INPUT_ROOT}"
