#!/bin/bash
#
# Builds Sublime, fetches the datasets, generates the workloads, runs the
# benchmarks and draws every figure and table in the paper. One command, from a
# clean clone, with nothing to set up first beyond the dependencies it checks
# for before it starts.
#
# Everything it produces goes in a `paper_results` directory *next to* the
# clone, so the repository itself stays clean.
#

set -euo pipefail

# Runs on Linux and macOS: bash 3.2 (what macOS ships as /bin/bash) and the BSD
# userland, so nothing below uses a GNU-only flag or a bash 4 feature.
OS=$(uname -s)

# The number of CPUs, which neither platform spells the same way.
cpu_count() {
    getconf _NPROCESSORS_ONLN 2> /dev/null || sysctl -n hw.ncpu 2> /dev/null || echo 4
}

# The figures of the paper proper.
PAPER_FIGURES=("accuracy" "skew" "vale_tuning" "expansion" "contraction" "accuracy_unbiased" "l2_size_function" "join_size")
# Sublime_MG, which is the journal extension, and is off unless asked for.
MG_FIGURES=("mg_accuracy" "mg_tail_latency" "mg_expansion" "mg_join_size")
FIGURE_OPTIONS=("${PAPER_FIGURES[@]}" "${MG_FIGURES[@]}")

FIGURES=""
WITH_MG=0
QUICK=0
RUN_TESTS=1

function print_help_message_exit() {
    cat <<EOF
Usage: evaluate.sh [options]

Builds everything, runs the benchmarks and draws the figures. With no options it
reproduces the figures of the paper.

Options:
  -f, --figures LIST  Comma-separated list of figures, instead of the default.
      --with-mg       Also run the Sublime_MG figures (the journal extension).
                      They roughly triple the running time.
      --quick         A few minutes rather than hours: one dataset, one memory
                      budget, two figures. Checks that the whole pipeline works
                      before committing a machine to the full run. The numbers
                      it produces are NOT the paper's.
      --skip-tests    Do not run the unit tests first.
  -h, --help          This message.

Figures of the paper:
  - accuracy:          average absolute error and insertion and query speed on real datasets   (Fig. 10)
  - skew:              average absolute error over synthetic Zipfian datasets of varying skew  (Fig. 11-A)
  - vale_tuning:       the memory saved by adaptively tuning VALE as the skew varies           (Fig. 11-B)
  - expansion:         average absolute error and memory on a growing stream                   (Fig.  8)
  - contraction:       average absolute error and memory as a stream's keys are deleted        (Fig. 12)
  - accuracy_unbiased: unbiased average absolute error, and insertion and query speed          (Fig. 13)
  - l2_size_function:  expanding Sublime_CS on the stream's l2 norm                            (Fig. 14)
  - join_size:         accuracy in estimating the size of a join of TPC-H tables               (Fig. 15/16)

Sublime_MG, the journal extension (--with-mg, or name them with -f):
  - mg_accuracy:       Sublime_MG against Misra-Gries, Space-Saving and Waving                 (Fig. 17)
  - mg_tail_latency:   the worst single insertion of each, in runs of its own                  (Fig. 18)
  - mg_expansion:      Sublime_MG across expansions, and what the error measure saves          (Fig. 19)
  - mg_join_size:      the join-size figure with MG and Sublime_MG added; draws its CMS and   (Fig. 20)
                       CS lines from join_size, which has to run alongside it
EOF
    exit $1
}

while [[ $# -gt 0 ]]; do
    case $1 in
        -f|--figures)
            [[ $# -ge 2 ]] || { echo "--figures needs a value"; print_help_message_exit 1; }
            FIGURES="$2"
            IFS="," read -ra FIGURES_ARRAY <<< "$FIGURES"
            for i in "${FIGURES_ARRAY[@]}"; do
                if ! printf "%s\n" "${FIGURE_OPTIONS[@]}" | grep -Fxq "$i"; then
                    echo "Unknown figure $i"
                    print_help_message_exit 1
                fi
            done
            shift 2
            ;;
        --with-mg)  WITH_MG=1;    shift ;;
        --quick)    QUICK=1;      shift ;;
        --skip-tests) RUN_TESTS=0; shift ;;
        -h|--help)  print_help_message_exit 0 ;;
        *)          echo "Unknown option $1"; print_help_message_exit 1 ;;
    esac
