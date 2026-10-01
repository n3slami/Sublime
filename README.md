# Sublime
Sublime is the first framework for generalizing a frequency estimation sketch (such as the Count-Min Sketch, Count Sketch, or Misra-Gries) to improve its accuracy and memory footprint by adapting to the workload's skew and the stream's length.
To save memory under skew, Sublime uses short counters upfront and elongates them as they overflow with extensions stored within the same cache line.
It leverages specialized bit manipulation routines to quickly access a counter's extension.
To maintain accurate estimates in the face of stream growth, Sublime expands its number of counters.
In doing so, it sublinearly bounds both the error and the memory footprint and establishes a Pareto frontier of trade-offs to choose from.

Our paper describes Sublime in detail and provides an in-depth theoretical analysis of its memory footprint and accuracy guarantees.
It also proves a memory footprint lower bound for sketches that adapt to the stream's length, showing that Sublime gives rise to sketches that use the optimal amount of space for their accuracy guarantee.

# Reproducing the Paper's Results

Building the project, fetching datasets, running benchmarks, and generating all figures and tables can be done with a single command.

There are two ways to run this command:
1. using Docker, which avoids polluting the host machine with dependencies, or
2. natively, which is useful when all required dependencies are already installed.

Both methods place their output in a `paper_results` directory **next to** the clone of Sublime's repository. 
They also support the same runtime options.
The most important option is `--quick`, which runs the benchmarks on smaller datasets and generates example figures for sanity checking before running the full benchmark suite.
All runtime options are described below.

## With Docker (Recommended)

Reproducing the paper's results natively requires a C++ toolchain, a Python environment, and a LaTeX installation.
To avoid installing these dependencies on the host machine, we provide a Dockerfile that builds an image with all required dependencies:
```Bash
git clone https://github.com/---/Sublime.git
cd Sublime
docker build -t sublime_eval .
mkdir -p ../paper_results
docker run --user "$(id -u):$(id -g)" \
           -v "$(cd .. && pwd)/paper_results":/usr/local/paper_results \
           sublime_eval
```

The generated figures and tables are saved on the host machine under `paper_results/figures/<timestamp>/` and numbered to match their corresponding figures and tables in the paper.

Sublime is intentionally compiled when the container runs rather than when the image is built, allowing `-march=native` to optimize for the host CPU.

## Natively

If the dependencies are already installed on the host machine, the paper can be evaluated natively by running the same script as the Docker container:
```Bash
git clone https://github.com/---/Sublime.git
cd Sublime
./evaluate.sh
```

This script checks that all dependencies are installed before it starts and names any missing ones.
We present the exact list of dependencies under "Reproducibility in Detail" below.

## Runtime Options

Both methods above support the same runtime options.

| | |
|---|---|
| `--quick` | Runs a reduced benchmark suite in a few minutes rather than hours, using only one dataset and two memory budgets. This option is intended for sanity checking the benchmarking setup; the resulting figures are *not* representative of the paper's results. |
| `--with-mg` | Also generates the Sublime_MG figures, which appear only in the journal version of the paper (Figs. 17--19). This option roughly triples the benchmarking time. |
| `-f accuracy,skew` | Runs only the benchmarks needed to generate the specified figures. The command `./evaluate.sh --help` lists the figure names. |
| `--skip-tests` | Skips the unit tests, which otherwise run before the benchmarks. |

A full run requires approximately **12 GB of free disk space** and **16 GB of RAM**. Using `--quick` reduces the required disk space to approximately **3 GB**.

Reproducing the eight figures from the conference paper takes a little over two hours of benchmarking, in addition to the one-time cost of downloading the datasets and generating the workloads on the first run. Using `--with-mg` additionally generates the three figures from the journal extension.

# Getting Started
To use Sublime in your own project, simply add the files in the `include` directory and include the header file corresponding to the desired version of Sublime.

The file `SublimeCMS.hpp` implements Sublime when applied to the Count-Min Sketch, complete with VALE's auto-tuning features.
The file `SublimeCMSNoTuning.hpp.in` fixes VALE's tuning and allows the compiler to apply intrusive optimizations to significantly improve insertion, deletion, and query performance.
Moreover, `SublimeCMSNoTuningMorris.hpp.in` probabilistically increments each counter during insertions to enable a fair comparison with Tailored Sketch.

Similarly, the file `SublimeCS.hpp` implements Sublime applied to the Count Sketch, and the file `SublimeCSNoTuning.hpp.in` fixes VALE's tuning in this case to enable higher performance.

All versions of Sublime provide the following APIs:
- `Insert`: Inserts the provided key into the sketch.
- `Delete`: Deletes the provided key from the sketch.
- `Query`: Returns an estimate of the provided key's frequency.
- `Size`: Returns the size of the sketch.
- `FlushPrefetchQueue`: Flushes all updates whose requests for chunks are
  in-flight. This ensures queries take into account all updates previously made
  to the sketch.

The file `example.cpp` in the `examples` directory illustrates the usage of
these APIs. All other operations such as auto-tuning VALE are transparently
handled by Sublime and therefore do not have APIs.

# Reproducibility in Detail

The `evaluate.sh` script compiles every version of Sublime and the baselines, runs the unit tests, downloads the datasets, generates the workloads, runs the benchmarks and draws the figures and tables.
The Dockerfile installs the dependencies and then runs that same script.

