#!/usr/bin/env bash
#SBATCH --job-name=oc_ae_build
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=16
#SBATCH --mem=64G
#SBATCH --time=00:30:00
#SBATCH --output=slurm-%x-%j.out
#SBATCH --error=slurm-%x-%j.err

set -euo pipefail

REPO="${SLURM_SUBMIT_DIR}"
RUN_ROOT="${SCRATCH}/orderedchaos_ae"
BUILD_DIR="${RUN_ROOT}/builds/${SLURM_JOB_ID}"
METRICS_DIR="${RUN_ROOT}/measurements/build_and_smoke/${SLURM_JOB_ID}"
BUILD_JOBS="${ORDEREDCHAOS_WORKERS:-${SLURM_CPUS_PER_TASK}}"
CC_BIN="${ORDEREDCHAOS_CC:-$(command -v gcc-12 || command -v gcc)}"
CXX_BIN="${ORDEREDCHAOS_CXX:-$(command -v g++-12 || command -v g++)}"

mkdir -p "${RUN_ROOT}/slurm" "${METRICS_DIR}"
cd "${REPO}"

record_environment() {
    {
        date --iso-8601=seconds
        hostname
        uname -a
        python3 --version
        cmake --version
        "${CXX_BIN}" --version
        env | sort
    } > "${METRICS_DIR}/environment.txt"
}

record_environment

/usr/bin/time -v -o "${METRICS_DIR}/configure.time" \
    cmake -S "${REPO}/sim" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="${CC_BIN}" \
    -DCMAKE_CXX_COMPILER="${CXX_BIN}"
/usr/bin/time -v -o "${METRICS_DIR}/build.time" \
    cmake --build "${BUILD_DIR}" --parallel "${BUILD_JOBS}"
/usr/bin/time -v -o "${METRICS_DIR}/ctest.time" \
    ctest --test-dir "${BUILD_DIR}" --output-on-failure --parallel "${BUILD_JOBS}" \
    > "${METRICS_DIR}/ctest.out" 2> "${METRICS_DIR}/ctest.err"

SMOKE_OUT="${RUN_ROOT}/raw/build-smoke-${SLURM_JOB_ID}/figure7_tornado_seed1"
mkdir -p "${SMOKE_OUT}"
/usr/bin/time -v -o "${METRICS_DIR}/smoke.time" \
    "${BUILD_DIR}/htsim_uec" \
    -end 10000 -seed 1 \
    -tm "${REPO}/scripts/connection_matrices/tornado_128_4000000.cm" \
    -topo "${REPO}/scripts/topologies/128_400.topo" \
    -q 148 -cwnd 148 -mtu 4160 -sack_threshold 0 -ecn 29 118 \
    -switch_random_drop_prob 5e-05 -fail_psn -1 -rto_ratio 2 \
    -load_balancing_algo rss -linkspeed 400000 \
    -rss_parameters mean_rtt 16 15 0 0 25 \
    -sender_cc_only -sender_cc_algo dctcp -nodes 128 -lgs_percent 10 \
    -disable_trim -precisefastlossrecovery 3 -pflr_proactive_probe 1 \
    -pflr_proactive_rtx_probe -no_droping_low_header -pflr_receiver_trim_probe_coalescing \
    > "${SMOKE_OUT}/stdout.log" 2> "${SMOKE_OUT}/stderr.log"

grep -q "finished at" "${SMOKE_OUT}/stdout.log"
sha256sum "${BUILD_DIR}/htsim_uec" > "${METRICS_DIR}/simulator.sha256"
printf 'build_and_smoke_status=PASS\n' > "${METRICS_DIR}/status.txt"
