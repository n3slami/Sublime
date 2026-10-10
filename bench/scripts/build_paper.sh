#!/bin/bash
#
# Builds a version of the paper whose figures are the ones a benchmark run just
# drew. `evaluate.sh --with-paper` runs it after the figures; it can also be run
# on its own:
#
#   bash bench/scripts/build_paper.sh <figures directory> [<work directory>]
#   bash bench/scripts/build_paper.sh --check     # Only check the TeX installation.
#
# e.g. `paper_results/figures/<timestamp>`. It fetches the paper's source from
# the `camera_ready` branch of its repository, replaces every figure the run
# produced, compiles, and leaves the PDF in the figures directory as `paper.pdf`.
# A figure the run did not produce -- a run with `-f`, say -- keeps the original,
# and the script says which.
#
# The repository bundles the ACM class and bibliography style it is written for
# (acmart v2.20), so the paper builds the same with any TeX Live from 2022 on,
# whatever acmart that TeX Live ships, if any. What it needs from the TeX
# installation is checked before anything is fetched -- and before `evaluate.sh`
# starts benchmarking, which calls this with `--check` -- one file for each
# package a minimal installation lacks. The fonts are among them on purpose: ACM's
# Libertine, Biolinum and Inconsolata are what the class asks for, but an
# installation without them does not fail; acmart quietly falls back to Computer
# Modern.
#
# Written for bash 3.2, which is what macOS ships as /bin/bash.
#

set -euo pipefail

DRAFT_URL="https://github.com/n3slami/Sketchbook_Draft.git"
DRAFT_BRANCH="camera_ready"
MAIN_TEX="sublime"

# Each figure of the paper, as the draft names it, and the name the plotter
# gives the same figure (`<name>_(Fig_<n>).pdf`, for any <n>).
FIGURES="accuracy skew_vale_tuning expansion contraction accuracy_unbiased l2_size_function join_size"

if [[ $# -lt 1 ]]; then
    echo "Usage: build_paper.sh <figures directory> [<work directory>] | --check"
    exit 1
fi
check_only=0
[[ "$1" == "--check" ]] && check_only=1

# --------------------------------------------------------------------------
# The TeX installation: everything the source needs, checked before anything
# is fetched. `kpsewhich` finds a file the way LaTeX would.
# --------------------------------------------------------------------------
missing=""
for tool in pdflatex bibtex kpsewhich git; do
    command -v "${tool}" > /dev/null 2>&1 || missing="${missing} ${tool}"
done
if [[ -z "${missing}" ]]; then
    # Run from an empty directory: kpsewhich looks in the current one first.
    for file in libertine.sty newtxmath.sty zi4.sty binhex.tex hyperxmp.sty pbalance.sty \
                zref-user.sty pgfplots.sty breqn.sty thm-restate.sty makecell.sty; do
        [[ -n "$(cd / && kpsewhich "${file}" 2> /dev/null)" ]] || missing="${missing} ${file}"
    done
fi
if [[ -n "${missing}" ]]; then
    echo "Building the paper needs TeX files that are not installed:${missing}"
    echo "  Docker:         rebuild the image with --build-arg WITH_PAPER=1"
    echo "  Debian, Ubuntu: sudo apt-get install texlive-fonts-extra texlive-plain-generic texlive-latex-extra"
    echo "  macOS:          brew install --cask mactex-no-gui"
    exit 1
fi
if [[ ${check_only} -eq 1 ]]; then
    echo "     TeX installation ok for the paper"
    exit 0
fi

figures_dir=$(cd "$1" && pwd)
work_dir=${2:-"$(dirname "$(dirname "${figures_dir}")")/paper"}
mkdir -p "${work_dir}"
work_dir=$(cd "${work_dir}" && pwd)

# --------------------------------------------------------------------------
# The source, fetched fresh into a clone that is reused across runs.
# --------------------------------------------------------------------------
clone="${work_dir}/draft"
if [[ -d "${clone}/.git" ]]; then
    git -C "${clone}" fetch --quiet --depth 1 origin "${DRAFT_BRANCH}"
    git -C "${clone}" checkout --quiet --force FETCH_HEAD
else
    rm -rf "${clone}"
    git clone --quiet --depth 1 --branch "${DRAFT_BRANCH}" "${DRAFT_URL}" "${clone}"
fi
commit=$(git -C "${clone}" rev-parse --short HEAD)
echo "     paper source: ${DRAFT_URL} (${DRAFT_BRANCH}, ${commit})"

# A fresh copy to build in, so the clone stays exactly what was fetched.
build="${work_dir}/build"
rm -rf "${build}"
mkdir -p "${build}"
(cd "${clone}" && tar --exclude=.git -cf - .) | (cd "${build}" && tar -xf -)

# --------------------------------------------------------------------------
# The figures.
# --------------------------------------------------------------------------
replaced=""
kept=""
for name in ${FIGURES}; do
    generated=""
    for candidate in "${figures_dir}/${name}_(Fig_"*").pdf"; do
        [[ -f "${candidate}" ]] && generated="${candidate}"
    done
    if [[ -n "${generated}" ]]; then
        cp "${generated}" "${build}/figures/${name}_camera_ready.pdf"
        replaced="${replaced} ${name}"
    else
        kept="${kept} ${name}"
    fi
done
echo "     figures replaced:${replaced:- none}"
if [[ -n "${kept}" ]]; then
    echo "     figures kept from the published paper, not drawn by this run:${kept}"
fi

# --------------------------------------------------------------------------
# Compile: LaTeX, BibTeX, and LaTeX twice more for the references.
# --------------------------------------------------------------------------
cd "${build}"
latex_pass() {
    if ! pdflatex -interaction=nonstopmode -halt-on-error "${MAIN_TEX}" > /dev/null 2>&1; then
        echo "LaTeX failed; the end of ${build}/${MAIN_TEX}.log:"
        tail -20 "${MAIN_TEX}.log"
        exit 1
    fi
}
latex_pass
bibtex "${MAIN_TEX}" > /dev/null 2>&1 || { echo "BibTeX failed; see ${build}/${MAIN_TEX}.blg"; exit 1; }
latex_pass
latex_pass
if grep -qE 'Warning: (Citation|Reference) .* undefined' "${MAIN_TEX}.log"; then
    echo "     warning: the paper has undefined references or citations; see ${build}/${MAIN_TEX}.log"
fi

cp "${MAIN_TEX}.pdf" "${figures_dir}/paper.pdf"
echo "     paper: ${figures_dir}/paper.pdf"
