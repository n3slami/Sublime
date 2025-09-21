#!/bin/bash

git submodule update --init ../sketches_benchmark/include/bitsense/
cd ../sketches_benchmark/include/bitsense/
git submodule update --init simulator/third_party/eigen
git submodule update --init simulator/third_party/fmt
git submodule update --init simulator/third_party/toml
sed -i '12i #include <cstdint>' ./simulator/src/common/hash.h
sed -i '12i #include <time.h>' ./simulator/src/common/hash.h
cd -

git submodule update --init ../sketches_benchmark/include/sead_counter/

git submodule update --init ../sketches_benchmark/include/waving_sketch/
sed -i '9i #include <unistd.h>' ../sketches_benchmark/include/waving_sketch/include/Waving.h