done

# What to run, if -f did not say.
if [[ -z "${FIGURES}" ]]; then
    if [[ ${QUICK} -eq 1 ]]; then
        FIGURES="accuracy,skew"
        [[ ${WITH_MG} -eq 1 ]] && FIGURES="${FIGURES},mg_accuracy"
    else
        FIGURES=$(IFS=,; echo "${PAPER_FIGURES[*]}")
        [[ ${WITH_MG} -eq 1 ]] && FIGURES="${FIGURES},$(IFS=,; echo "${MG_FIGURES[*]}")"
    fi
elif [[ ${WITH_MG} -eq 1 ]]; then
    FIGURES="${FIGURES},$(IFS=,; echo "${MG_FIGURES[*]}")"
fi

# Say which stage failed, rather than leaving the last command's message as the
# only clue. Every stage sets `stage` before it starts.
stage="startup"
started=$(date +%s)
on_error() {
    echo
    echo "=============================================================="
    echo " FAILED during: ${stage}"
    echo "=============================================================="
    exit 1
}
trap on_error ERR

announce() {
    stage="$1"
    echo
    echo "[==] ${1}"
}

# --------------------------------------------------------------------------
# Preflight: refuse early, and in one sentence, rather than obscurely later.
# --------------------------------------------------------------------------
announce "checking dependencies"
missing=()
for tool in cmake make git python3 curl; do
    command -v "${tool}" > /dev/null 2>&1 || missing+=("${tool}")
done
# Any C++ compiler CMake can find; on macOS `g++` is Apple Clang under that name.
if ! command -v c++ > /dev/null 2>&1 && ! command -v g++ > /dev/null 2>&1 \
        && ! command -v clang++ > /dev/null 2>&1; then
    missing+=("a C++17 compiler")
fi
if ! python3 -c "import venv" > /dev/null 2>&1; then
    missing+=("python3-venv (the 'venv' module)")
fi
if ! command -v latex > /dev/null 2>&1; then
    missing+=("latex (the plots are typeset with it; texlive-latex-base and texlive-fonts-recommended suffice)")
