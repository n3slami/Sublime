#!/bin/bash

#
# This file is part of Sketchbook <https://github.com/n3slami/Sketchbook>.
# Copyright (C) 2025 Navid Eslami
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#

if [ "$#" -ne 2 ]; then
    echo "Illegal number of parameters, usage: generate_datasets.sh <build_path> <real_datasets_path>"
fi

BUILD_PATH=$(realpath $1)
if [ ! -d "$BUILD_PATH" ]; then
    echo "Build path does not exist"
    exit 1
fi
REAL_DATASETS_PATH=$(realpath $2)
if [ ! -d "$REAL_DATASETS_PATH" ]; then
    echo "Real datasets path does not exist"
    exit 1
fi

WORKLOAD_GEN_PATH=$(realpath $BUILD_PATH/bench/workload_gen)
if [ ! -f "$WORKLOAD_GEN_PATH" ]; then
    echo "Workload generator does not exist"
    exit 1
fi


OUT_PATH=$(realpath ./workloads)

generate_synthetic() {
    if ! test -f zipf_0.00; then
        echo "    [++] generating zipf_0.00"
        $WORKLOAD_GEN_PATH -t standard --fdist unif -o zipf_0.00
    else 
        echo "    [--] zipf_0.00 already generated"
    fi

    i=0
    x=0.20
    while [ $i -le 4 ]
    do
        if ! test -f zipf_${x}; then
            echo "    [++] generating zipf_${x}"
            $WORKLOAD_GEN_PATH -t standard --fdist zipf ${x} -o zipf_${x}
        else 
            echo "    [--] zipf_${x} already generated"
        fi
        x=$(echo $x + 0.2 | bc)
        i=$(($i + 1))
    done
}

generate_real() {
    if ! test -f caida; then
        echo "    [++] generating caida"
        $WORKLOAD_GEN_PATH -t standard_string --fdist real $REAL_DATASETS_PATH/0.dat \
                                                      real $REAL_DATASETS_PATH/1.dat \
                                                      real $REAL_DATASETS_PATH/2.dat \
                                                      real $REAL_DATASETS_PATH/3.dat \
                                                      real $REAL_DATASETS_PATH/4.dat \
                                                      real $REAL_DATASETS_PATH/5.dat \
                                                      real $REAL_DATASETS_PATH/6.dat \
                                                      real $REAL_DATASETS_PATH/7.dat \
                                                      real $REAL_DATASETS_PATH/8.dat \
                                                      real $REAL_DATASETS_PATH/9.dat \
                                                      real $REAL_DATASETS_PATH/10.dat \
                                                      -o caida
    else 
        echo "    [--] caida already generated"
    fi

    if ! test -f kosarak; then
        echo "    [++] generating kosarak"
        $WORKLOAD_GEN_PATH -t standard --fdist real $REAL_DATASETS_PATH/kosarak.dat --key-len-binary 0 -o kosarak
    else 
        echo "    [--] kosarak already generated"
    fi

    if ! test -f webdocs; then
        echo "    [++] generating webdocs"
        $WORKLOAD_GEN_PATH -t standard --fdist real $REAL_DATASETS_PATH/webdocs.dat --key-len-binary 0 -o webdocs
    else 
        echo "    [--] webdocs already generated"
    fi
}

generate_expand() {
    if ! test -f caida_expand; then
        echo "    [++] generating caida_expand "
        $WORKLOAD_GEN_PATH -t expand --measurement-period 200000 --fdist real $REAL_DATASETS_PATH/0.dat \
                                                                         real $REAL_DATASETS_PATH/1.dat \
                                                                         real $REAL_DATASETS_PATH/2.dat \
                                                                         real $REAL_DATASETS_PATH/3.dat \
                                                                         real $REAL_DATASETS_PATH/4.dat \
                                                                         real $REAL_DATASETS_PATH/5.dat \
                                                                         real $REAL_DATASETS_PATH/6.dat \
                                                                         real $REAL_DATASETS_PATH/7.dat \
                                                                         real $REAL_DATASETS_PATH/8.dat \
                                                                         real $REAL_DATASETS_PATH/9.dat \
                                                                         real $REAL_DATASETS_PATH/10.dat \
                                                                         -o caida_expand
    else 
        echo "    [--] caida_expand already generated"
    fi

}

generate_delete() {
    if ! test -f caida_delete; then
        echo "    [++] generating caida_delete"
        $WORKLOAD_GEN_PATH -t delete --fdist real $REAL_DATASETS_PATH/0.dat \
                                                  real $REAL_DATASETS_PATH/1.dat \
                                                  real $REAL_DATASETS_PATH/2.dat \
                                                  real $REAL_DATASETS_PATH/3.dat \
                                                  real $REAL_DATASETS_PATH/4.dat \
                                                  real $REAL_DATASETS_PATH/5.dat \
                                                  real $REAL_DATASETS_PATH/6.dat \
                                                  real $REAL_DATASETS_PATH/7.dat \
                                                  real $REAL_DATASETS_PATH/8.dat \
                                                  real $REAL_DATASETS_PATH/9.dat \
                                                  real $REAL_DATASETS_PATH/10.dat \
                                                  -o caida_delete
    else 
        echo "    [--] caida_delete already generated"
    fi
}


mkdir -p $OUT_PATH/synthetic && cd $OUT_PATH/synthetic || exit 1
if ! generate_synthetic ; then
    echo "[!!] generate_synthetic failed"
    exit 1
fi
echo "[!!] generate_synthetic done"

mkdir -p $OUT_PATH/real && cd $OUT_PATH/real || exit 1
if ! generate_real ; then
    echo "[!!] generate_real failed"
    exit 1
fi
echo "[!!] generate_real done"

mkdir -p $OUT_PATH/expand && cd $OUT_PATH/expand || exit 1
if ! generate_expand ; then
    echo "[!!] generate_expand failed"
    exit 1
fi
echo "[!!] generate_expand done"

mkdir -p $OUT_PATH/delete && cd $OUT_PATH/delete || exit 1
if ! generate_delete ; then
    echo "[!!] generate_delete failed"
    exit 1
fi
echo "[!!] generate_delete done"

echo "[!!] success, all workloads generated"
