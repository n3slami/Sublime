#!/bin/bash

#
# This file is part of Sketchbook <--->.
# Copyright (C) 2025 ---
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

FIGURE_OPTIONS=("accuracy" "skew" "vale_tuning" "expansion" "contraction" "accuracy_unbiased" "l2_size_function" "join_size")

FIGURES="accuracy,skew,vale_tuning,expansion,contraction,accuracy_unbiased,l2_size_function,join_size"

function print_help_message_exit() {
    echo "Usage: generate_datasets.sh <build_path> <real_datasets_path> [-f|--figures ${FIGURES}]"
    echo "The figures parameter is a list of comma-separated names describing what workloads to generate:"
    echo "      - accuracy:          measures average absolute error and insertion and query speed on real datasets          (Fig. 10 in the paper)"
    echo "      - skew:              measures average absolute error over synthetic Zipfian datasets with varying skew       (Fig. 11-A) in the paper)"
    echo "      - vale_tuning:       measures the memory savings of adaptively tuning VALE when the skew varies              (Fig. 11-B) in the paper)"
    echo "      - expansion:         measures average absolute error and memory on a growing stream                          (Fig.  8 in the paper)"
    echo "      - contraction:       measures average absolute error and memory as all keys in a stream are deleted          (Fig. 12 in the paper)"
    echo "      - accuracy_unbiased: measures unbiased average absolute error and insertion and query speed on real datasets (Fig. 13 in the paper)"
    echo "      - l2_size_function:  measures the effects of expanding based on the l2-norm of the stream for Sublime_CS     (Fig. - in the paper)"
    echo "      - join_size:         measures the accuracy of the sketches in estimating the size of a join of TPC-H tables  (Fig. - in the paper)"
    echo "By default, all datasets are generated"
    exit $1
}

if [[ "$#" -lt 2 ]]; then
    echo "Too few parameters"
    print_help_message_exit 1
fi
BUILD_PATH=$(realpath $1)
if [ ! -d "$BUILD_PATH" ]; then
    echo "Sublime build path does not exist"
    exit 1
fi
WORKLOAD_GEN_PATH=$(realpath $BUILD_PATH/bench/workload_gen)
if [ ! -f "$WORKLOAD_GEN_PATH" ]; then
    echo "Workload generator does not exist"
    exit 1
fi
REAL_DATASETS_PATH=$(realpath $2)
if [ ! -d "$REAL_DATASETS_PATH" ]; then
    echo "Real datasets path does not exist"
    exit 1
fi
shift # past memento build path
shift # past real datasets path

while [[ $# -gt 0 ]]; do
    case $1 in
        -f|--figures)
            FIGURES="$2"
            IFS="," read -ra FIGURES_ARRAY <<< "$FIGURES"
            for i in "${FIGURES_ARRAY[@]}"; do 
                if ! printf "%s\n" "${FIGURE_OPTIONS[@]}" | grep -Fxq "$i"; then
                    echo "Unknown figure $i"
                    print_help_message_exit 1
                fi
            done
            shift # past argument
            shift # past value
            ;;
        -*|--*)
            echo "Unknown option $1"
            print_help_message_exit 1
            ;;
        *)
            echo "Unknown argument $1"
            print_help_message_exit 1
            ;;
    esac
done

OUT_PATH=$(realpath ./workloads)

generate_synthetic() {
    if ! test -f zipf_0.00; then
        echo "    [++] generating zipf_0.00"
        $WORKLOAD_GEN_PATH -t standard --fdist unif -o zipf_0.00
    else 
        echo "    [--] zipf_0.00 already generated"
    fi

    exps=("0.20" "0.40" "0.80" "1.60" "3.20")
    for x in "${exps[@]}"; do
        if ! test -f zipf_${x}; then
            echo "    [++] generating zipf_${x}"
            $WORKLOAD_GEN_PATH -t standard --fdist zipf ${x} -o zipf_${x}
        else 
            echo "    [--] zipf_${x} already generated"
        fi
    done
}