**Hardware.** 
Any x86-64 machine.
We use Intel's BMI2 instruction for implementing rank and select, as well as AVX-512 for implementing Sublime_CS's l2-norm estimator where they are available.
We have also included fall-backs to portable code where these instructions are unavailable.
Using these fall-backs may result in timings that are not comparable to the paper's, since our machine supported both BMI2 and AVX-512.
Because the container compiles Sublime at run time rather than at image build time, it picks up whichever of these the machine's CPU supports.

**Dependencies for a Native Run.**
The `evaluate.sh` script checks for the following dependencies before it starts and names any that are missing:
- CMake 3.5 or later, GNU Make, and GCC 11 or later
- Git, wget, curl, and Bash 4.4 or later
- Python 3.8 or later, with the `venv` module (`python3-venv` on Debian and Ubuntu).
  The `evaluate.sh` script creates a virtual environment and installs matplotlib onto it, unless matplotlib is already importable.
- A LaTeX installation, used to typeset the figures' text.
  The packages `texlive-latex-base`, `texlive-latex-recommended` and `texlive-fonts-recommended` are sufficient on Debian and Ubuntu.

**Datasets.**
The datasets are downloaded to `paper_results/real_datasets` and checked against recorded SHA-256 checksums.
They are fetched once and reused by later runs.

*Note on CAIDA*: 
We exclude the full CAIDA 2018 dataset used in our evaluation and only use a 10% slice of it, because the full dataset requires approval for use in research projects.
We repeat this slice 10 times to create a workload of the same size as when using the full dataset.
It is still possible to reproduce the exact results in our paper by adding the remaining 90%, i.e. the files `1.dat` to `10.dat`, to the `paper_results/real_datasets` directory before running `evaluate.sh`, in either the Dockerized or the native version.

## Sublime_MG, the Journal Extension

The three Sublime_MG figures (17, 18 and 19) belong to the journal extension rather than to the conference paper, and are **not generated by default**.
Adding `--with-mg` generates them as well, which roughly triples the running time of the benchmarks.

# Building
To build the different versions of Sublime along with their examples, unit tests, and benchmarks, navigate to the project's root directory and execute the commands
```Bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
make -j8
```
`CMAKE_POLICY_VERSION_MINIMUM` is needed only with CMake 4 or later, and has no effect when used with older CMake versions.
The parts being configured and compiled can be controlled with the following CMake options:
- `BUILD_TESTS`: Builds the tests.
- `BUILD_EXAMPLES`: Builds the examples.
- `BUILD_BENCHMARKS`: Builds the benchmark suite while cloning the repositories of the baselines.

All three options are set by default. To turn one off, e.g., `BUILD_BENCHMARKS`, substitute the second line in the build script above with:
```Bash
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCHMARKS=0
```

In addition to the above options, Sublime's tuning of VALE can be fixed by specifying the following CMake options before compiling the code:
- `FIXED_TUNING_C={c}`: Fixes the number of counters per chunk to `c`.
- `FIXED_TUNING_S={s}`: Fixes the stub length to `s`.

These parameters impact the files `SublimeCMSNoTuning.hpp.in`, `SublimeCMSNoTuningMorris.hpp.in`, and `SublimeCSNoTuning.hpp.in`.
They do not change the tuning of the core versions of Sublime, i.e., those in `SublimeCMS.hpp` and `SublimeCS.hpp`.

For `SublimeCMSNoTuningMorris.hpp.in`, the increment probability of the counters can be set using the following CMake option:
- `LOG_REC_INC_PROB={p}`: Set the increment probability to $2^{-p}$.

# Running Unit Tests
Running the following command from the project's root after building Sublime will execute its unit tests:
```Bash
ctest -VV
```

# Citation
Please use the following BibTeX entry to cite our work in your own publications:
```{bibtex}
@article{10.1145/3802116,
author = {Eslami, Navid and Bercea, Ioana and Pagh, Rasmus and Dayan, Niv},
title = {Sublime: Sublinear Error \& Space for Unbounded Skewed Streams},
year = {2026},
issue_date = {June 2026},
publisher = {Association for Computing Machinery},
address = {New York, NY, USA},
volume = {4},
number = {3},
url = {https://doi.org/10.1145/3802116},
doi = {10.1145/3802116},
abstract = {Modern stream processing systems often need to track the frequency of distinct keys in a data stream in real-time. Since maintaining exact counts can require a prohibitive amount of memory, many applications rely on compact, probabilistic data structures known as frequency estimation sketches to approximate them. However, mainstream frequency estimation sketches fall short in two critical aspects. First, they are memory-inefficient under skewed workloads because they use uniformly-sized counters to count the keys, thus wasting memory on storing the leading zeros of many small counts. Second, their estimation error deteriorates at least linearly with the length of the stream --- which may grow indefinitely --- because they rely on a fixed number of counters. We present Sublime, a framework that generalizes frequency estimation sketches to address these challenges. To reduce memory footprint under skew, Sublime begins with short counters and dynamically elongates them as they overflow, storing their extensions within the same cache line. It employs efficient bit manipulation routines to quickly locate and access a counter's extensions. To maintain accuracy as the stream grows, Sublime also expands its number of counters at a configurable rate, exposing a new spectrum of accuracy-memory tradeoffs that applications can tune to their needs. We apply Sublime to both Count-Min Sketch and Count Sketch. Through theoretical analysis and empirical evaluation, we show that Sublime significantly improves accuracy and memory over the state of the art while maintaining competitive or superior performance.},
journal = {Proc. ACM Manag. Data},
month = may,
articleno = {239},
numpages = {29},
keywords = {frequency estimation sketch, data growth, scalability}
}
```
