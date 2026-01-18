#!/bin/bash

# Script adapted from https://github.com/marcocosta97/grafite/blob/main/bench/scripts/download_datasets.sh.

trap "exit" SIGINT

if ((BASH_VERSINFO[0] < 4)); then
    echo "Bash version >= 4 required for associative arrays."
    exit 1
fi

DIR_DATA="real_datasets"

# Set download urls
declare -A urls
urls["kosarak"]="http://fimi.uantwerpen.be/data/kosarak.dat"
urls["webdocs"]="http://fimi.uantwerpen.be/data/webdocs.dat.gz"
urls["caida"]="https://github.com/StingySketch/Stingy-Sketch/raw/refs/heads/main/src/Frequency%20Estimation/0.dat"
declare -A google_drive_file_ids
google_drive_file_ids["lineitem_ext"]="1wxyFeLXhBV_hMFfrXqX296ZaEGNz169r"
google_drive_file_ids["orders_ext"]="1TSbpDMZ7RR5mCdLEgD9P-6VGa2RPyGqr"
urls["lineitem_ext"]="https://drive.usercontent.google.com"
urls["orders_ext"]="https://drive.usercontent.google.com"

download() {
    DATASET=$1
    URL=${urls[${DATASET}]}
    echo "Downloading '${DATASET}'..."
    if [[ "$DATASET" == *"_ext"* ]]; then
        curl -c ./cookie.txt -s -L "https://drive.google.com/uc?export=download&id=${google_drive_file_ids[$DATASET]}" > /dev/null
        curl -Lb ./cookie.txt "https://drive.usercontent.google.com/download?id=${google_drive_file_ids[$DATASET]}&confirm=$(awk '/download/ {print $NF}' ./cookie.txt)" -o $DATASET.tbl
        rm ./cookie.txt
    else
        wget -q --progress=bar ${URL} -P ./${DIR_DATA}
    fi
    return $?
}

decompress() {
    FILE=$1
    echo "Decompressing '${FILE}'..."
    gzip -d ${FILE}
    return $?
}

# Create data directory
if [ ! -d "${DIR_DATA}" ]; then
    mkdir -p "${DIR_DATA}";
fi

# Download datasets
for dataset in ${!urls[@]}; do
    FILE_DAT=${DIR_DATA}/${dataset}.dat
    if [ -f ${FILE_DAT} ]; then
        echo "File '${FILE_DAT}' already exists."
    else 
        download ${dataset}
        if [ $? -neq 0 ]; then
            echo "Download failed. Please try again."
        fi
        if [[ "${dataset}" == "webdocs" ]]; then
            decompress ${FILE_DAT}.gz
        fi
    fi
done