fi
if [[ ${#missing[@]} -gt 0 ]]; then
    echo "Missing dependencies:"
    printf '  - %s\n' "${missing[@]}"
    echo
    if [[ "${OS}" == "Darwin" ]]; then
        echo "On macOS (with Homebrew, after \`xcode-select --install\` for the compiler):"
        echo "  brew install cmake python git curl"
        echo "  brew install --cask mactex-no-gui     # the full TeX distribution, about 5 GB"
    else
        echo "On Debian or Ubuntu:"
        echo "  sudo apt-get install build-essential cmake git python3 python3-venv curl \\"
        echo "                       texlive-latex-base texlive-latex-recommended texlive-fonts-recommended"
    fi
    exit 1
fi

project_root=$(pwd)
results_parent=$(dirname "${project_root}")
# The full run downloads ~1.5 GB of datasets and writes ~8 GB of workloads.
needed_gb=$([[ ${QUICK} -eq 1 ]] && echo 3 || echo 12)
# POSIX `df -P -k` reads the same on Linux and macOS: 1K blocks, free in column 4.
available_gb=$(df -P -k "${results_parent}" | awk 'NR == 2 { print int($4 / 1048576) }')
if [[ ${available_gb} -lt ${needed_gb} ]]; then
    echo "Only ${available_gb} GB free at ${results_parent}; this needs about ${needed_gb} GB."
    exit 1
fi
echo "     toolchain ok, ${available_gb} GB free at ${results_parent}"
# The paper's numbers come from an x86 machine with AVX-512. Anything else runs
# the portable fallbacks (see include/portability.hpp), which give the same
# answers -- bar Sublime_CS's l2-norm estimator, whose AVX-512 and scalar paths
# have never agreed -- but not the same timings.
case "$(uname -m)" in
    x86_64|amd64)
        if [[ "${OS}" == "Darwin" ]]; then
            cpu_flags=$(sysctl -n machdep.cpu.leaf7_features 2> /dev/null || true)
        else
            cpu_flags=$(grep -m1 '^flags' /proc/cpuinfo 2> /dev/null || true)
        fi
        if ! echo "${cpu_flags}" | grep -qi avx512; then
            echo "     note: no AVX-512 on this CPU. Everything builds and runs, but the"
            echo "           timings will not be comparable to the paper's."
        fi
        ;;
    *)
        echo "     note: not an x86 machine. Sublime and the baselines run on portable code"
        echo "           paths, except SALSA, which is x86-only: it is skipped, and the"
        echo "           figures are drawn without it."
        echo "           Timings are not comparable to the paper's."
        ;;
esac

# --------------------------------------------------------------------------
# Build.
# --------------------------------------------------------------------------
announce "building Sublime and the benchmark suite"
mkdir -p build && cd build
# CMAKE_POLICY_VERSION_MINIMUM is for doctest, which the test suite fetches and
# which still declares cmake_minimum_required(VERSION 3.0); CMake 4 refuses that
# outright. Ignored, with a warning, by CMake 3.
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
make -j"$(cpu_count)"
cd "${project_root}"

if [[ ${RUN_TESTS} -eq 1 ]]; then
    announce "running the unit tests"
    (cd build && ctest --output-on-failure)
fi

# --------------------------------------------------------------------------
# Datasets, workloads, benchmarks, figures.
# --------------------------------------------------------------------------
mkdir -p "${results_parent}/paper_results"
cd "${results_parent}/paper_results"
export SUBLIME_QUICK=${QUICK}

announce "downloading the datasets"
bash "${project_root}/bench/scripts/download_datasets.sh"

announce "generating the workloads"
bash "${project_root}/bench/scripts/generate_datasets.sh" "${project_root}/build" real_datasets -f "${FIGURES}"

announce "preparing the Python environment"
# A virtualenv only if one is needed. The Docker image installs matplotlib at
# build time, so a containerised run needs no network here at all, and a
# reviewer who already has it system-wide is not made to download it again.
USING_VENV=0
if python3 -c "import matplotlib" > /dev/null 2>&1; then
    echo "     matplotlib is already available; no virtualenv needed"
else
    if [ ! -d ".venv" ]; then
        python3 -m venv .venv
    fi
    # shellcheck disable=SC1091
    source .venv/bin/activate
    USING_VENV=1
    python3 -m pip install --quiet --disable-pip-version-check -r "${project_root}/bench/scripts/requirements.txt"
fi

announce "running the benchmarks"
mkdir -p figures
python3 "${project_root}/bench/scripts/run_benchmarks.py" "${project_root}/build" workloads \
        -b ${FIGURES//,/ } $([[ ${QUICK} -eq 1 ]] && echo --quick)

announce "drawing the figures"
python3 "${project_root}/bench/scripts/plot.py" -f ${FIGURES//,/ }
[[ ${USING_VENV} -eq 1 ]] && deactivate

# --------------------------------------------------------------------------
trap - ERR
elapsed=$(( $(date +%s) - started ))
latest=$(ls -1t figures | head -1)
echo
echo "=============================================================="
printf " Done in %dh %02dm %02ds.\n" $((elapsed/3600)) $(((elapsed%3600)/60)) $((elapsed%60))
echo
echo " Figures and tables:"
echo "   $(cd figures/"${latest}" && pwd)"
ls -1 "figures/${latest}" | sed 's/^/     /'
if [[ ${QUICK} -eq 1 ]]; then
    echo
    echo " This was --quick: one dataset and one memory budget per figure."
    echo " Re-run without it to reproduce the paper's numbers."
fi
echo "=============================================================="
