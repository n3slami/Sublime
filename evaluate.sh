#!/bin/bash

FIGURE_OPTIONS=("accuracy" "skew" "vale_tuning" "expansion" "contraction" "accuracy_unbiased" "l2_size_function" "join_size")

FIGURES="accuracy,skew,vale_tuning,expansion,contraction,accuracy_unbiased,l2_size_function,join_size"

function print_help_message_exit() {
    echo "Usage: evaluate.sh [-f|--figures ${FIGURES}]"
    echo "The figures parameter is a list of comma-separated names describing what experiments to run:"
    echo "      - accuracy:          measures average absolute error and insertion and query speed on real datasets          (Fig. 10 in the paper)"
    echo "      - skew:              measures average absolute error over synthetic Zipfian datasets with varying skew       (Fig. 11-A) in the paper)"
    echo "      - vale_tuning:       measures the memory savings of adaptively tuning VALE when the skew varies              (Fig. 11-B) in the paper)"
    echo "      - expansion:         measures average absolute error and memory on a growing stream                          (Fig.  8 in the paper)"
    echo "      - contraction:       measures average absolute error and memory as all keys in a stream are deleted          (Fig. 12 in the paper)"
    echo "      - accuracy_unbiased: measures unbiased average absolute error and insertion and query speed on real datasets (Fig. 13 in the paper)"
    echo "      - l2_size_function:  measures the effects of expanding based on the l2-norm of the stream for Sublime_CS     (Fig. - in the paper)"
    echo "      - join_size:         measures the accuracy of the sketches in estimating the size of a join of TPC-H tables  (Fig. - in the paper)"
    echo "By default, all figures are generated"
    exit $1
}

while [[ $# -gt 0 ]]; do
    case $1 in
        -f|--figures)
            FIGURES="$2"
            IFS="," read -ra FIGURES_ARRAY <<< "$FIGURES"
            for i in "${FIGURES_ARRAY[@]}"; do 
                if ! printf "%s\n" "${FIGURE_OPTIONS[@]}" | grep -Fxq "$i"; then
                    echo "Unknown figure $i"
                    print_help_message_exit 1;
                fi
            done
            shift # past argument
            shift # past value
            ;;
        -h|--help)
            print_help_message_exit 0;
            ;;
        -*|--*)
            echo "Unknown option $1"
            print_help_message_exit 1;
            ;;
        *)
            echo "Unknown argument $1"
            print_help_message_exit 1;
            ;;
    esac
done

project_root=$(pwd)

mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j8
if [[ $? -ne 0 ]]; then
    echo "Compilation of default suite failed"
    exit 1
fi

cd ../.. && mkdir -p paper_results && cd paper_results
mkdir -p figures && touch figures/tmp.txt
if [ ! -d ".venv" ]; then
    python3 -m venv .venv
fi
bash ${project_root}/bench/scripts/download_datasets.sh
bash ${project_root}/bench/scripts/generate_datasets.sh ${project_root}/build real_datasets -f ${FIGURES}
source .venv/bin/activate
python3 ${project_root}/bench/scripts/run_benchmarks.py ${project_root}/build workloads -b ${FIGURES//,/ }

cd ${project_root} 
cd ../paper_results/
if ! python3 -c "import matplotlib"; then
    .venv/bin/pip install matplotlib
fi
python3 ${project_root}/bench/scripts/plot.py -f ${FIGURES//,/ }
deactivate

