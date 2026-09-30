#!/bin/bash
#
# Puts the two baseline sketches this benchmark suite compares against in place:
# WavingSketch and SALSA. They are git submodules, and CMake runs this at
# configure time when BUILD_BENCHMARKS is on.
#
# Two things here are not just `git submodule update`.
#
# The artifact may arrive as a *tarball* rather than a clone -- which is how
# reproducibility reviewers usually receive one -- and then there is no `.git`
# for `git submodule` to work against. So each dependency falls back to a plain
# clone pinned to the same commit the submodule records, which is the commit
# this suite was evaluated with.
#
# WavingSketch also needs <unistd.h>, which it uses (for `usleep`) without
# including and which newer glibc and musl no longer pull in transitively. That
# used to be a `sed -i` into the submodule's own header, which is why the
# submodule showed as modified in `git status` -- and, since the insert was
# unconditional, gained one more copy of the line on every configure. The
# include is now forced from the build system instead (`-include unistd.h` on
# WavingLib, in bench/CMakeLists.txt), so the checkout here stays pristine.
#

set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
include_dir="${script_dir}/../sketches_benchmark/include"

# Commit pinned for each dependency, kept in step with .gitmodules.
waving_url="https://github.com/WavingSketch/Waving-Sketch.git"
waving_commit="c1d8175f7a6cb74ae1b2b6635411ff01a1fadede"
salsa_url="https://github.com/SALSA-ICDE2021/SALSA.git"
salsa_commit="04868162fbd97ac8065367013a8cbfd6859ad3d8"

# Fetches one dependency, by submodule where that is possible and by a pinned
# clone where it is not.
fetch_dependency() {
    local path=$1 url=$2 commit=$3
    if [ -n "$(ls -A "${path}" 2>/dev/null)" ]; then
        return 0                                # Already in place.
    fi
    if git -C "${script_dir}" rev-parse --git-dir > /dev/null 2>&1; then
        git -C "${script_dir}" submodule update --init "${path}"
        return 0
    fi
    echo "[setup_includes] no git checkout here, cloning $(basename "${path}") at ${commit:0:8}"
    git clone --quiet "${url}" "${path}"
    git -C "${path}" checkout --quiet "${commit}"
}

fetch_dependency "${include_dir}/waving_sketch" "${waving_url}" "${waving_commit}"
fetch_dependency "${include_dir}/salsa" "${salsa_url}" "${salsa_commit}"
