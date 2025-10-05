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
generate_classical_comparison_bench() {
    i=0
    x=.10
    while [ $i -le 20 ]
    do
        if ! test -f zipf_${x}; then
            $WORKLOAD_GEN_PATH -t standard --fdist zipf ${x} -o zipf_${x}
        fi
        x=$(echo $x + 0.2 | bc)
        i=$(($i + 1))
    done
    if ! test -f zipf_1.00; then
        $WORKLOAD_GEN_PATH -t standard --fdist zipf 1.00 -o zipf_1.00
    fi
}

generate_real_bench() {
    #$WORKLOAD_GEN_PATH -t standard_string --fdist real $REAL_DATASETS_PATH/CAIDA.dat -o caida
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
}


: '
mkdir -p $OUT_PATH/classical_comparison_bench && cd $OUT_PATH/classical_comparison_bench || exit 1
if ! generate_classical_comparison_bench ; then
    echo "[!!] generate_classical_comparison_bench generation failed"
    exit 1
fi
echo "[!!] classical_comparison_bench generated"
'

mkdir -p $OUT_PATH/real_bench && cd $OUT_PATH/real_bench || exit 1
if ! generate_real_bench ; then
    echo "[!!] generate_real_bench generation failed"
    exit 1
fi
echo "[!!] generate_real_bench generated"

echo "[!!] success, all workloads generated"
