#!/bin/bash

git submodule update --init ../sketches_benchmark/include/bitsense/
cd ../sketches_benchmark/include/bitsense/
git submodule update --init simulator/third_party/eigen
git submodule update --init simulator/third_party/fmt
git submodule update --init simulator/third_party/toml
cd -

