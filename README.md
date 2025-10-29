# Sublime
Sublime is the first framework for generalizing a frequency estimation sketch
(such as the Count-Min Sketch, Count Sketch, or Misra-Gries) to improve their
accuracy and memory footprint by adapting to the workload's skew and the
stream's length. To save memory under skew, Sublime uses short counters upfront
and elongates them as they overflow with extensions stored within the same
cache line. It leverages specialized bit manipulation routines to quickly
access a counter’s extension. To maintain accurate estimates in the face of
stream growth, Sublime expands its number of counters. In doing so, it
sublinearly bounds both the error and the memory footprint and establishes a
Pareto frontier of tradeoffs to choose from.

Our paper describes Sublime in detail and provides an in-depth theoretical
analysis for its memory footprint and accuracy guarantees. It also proves a
memory footprint lower bound for sketches that adapt to the stream's length,
showing that Sublime gives rise to sketches that use the optimal amount of
space for their accuracy guarantee.

# Getting Started
To use Sublime in developing your own project, simply add the files in the
`include` directory and include the header file corresponding to the desired
version of Sublime. 

The file `SublimeCMS.hpp` implements Sublime when applied to the Count-Min
Sketch, complete with VALE's auto-tuning features. The file
`SublimeCMSNoTuning.hpp.in` fixes VALE's tuning and allows the compiler to
apply intrusive optimizations to significantly improve insertion, deletion, and
query performance. Moreover, `SublimeCMSNoTuningMorris.hpp.in`
probabilistically increments each counter during insertions to enable a fair
comparison with Tailored Sketch.

Similarly, the file `SublimeCS.hpp` implements Sublime applied to the Count
Sketch, and the file `SublimeCSNoTuning.hpp.in` fixes VALE's tuning in this
case to enable higher performance.

All versions of Sublime provide the following APIs:
- `Insert`: Inserts the provided key into the sketch.
- `Delete`: Deletes the provided key from the sketch.
- `Query`: Returns an estimate of the provided key's frequency.
- `Size`: Returns the size of the sketch.
- `FlushPrefetchQueue`: Flushes all updates whose requests for chunks are
  in-flight. This ensures queries take into account all updates previously made
  to the filter.
The file `example.cpp` in the `examples` directory illustrates the usage of
these APIs. All other operations such as auto-tuning VALE are transparently
handled by Sublime and therefore do not have APIs.

# Quick Reprodicibility 
This repository contains an `evaluate.sh` script. This script downloads the
relevant datasets, compiles the different versions of Sublime, sets the
benchmarks up, runs them, and produces the figures and tables. To ease the
reproducibility process, we provide a Dockerfile that spins up an Ubuntu Docker
container with the requirements installed and runs `evaluate.sh` to gather the
results. One may choose to run the experiments in Docker, or to run the
experiments natively. Both methods take ~1 hour to complete and we provide
instructions on how to do either. We advise using a machine with at least 16GB
of RAM.

*Note*: We exclude the full CAIDA 2018 dataset used in our evaluation and only
use a 10% slide of the dataset. This exclusion is due to the CAIDA dataset
requiring approval for use in research projects. We repeat this slice of the
dataset 10 times to scale the create a workload of the same size as when using
the full CAIDA dataset. Nevertheless, it is still possible to reproduce the
exact results in our paper by adding the remaining 90% of the dataset, i.e.,
the files 1.dat to 10.dat, to the `paper_results/real_datasets` directory
before running the `evalute.sh` script in either the dockerized version or the
native version of our benchmarks.

## Executing with Docker
Running the following commands clones Sublime and evaluates it:
```Bash
git clone https://github.com/---/Sublime.git
cd Sublime
docker build -t sublime_eval .
mkdir -p ../paper_results/figures/ && docker run -v ../paper_results/figures:/usr/local/paper_results/figures -it sublime_eval
```
The above creates a **directory next to the cloned repository** named
`paper_results` and puts the generated figures and tables in the
`paper_results/figures` subdirectory within. The figures and tables will will
be numbered to match the paper.

## Executing Natively
To use the `evaluate.sh` script, ensure that the following dependencies are
satisfied:

- Make >=3.30.8 (Used to compile the baselines)
- python3-env >=3.13.5 (Used to plot the experimental results only)
- Python 3 >=3.7 (Used to plot the experimental results only)
- Latex full (Used to plot the experimental results only) 

Then, running
```Bash
git clone https://github.com/---/Sublime.git
cd Sublime
bash evaluate.sh
```
reproduces the results in the paper, creating a `paper_results` directory
**next to the cloned repository** in the process. It places the generated
figures and tables in the `paper_results/figures` subdirectory, numbering them
to match the paper. 

# Building
The following lists the dependencies required for building Sublime and its
evaluation suite:
- cmake 3.5 (or later)
- gcc-11 (or later)
- Git 2.13 (or later)
- Python 3.8 (or later)
- Bash 4.4 (or later)
  - realpath 8.28 (or later)
  - wget 1.19.4 (or later)

To build the different versions of Sublime along with their examples, unit
tests, and benchmarks, navigate to the project's root directory and execute the
commands
```Bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j8
```
You can control which parts are configured and compiled with the following
CMake options:
- `BUILD_TESTS`: Builds the tests.
- `BUILD_EXAMPLES`: Builds the examples.
- `BUILD_BENCHMARKS`: Builds the benchmark suite while cloning the repositories
  of the baselines.
All these options are set by default. To turn one off, for example,
`BUILD_BENCHMARKS`, substitute the second line in the build script above with:
```Bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCHMARKS=0
```

In addition to the above options, you can fix the tuning of Sublime's (i.e.,
VALE's) parameters by specifying the following CMake options before compiling
the code:
- `FIXED_TUNING_C={c}`: Fixes the number of counters per chunk to `c`.
- `FIXED_TUNING_S={s}`: Fixes the stub length to `s`.
These parameters impact the files `SublimeCMSNoTuning.hpp.in`,
`SublimeCMSNoTuningMorris.hpp.in`, and `SublimeCSNoTuning.hpp.in`. They do not
change the tuning of the core versions of Sublime, i.e., those in
`SublimeCMS.hpp` and `SublimeCS.hpp`.

For `SublimeCMSNoTuningMorris.hpp.in`, you can also set the increment
probability of the counters in Sublime by supplying the following CMake option:
- `LOG_REC_INC_PROB={p}`: Set the increment probability to $2^{-p}$.

# Running Unit Tests
After building Sublime, run the following command from the project's root
directory to execute the unit tests:
```Bash 
ctest -VV
```

# Citation
Please use the following BibTeX entry to cite our work in your own
publications:
```{bibtex}
@article{}
```
