#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
cd "${REPO_ROOT}"

UV_VERSION="0.9.24"
PYTHON_VERSION="3.13.5"
INSTALL_UV=0

usage() {
    printf '%s\n' \
        "Usage: artifact/scripts/setup_environment.sh [--install-uv]" \
        "" \
        "Create the OrderedChaos Python environment in the generated-data root." \
        "Set SCRATCH before running this script. Use --install-uv to install" \
        "uv ${UV_VERSION} under that root when it is not available."
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --install-uv)
            INSTALL_UV=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'Unknown argument: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ -z "${SCRATCH:-}" ]]; then
    printf '%s\n' \
        'SCRATCH is not set.' \
        'Set it to a writable work directory and run this script again.' >&2
    exit 2
fi

ROOT="${SCRATCH}/orderedchaos_ae"
TOOLS="${ROOT}/tools"
VENV="${ROOT}/venv"
UV_CACHE_DIR="${ROOT}/uv-cache"
UV_PYTHON_INSTALL_DIR="${ROOT}/python"
SCRATCH_UV="${TOOLS}/uv"
mkdir -p "${TOOLS}" "${UV_CACHE_DIR}" "${UV_PYTHON_INSTALL_DIR}"
export UV_CACHE_DIR UV_PYTHON_INSTALL_DIR

uv_binary=""
if [[ -x "${SCRATCH_UV}" ]]; then
    uv_binary="${SCRATCH_UV}"
elif command -v uv >/dev/null 2>&1; then
    uv_binary="$(command -v uv)"
fi

uv_matches() {
    [[ "$($1 --version 2>/dev/null)" == "uv ${UV_VERSION}" ]]
}

if [[ -n "${uv_binary}" ]] && ! uv_matches "${uv_binary}"; then
    if [[ "${INSTALL_UV}" -eq 0 ]]; then
        printf 'OrderedChaos requires uv %s, but found %s.\n' \
            "${UV_VERSION}" "$(${uv_binary} --version 2>/dev/null || printf unknown)" >&2
        printf '%s\n' 'Run this command again with --install-uv.' >&2
        exit 2
    fi
    uv_binary=""
fi

if [[ -z "${uv_binary}" ]]; then
    if [[ "${INSTALL_UV}" -eq 0 ]]; then
        printf 'uv %s is unavailable.\n' "${UV_VERSION}" >&2
        printf '%s\n' 'Run this command again with --install-uv.' >&2
        exit 2
    fi
    if ! command -v curl >/dev/null 2>&1; then
        printf '%s\n' 'curl is required to install uv but was not found.' >&2
        exit 2
    fi
    printf 'Installing uv %s in %s\n' "${UV_VERSION}" "${TOOLS}"
    curl --proto '=https' --tlsv1.2 --fail --location --silent --show-error \
        "https://astral.sh/uv/${UV_VERSION}/install.sh" | \
        env UV_INSTALL_DIR="${TOOLS}" UV_NO_MODIFY_PATH=1 sh
    uv_binary="${SCRATCH_UV}"
fi

if [[ ! -x "${uv_binary}" ]] || ! uv_matches "${uv_binary}"; then
    printf 'Failed to provision uv %s at %s.\n' \
        "${UV_VERSION}" "${uv_binary:-${SCRATCH_UV}}" >&2
    exit 2
fi

"${uv_binary}" python install --no-bin "${PYTHON_VERSION}"
"${uv_binary}" venv --python "${PYTHON_VERSION}" --clear "${VENV}"
"${uv_binary}" pip install --python "${VENV}/bin/python" \
    --requirement artifact/requirements.txt

"${VENV}/bin/python" - <<'PY'
import fitz
import matplotlib
import numpy
import pandas
import scipy
import seaborn
from matplotlib import font_manager

try:
    font_manager.findfont("Liberation Serif", fallback_to_default=False)
    plot_font = "Liberation Serif"
except ValueError:
    # DejaVu Serif is distributed with Matplotlib, so this path requires no
    # operating-system package or additional network download.
    font_manager.findfont("DejaVu Serif", fallback_to_default=False)
    plot_font = "DejaVu Serif (offline fallback)"

print("OrderedChaos Python environment is ready")
print("numpy", numpy.__version__)
print("pandas", pandas.__version__)
print("matplotlib", matplotlib.__version__)
print("seaborn", seaborn.__version__)
print("scipy", scipy.__version__)
print("PyMuPDF", fitz.version[0])
print("plot font", plot_font)
PY

printf 'Activate with: source %s/bin/activate\n' "${VENV}"
