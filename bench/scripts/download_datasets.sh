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

download() {
    DATASET=$1
    URL=${urls[${DATASET}]}
    echo "Downloading '${DATASET}'..."
    wget -q --progress=bar ${URL} -P ./${DIR_DATA}
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
            decompress ${dataset}
        fi
    fi
done

