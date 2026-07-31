#!/usr/bin/env bash
#SBATCH --job-name=oc_source_build
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=32
#SBATCH --mem=64G
#SBATCH --time=00:30:00
#SBATCH --output=slurm-%x-%j.out
#SBATCH --error=slurm-%x-%j.err

set -euo pipefail
cd "${SLURM_SUBMIT_DIR}"

MEASURE="${SCRATCH}/orderedchaos_ae/measurements/source-build-${SLURM_JOB_ID}"
BUILD_JOBS="${ORDEREDCHAOS_WORKERS:-16}"
CC_BIN="${ORDEREDCHAOS_CC:-$(command -v gcc-12 || command -v gcc)}"
CXX_BIN="${ORDEREDCHAOS_CXX:-$(command -v g++-12 || command -v g++)}"
mkdir -p "${MEASURE}"

artifact/scripts/prepare_source_snapshots.sh > "${MEASURE}/prepare.out"

for name in trace_source camera_ready_source; do
    worktree="${SCRATCH}/orderedchaos_ae/worktrees/${name}"
    cmake -S "${worktree}/sim" -B "${worktree}/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="${CC_BIN}" \
        -DCMAKE_CXX_COMPILER="${CXX_BIN}" \
        > "${MEASURE}/${name}-configure.out" \
        2> "${MEASURE}/${name}-configure.err"
    /usr/bin/time -v -o "${MEASURE}/${name}-build.time" \
        cmake --build "${worktree}/build" --parallel "${BUILD_JOBS}" \
        > "${MEASURE}/${name}-build.out" 2> "${MEASURE}/${name}-build.err"
done

/usr/bin/time -v -o "${MEASURE}/current-build.time" \
    cmake --build build --parallel "${BUILD_JOBS}" \
    > "${MEASURE}/current-build.out" 2> "${MEASURE}/current-build.err"

for binary in \
    "${SCRATCH}/orderedchaos_ae/worktrees/trace_source/build/htsim_uec" \
    "${SCRATCH}/orderedchaos_ae/worktrees/camera_ready_source/build/htsim_uec" \
    build/htsim_uec; do
    test -x "${binary}"
done
