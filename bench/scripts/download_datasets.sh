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

# Written for the bash 3.2 macOS ships as /bin/bash, so no associative arrays:
# each table below is a `case` on the dataset's name instead.

DIR_DATA="real_datasets"
DATASETS="kosarak webdocs caida lineitem_ext orders_ext"

# The FIMI repository kosarak and webdocs came from (fimi.uantwerpen.be) is
# gone: every path under it now 404s and its index is a stub. These are Mohammed
# Zaki's copies at RPI, which host the FIMI workshop data. Byte-for-byte the same
# files -- which is not a claim to take on trust, and is not taken on trust: both
# are checked against the SHA-256s below, recorded from the copies the paper's
# results were produced with.
url_of() {
    case $1 in
        kosarak) echo "https://www.cs.rpi.edu/~zaki/Workshops/FIMI/data/kosarak.dat.gz" ;;
        webdocs) echo "https://www.cs.rpi.edu/~zaki/Workshops/FIMI/data/webdocs.dat.gz" ;;
        caida)   echo "https://github.com/StingySketch/Stingy-Sketch/raw/refs/heads/main/src/Frequency%20Estimation/0.dat" ;;
        *)       echo "https://drive.usercontent.google.com" ;;
    esac
}

# Downloads that arrive gzipped, and so are decompressed into their target once
# they land. Note that the same kosarak file is also served from the same
# directory as plain `kosarak.dat` -- which is a gzip stream *despite the name*,
# with no .gz suffix and no Content-Encoding. Use the .gz URL above rather than
# that one: taking the other at face value leaves a compressed file where the
# workload generator expects text, and it reads it as garbage rather than
# failing, so a whole run completes and produces numbers that mean nothing.
is_gzipped() {
    case $1 in
        kosarak|webdocs) return 0 ;;
        *)               return 1 ;;
    esac
}

google_drive_id_of() {
    case $1 in
        lineitem_ext) echo "1wxyFeLXhBV_hMFfrXqX296ZaEGNz169r" ;;
        orders_ext)   echo "1TSbpDMZ7RR5mCdLEgD9P-6VGa2RPyGqr" ;;
    esac
}

# The file each download is expected to leave behind. CAIDA's is `0.dat` rather
# than `caida.dat`: it is the first tenth of the 2018 trace, and the workload
# generator reads `0.dat` .. `10.dat` by those names. Checking for the wrong one
# is why this used to re-fetch it on every run and leave `0.dat.1`, `0.dat.2`
# and so on lying around.
target_of() {
    case $1 in
        caida) echo "0.dat" ;;
        *_ext) echo "$1.tbl" ;;
        *)     echo "$1.dat" ;;
    esac
}

sha256_of() {
    case $1 in
        kosarak)      echo "b7855ba155567d52390aa2a9eb09bb91ca27b5c737d358010501038f42d13dcf" ;;
        webdocs)      echo "ab0f87cd26b9ecdce9a3e08a78e2f1609bf30cb71fa6c11f59d11a00663dc7cd" ;;
        caida)        echo "b86bc9c5b5daf38a3e3528f846dda545e7f6fe92e964fdf8bf83757cf067bd26" ;;
        lineitem_ext) echo "89b781eac94bb142d654bf6e240f13f1adf049da46dd4c08a386c22b0328a7a0" ;;
        orders_ext)   echo "21b010d14cc2ac5c0eb10f7e0aa0ebdaf9bc67a6c81f1d5f35170802919b7d68" ;;
    esac
}

# A file's SHA-256: `sha256sum` on Linux, `shasum` on macOS, OpenSSL elsewhere.
file_sha256() {
    if command -v sha256sum > /dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum > /dev/null 2>&1; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        openssl dgst -sha256 -r "$1" | cut -d' ' -f1
    fi
}

# Fetches one dataset to a temporary name and moves it into place only once it
# is complete, so an interrupted run cannot leave a half file that the next run
# mistakes for a finished one.
download() {
    local dataset=$1
    local target
    target="${DIR_DATA}/$(target_of "${dataset}")"
    echo "Downloading '${dataset}'..."
    if [[ "${dataset}" == *"_ext"* ]]; then
        local id
        id=$(google_drive_id_of "${dataset}")
        curl -c ./cookie.txt -s -L "https://drive.google.com/uc?export=download&id=${id}" > /dev/null || return 1
        curl -fsSL -b ./cookie.txt \
             "https://drive.usercontent.google.com/download?id=${id}&confirm=$(awk '/download/ {print $NF}' ./cookie.txt)" \
             -o "${target}.part" || { rm -f ./cookie.txt; return 1; }
        rm -f ./cookie.txt
    else
        curl -fL --progress-bar -o "${target}.part" "$(url_of "${dataset}")" || return 1
    fi
    if is_gzipped "${dataset}"; then
        echo "Decompressing '${dataset}'..."
        mv "${target}.part" "${target}.gz"
        gzip -d -f "${target}.gz" || return 1      # Leaves ${target}.
        return 0
    fi
    mv "${target}.part" "${target}"
}

verify() {
    local dataset=$1
    local target want got
    target="${DIR_DATA}/$(target_of "${dataset}")"
    want=$(sha256_of "${dataset}")
    got=$(file_sha256 "${target}")
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
wanted="${DATASETS}"
if [[ "${SUBLIME_QUICK:-0}" == "1" ]]; then
    wanted="kosarak"
    echo "Quick run: fetching kosarak only."
fi

failed=0
for dataset in ${wanted}; do
    target="${DIR_DATA}/$(target_of "${dataset}")"
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
