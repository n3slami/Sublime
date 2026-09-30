# Sublime
Sublime is the first framework for generalizing a frequency estimation sketch
(such as the Count-Min Sketch, Count Sketch, or Misra-Gries) to improve its
accuracy and memory footprint by adapting to the workload's skew and the
stream's length. To save memory under skew, Sublime uses short counters upfront
and elongates them as they overflow with extensions stored within the same
cache line. It leverages specialized bit manipulation routines to quickly
access a counter’s extension. To maintain accurate estimates in the face of
stream growth, Sublime expands its number of counters. In doing so, it
sublinearly bounds both the error and the memory footprint and establishes a
Pareto frontier of tradeoffs to choose from.

Our paper describes Sublime in detail and provides an in-depth theoretical
analysis of its memory footprint and accuracy guarantees. It also proves a
memory footprint lower bound for sketches that adapt to the stream's length,
showing that Sublime gives rise to sketches that use the optimal amount of
space for their accuracy guarantee.

# Reproducing the Paper's Results

Everything — building, fetching the datasets, running the benchmarks, drawing
every figure and table — is one command, and there are two ways to run it.

## With Docker (recommended)

Reproducing these results natively means putting a C++ toolchain, a Python
environment and a **LaTeX installation** on your machine. The last of those is
not a small thing to add to a system you did not otherwise want to change. The
image carries all of it: the only thing you install is Docker, nothing else
touches your system, and deleting the image afterwards leaves no trace of any of
it behind.

```Bash
git clone https://github.com/---/Sublime.git
cd Sublime
docker build -t sublime_eval .
mkdir -p ../paper_results
docker run --user "$(id -u):$(id -g)" \
           -v "$(cd .. && pwd)/paper_results":/usr/local/paper_results \
           sublime_eval
```

The figures and tables come back out onto your machine, in
`paper_results/figures/<timestamp>/`, owned by you rather than by root.

Sublime is deliberately compiled when the container *runs*, not when the image
is built, so that `-march=native` sees your CPU. The container measures the
machine it runs on, not the one the image was built on.

## Natively

If you would rather not use Docker, or already have the dependencies:

```Bash
git clone https://github.com/---/Sublime.git
cd Sublime
./evaluate.sh
```

This is the same script the container runs, so the two paths do identical work.
It checks that everything it needs is installed before it starts and names
anything missing; "Reproducibility in Detail" below lists the dependencies.

## Options

Both forms take the same options, and put their output in the same place — a
`paper_results` directory **next to** the clone, with the figures and tables
numbered as in the paper.

| | |
|---|---|
| `--quick` | A few minutes instead of hours, to check the setup works before committing a machine. One dataset, two memory budgets. The numbers are *not* the paper's. |
| `--with-mg` | Also run the Sublime_MG figures of the journal extension (Fig. 17-19). Roughly triples the time. |
| `-f accuracy,skew` | Run only the named figures. `./evaluate.sh --help` lists them. |
| `--skip-tests` | Skip the unit tests, which otherwise run first. |

A full run needs about **12 GB of free disk** and **16 GB of RAM**; `--quick`
needs about 3 GB of disk.

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
  to the sketch.

The file `example.cpp` in the `examples` directory illustrates the usage of
these APIs. All other operations such as auto-tuning VALE are transparently
handled by Sublime and therefore do not have APIs.

# Reproducibility in Detail

`evaluate.sh` compiles every version of Sublime and the baselines, runs the unit
tests, downloads the datasets, generates the workloads, runs the benchmarks and
draws the figures and tables. The Dockerfile installs the dependencies and then
runs that same script, which is why the two paths produce the same work.

**Hardware.** Any x86-64 machine. Sublime uses BMI2 for rank and select and
AVX-512 for Sublime_CS's l2-norm estimator where they are available, and falls
back to portable code where they are not, so the results reproduce on a machine
without them — though the timings will not be comparable to the paper's, which
were measured on a machine with both. Because the container compiles Sublime at
run time rather than at image build time, it picks up whichever of these your
CPU provides.

**Dependencies, for a native run.** `evaluate.sh` checks for all of these before
it starts and names any that are missing:

- CMake 3.5 or later, GNU Make, and GCC 11 or later
- Git, wget, curl, and Bash 4.4 or later
- Python 3.8 or later, with the `venv` module (`python3-venv` on Debian and
  Ubuntu). `evaluate.sh` creates a virtualenv and installs matplotlib into it,
  unless matplotlib is already importable.
- A LaTeX installation, used to typeset the figures' text. `texlive-latex-base`,
  `texlive-latex-recommended` and `texlive-fonts-recommended` are enough on
  Debian and Ubuntu — a full TeX Live is not needed.

**The datasets** are downloaded to `paper_results/real_datasets` and checked
against recorded SHA-256 checksums, so a truncated or replaced download is
caught rather than quietly producing different numbers. They are fetched once
and reused by later runs.

*Note on CAIDA*: we exclude the full CAIDA 2018 dataset used in our evaluation
and only use a 10% slice of it, because the full dataset requires approval for
use in research projects. We repeat this slice 10 times to create a workload of
the same size as when using the full dataset. It is still possible to reproduce
the exact results in our paper by adding the remaining 90%, i.e. the files
`1.dat` to `10.dat`, to the `paper_results/real_datasets` directory before
running `evaluate.sh`, in either the dockerized or the native version.

## Sublime_MG, the journal extension

The three Sublime_MG figures (17, 18 and 19) belong to the journal extension
rather than to the conference paper, and are **off by default**. Add `--with-mg`
to run them as well, or name them individually with `-f`. They roughly triple
the running time.

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
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
make -j8
```
`CMAKE_POLICY_VERSION_MINIMUM` is needed only with CMake 4 or later, and is
harmless before it: the unit tests fetch doctest, which still declares
`cmake_minimum_required(VERSION 3.0)`, and CMake 4 refuses that outright. Drop
it if you are also passing `-DBUILD_TESTS=0`.
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
