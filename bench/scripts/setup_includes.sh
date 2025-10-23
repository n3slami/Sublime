#!/bin/bash

git submodule update --init ../sketches_benchmark/include/waving_sketch/
sed -i '9i #include <unistd.h>' ../sketches_benchmark/include/waving_sketch/include/Waving.h

git submodule update --init ../sketches_benchmark/include/salsa/