generate_real() {
    if ! test -f caida; then
        echo "    [++] generating caida"
        if test -f $REAL_DATASETS_PATH/1.dat; then
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
            $WORKLOAD_GEN_PATH -t standard_string --fdist real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          -o caida
        fi
    else 
        echo "    [--] caida already generated"
    fi

    if ! test -f caida_repeat; then
        echo "    [++] generating caida_repeat"
        if test -f $REAL_DATASETS_PATH/1.dat; then
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
                                                          real $REAL_DATASETS_PATH/0.dat \
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
                                                          real $REAL_DATASETS_PATH/0.dat \
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
                                                          -o caida_repeat
        else 
            $WORKLOAD_GEN_PATH -t standard_string --fdist real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          real $REAL_DATASETS_PATH/0.dat \
                                                          -o caida_repeat
        fi
    else 
        echo "    [--] caida_repeat already generated"
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
        if test -f $REAL_DATASETS_PATH/1.dat; then
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
            $WORKLOAD_GEN_PATH -t expand --measurement-period 200000 --fdist real $REAL_DATASETS_PATH/0.dat \
                                                                             real $REAL_DATASETS_PATH/0.dat \
                                                                             real $REAL_DATASETS_PATH/0.dat \
                                                                             real $REAL_DATASETS_PATH/0.dat \
                                                                             real $REAL_DATASETS_PATH/0.dat \
                                                                             real $REAL_DATASETS_PATH/0.dat \
                                                                             real $REAL_DATASETS_PATH/0.dat \
                                                                             real $REAL_DATASETS_PATH/0.dat \
                                                                             real $REAL_DATASETS_PATH/0.dat \
                                                                             real $REAL_DATASETS_PATH/0.dat \
                                                                             real $REAL_DATASETS_PATH/0.dat \
                                                                             -o caida_expand
        fi
    else 
        echo "    [--] caida_expand already generated"
    fi
    if ! test -f webdocs_expand; then
        echo "    [++] generating webdocs_expand "
        $WORKLOAD_GEN_PATH -t expand --measurement-period 5000000 --fdist real $REAL_DATASETS_PATH/webdocs.dat --key-len-binary 0 -o webdocs_expand
    else 
        echo "    [--] webdocs_expand already generated"
    fi
}

generate_delete() {
    if ! test -f caida_delete; then
        echo "    [++] generating caida_delete"
        if test -f $REAL_DATASETS_PATH/1.dat; then
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
            $WORKLOAD_GEN_PATH -t delete --fdist real $REAL_DATASETS_PATH/0.dat \
                                                 real $REAL_DATASETS_PATH/0.dat \
                                                 real $REAL_DATASETS_PATH/0.dat \
                                                 real $REAL_DATASETS_PATH/0.dat \
                                                 real $REAL_DATASETS_PATH/0.dat \
                                                 real $REAL_DATASETS_PATH/0.dat \
                                                 real $REAL_DATASETS_PATH/0.dat \
                                                 real $REAL_DATASETS_PATH/0.dat \
                                                 real $REAL_DATASETS_PATH/0.dat \
                                                 real $REAL_DATASETS_PATH/0.dat \
                                                 real $REAL_DATASETS_PATH/0.dat \
                                                 -o caida_delete
        fi
    else 
        echo "    [--] caida_delete already generated"
    fi
}


generate_join_size() {
    if ! test -f join_size; then
        echo "    [++] generating join_size"
        $WORKLOAD_GEN_PATH -t join_size --fdist real $REAL_DATASETS_PATH/lineitem_ext.tbl \
                                                real $REAL_DATASETS_PATH/orders_ext.tbl \
                                        --max-repeat 1 4 \
                                                -o join_size
    else 
        echo "    [--] join_size already generated"
    fi
}


: '
if [[ "$FIGURES" == *"skew"* || "$FIGURES" == *"vale_tuning"* || "$FIGURES" == *"l2_size_function"* ]]; then
    echo "[!!] generate_synthetic start"
    mkdir -p $OUT_PATH/synthetic && cd $OUT_PATH/synthetic || exit 1
    if ! generate_synthetic ; then
        echo "[!!] generate_synthetic failed"
        exit 1
    fi
    echo "[!!] generate_synthetic done"
fi

if [[ "$FIGURES" == *"accuracy"* || "$FIGURES" == *"accuracy_unbiased"* ]]; then
    echo "[!!] generate_real start"
    mkdir -p $OUT_PATH/real && cd $OUT_PATH/real || exit 1
    if ! generate_real ; then
        echo "[!!] generate_real failed"
        exit 1
    fi
    echo "[!!] generate_real done"
fi

if [[ "$FIGURES" == *"expansion"* ]]; then
    echo "[!!] generate_expand start"
    mkdir -p $OUT_PATH/expand && cd $OUT_PATH/expand || exit 1
    if ! generate_expand ; then
        echo "[!!] generate_expand failed"
        exit 1
    fi
    echo "[!!] generate_expand done"
fi

if [[ "$FIGURES" == *"contraction"* ]]; then
    echo "[!!] generate_delete start"
    mkdir -p $OUT_PATH/delete && cd $OUT_PATH/delete || exit 1
    if ! generate_delete ; then
        echo "[!!] generate_delete failed"
        exit 1
    fi
    echo "[!!] generate_delete done"
fi
'

if [[ "$FIGURES" == *"join_size"* ]]; then
    echo "[!!] generate_join_size start"
    mkdir -p $OUT_PATH/real && cd $OUT_PATH/real || exit 1
    if ! generate_join_size ; then
        echo "[!!] generate_join_size failed"
        exit 1
    fi
    echo "[!!] generate_join_size done"
fi

echo "[!!] success, all workloads generated"
