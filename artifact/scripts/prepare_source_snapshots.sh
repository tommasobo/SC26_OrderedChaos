#!/usr/bin/env bash
set -euo pipefail

repo=$(git rev-parse --show-toplevel)
scratch=${SCRATCH:?Set SCRATCH to a high-capacity filesystem}
root=${scratch}/orderedchaos_ae/worktrees
compiler_patch=${repo}/artifact/patches/gcc12_compatibility.patch
absolute_rto_patch=${repo}/artifact/patches/absolute_rto.patch
mkdir -p "${root}"

prepare() {
    local name=$1
    local source_id=$2
    local archive_name=$3
    local path=${root}/${name}
    local archive="${repo}/artifact/source_snapshots/${archive_name}"

    if [[ ! -f "${path}/.orderedchaos_source_id" ]]; then
        test -s "${archive}"
        rm -rf "${path}"
        mkdir -p "${path}"
        tar -xzf "${archive}" -C "${path}"
        printf '%s\n' "${source_id}" > "${path}/.orderedchaos_source_id"
        git -C "${path}" init -q
        git -C "${path}" add .
        git -C "${path}" \
            -c user.name="OrderedChaos artifact" \
            -c user.email="artifact@orderedchaos.invalid" \
            -c commit.gpgsign=false \
            commit -q -m "Imported source snapshot ${name}"
    fi
    test "$(cat "${path}/.orderedchaos_source_id")" = "${source_id}"

    for patch in "${compiler_patch}" "${absolute_rto_patch}"; do
        if git -C "${path}" apply --check "${patch}" 2>/dev/null; then
            git -C "${path}" apply "${patch}"
        elif ! git -C "${path}" apply --reverse --check "${patch}" 2>/dev/null; then
            if [[ ${patch} == "${compiler_patch}" ]] \
                && grep -qE '^[[:space:]]+size_t newsize' "${path}/sim/utils/circular_buffer.h"; then
                sed -i '/#include <assert.h>/a #include <cstddef>' \
                    "${path}/sim/utils/circular_buffer.h"
                sed -i 's/size_t newsize/std::size_t newsize/' \
                    "${path}/sim/utils/circular_buffer.h"
                sed -i '/std::size_t newsize/s/\r$//' \
                    "${path}/sim/utils/circular_buffer.h"
                continue
            fi
            if [[ ${patch} == "${compiler_patch}" ]] \
                && grep -q '#include <cstddef>' "${path}/sim/utils/circular_buffer.h" \
                && grep -q 'std::size_t newsize' "${path}/sim/utils/circular_buffer.h"; then
                continue
            fi
            if [[ ${patch} == "${absolute_rto_patch}" ]] \
                && grep -q 'double rto_us' "${path}/sim/main_uec.cpp" \
                && grep -q 'UecSrc::_min_rto = timeFromUs(rto_us)' "${path}/sim/main_uec.cpp"; then
                continue
            fi
            echo "Unexpected source changes in ${path}" >&2
            exit 2
        fi
    done
    git -C "${path}" diff --check
    printf '%s %s (bundled source archive)\n' "${name}" "${source_id}"
}

prepare trace_source trace-source-v1 trace_source.tar.gz
prepare camera_ready_source camera-ready-source-v1 camera_ready_source.tar.gz
