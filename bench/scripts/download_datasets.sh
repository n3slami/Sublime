#!/bin/bash
#
# Fetches the real datasets the benchmarks replay, into ./real_datasets.
#
# Script adapted from https://github.com/marcocosta97/grafite/blob/main/bench/scripts/download_datasets.sh.
#
# Every download is checked against a SHA-256 recorded here. A dataset that
# arrives truncated or replaced -- the Google Drive links in particular are not
# stable forever -- otherwise flows straight into workload generation and comes
# out the far end as plausible-looking numbers that are not the paper's.
#

set -uo pipefail
trap "exit" SIGINT

if ((BASH_VERSINFO[0] < 4)); then
    echo "Bash version >= 4 required for associative arrays."
    exit 1
fi

DIR_DATA="real_datasets"

declare -A urls
# The FIMI repository these two came from (fimi.uantwerpen.be) is gone: every
# path under it now 404s and its index is a stub. These are Mohammed Zaki's
# copies at RPI, which host the FIMI workshop data. Byte-for-byte the same files
# -- which is not a claim to take on trust, and is not taken on trust: both are
# checked against the SHA-256s below, recorded from the copies the paper's
# results were produced with.
urls["kosarak"]="https://www.cs.rpi.edu/~zaki/Workshops/FIMI/data/kosarak.dat.gz"
urls["webdocs"]="https://www.cs.rpi.edu/~zaki/Workshops/FIMI/data/webdocs.dat.gz"
urls["caida"]="https://github.com/StingySketch/Stingy-Sketch/raw/refs/heads/main/src/Frequency%20Estimation/0.dat"
urls["lineitem_ext"]="https://drive.usercontent.google.com"
urls["orders_ext"]="https://drive.usercontent.google.com"

# Downloads that arrive gzipped, and so are decompressed into `targets[...]`
# once they land. Note that the same kosarak file is also served from the same
# directory as plain `kosarak.dat` -- which is a gzip stream *despite the name*,
# with no .gz suffix and no Content-Encoding. Use the .gz URL below rather than
# that one: taking the other at face value leaves a compressed file where the
# workload generator expects text, and it reads it as garbage rather than
# failing, so a whole run completes and produces numbers that mean nothing.
declare -A gzipped
gzipped["kosarak"]=1
gzipped["webdocs"]=1

declare -A google_drive_file_ids
google_drive_file_ids["lineitem_ext"]="1wxyFeLXhBV_hMFfrXqX296ZaEGNz169r"
google_drive_file_ids["orders_ext"]="1TSbpDMZ7RR5mCdLEgD9P-6VGa2RPyGqr"

# The file each download is expected to leave behind. CAIDA's is `0.dat` rather
# than `caida.dat`: it is the first tenth of the 2018 trace, and the workload
# generator reads `0.dat` .. `10.dat` by those names. Checking for the wrong one
# is why this used to re-fetch it on every run and leave `0.dat.1`, `0.dat.2`
# and so on lying around.
declare -A targets
targets["kosarak"]="kosarak.dat"
targets["webdocs"]="webdocs.dat"
targets["caida"]="0.dat"
targets["lineitem_ext"]="lineitem_ext.tbl"
targets["orders_ext"]="orders_ext.tbl"

declare -A sha256
sha256["kosarak"]="b7855ba155567d52390aa2a9eb09bb91ca27b5c737d358010501038f42d13dcf"
sha256["webdocs"]="ab0f87cd26b9ecdce9a3e08a78e2f1609bf30cb71fa6c11f59d11a00663dc7cd"
sha256["caida"]="b86bc9c5b5daf38a3e3528f846dda545e7f6fe92e964fdf8bf83757cf067bd26"
sha256["lineitem_ext"]="89b781eac94bb142d654bf6e240f13f1adf049da46dd4c08a386c22b0328a7a0"
sha256["orders_ext"]="21b010d14cc2ac5c0eb10f7e0aa0ebdaf9bc67a6c81f1d5f35170802919b7d68"

# Fetches one dataset to a temporary name and moves it into place only once it
# is complete, so an interrupted run cannot leave a half file that the next run
# mistakes for a finished one.
download() {
    local dataset=$1
    local target="${DIR_DATA}/${targets[$dataset]}"
    echo "Downloading '${dataset}'..."
    if [[ "${dataset}" == *"_ext"* ]]; then
        local id=${google_drive_file_ids[$dataset]}
        curl -c ./cookie.txt -s -L "https://drive.google.com/uc?export=download&id=${id}" > /dev/null || return 1
        curl -fsSL -b ./cookie.txt \
             "https://drive.usercontent.google.com/download?id=${id}&confirm=$(awk '/download/ {print $NF}' ./cookie.txt)" \
             -o "${target}.part" || { rm -f ./cookie.txt; return 1; }
        rm -f ./cookie.txt
    else
        wget -q --show-progress -O "${target}.part" "${urls[$dataset]}" || return 1
    fi
    if [[ "${gzipped[$dataset]:-0}" == "1" ]]; then
        echo "Decompressing '${dataset}'..."
        mv "${target}.part" "${target}.gz"
        gzip -d -f "${target}.gz" || return 1      # Leaves ${target}.
        return 0
    fi
    mv "${target}.part" "${target}"
}

verify() {
    local dataset=$1
    local target="${DIR_DATA}/${targets[$dataset]}"
    local want=${sha256[$dataset]}
    local got
    got=$(sha256sum "${target}" | cut -d' ' -f1)
    if [[ "${got}" != "${want}" ]]; then
        echo "  !! '${target}' does not match its recorded checksum."
        echo "     expected ${want}"
        echo "     got      ${got}"
        echo "     Delete it and re-run to try again; if it keeps failing, the"
        echo "     source has changed and the results will not be the paper's."
        return 1
    fi
    return 0
}

mkdir -p "${DIR_DATA}"

# `evaluate.sh --quick` generates only the kosarak-derived workloads, so there
# is no reason to pull the other gigabyte and a half down first.
wanted=("${!urls[@]}")
if [[ "${SUBLIME_QUICK:-0}" == "1" ]]; then
    wanted=("kosarak")
    echo "Quick run: fetching kosarak only."
fi

failed=0
for dataset in "${wanted[@]}"; do
    target="${DIR_DATA}/${targets[$dataset]}"
    if [ -f "${target}" ]; then
        if verify "${dataset}"; then
            echo "File '${target}' already exists."
            continue
        fi
        failed=1
        continue
    fi
    if ! download "${dataset}"; then
        echo "  !! Download of '${dataset}' failed. Check your network and re-run."
        rm -f "${target}.part" "${target}.gz.part"
        failed=1
        continue
    fi
    verify "${dataset}" || failed=1
done

if [[ ${failed} -ne 0 ]]; then
    echo "One or more datasets could not be fetched or did not verify."
    exit 1
fi
echo "All datasets present and verified."
