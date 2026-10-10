# Sublime
Sublime is the first framework for generalizing a frequency estimation sketch (such as the Count-Min Sketch, Count Sketch, or Misra-Gries) to improve its accuracy and memory footprint by adapting to the workload's skew and the stream's length.
To save memory under skew, Sublime uses short counters upfront and elongates them as they overflow with extensions stored within the same cache line.
It leverages specialized bit manipulation routines to quickly access a counter's extension.
To maintain accurate estimates in the face of stream growth, Sublime expands its number of counters.
In doing so, it sublinearly bounds both the error and the memory footprint and establishes a Pareto frontier of trade-offs to choose from.

Our paper describes Sublime in detail and provides an in-depth theoretical analysis of its memory footprint and accuracy guarantees.
It also proves a memory footprint lower bound for sketches that adapt to the stream's length, showing that Sublime gives rise to sketches that use close to the optimal amount of space for their accuracy guarantee.

# Reproducing the Paper's Results

Building the project, downloading datasets, running benchmarks, and generating all figures and tables can be done with a single command.
With the `--with-paper` option, this command also produces a new PDF of the paper containing the regenerated figures.

There are two ways to run this command:
1. **Using Docker**, which avoids installing dependencies on the host machine.
2. **Natively**, which is useful when all required dependencies are already installed.

Both methods support x86 and ARM architectures and can be used on Linux, macOS, and Windows.
They save their output to a `paper_results` directory **next to** the cloned Sublime repository and support the same runtime options.

The most important runtime option is `--quick`, which runs a reduced set of benchmarks on smaller datasets and generates example figures to verify that the setup works before running the full benchmark suite.
All runtime options are described below.

## With Docker (Recommended)

Reproducing the paper's results natively requires a C++ toolchain, a Python environment, and a LaTeX installation.
To avoid installing these dependencies on the host machine, we provide a Dockerfile that builds an image containing all required dependencies:

```bash
git clone https://github.com/---/Sublime.git
cd Sublime
docker build -t sublime_eval .
mkdir -p ../paper_results
docker run --user "$(id -u):$(id -g)" \
           -v "$(cd .. && pwd)/paper_results":/usr/local/paper_results \
           sublime_eval
```

The generated figures and tables are saved on the host machine under `paper_results/figures/<timestamp>/` and numbered to match their counterparts in the paper.

To also generate a new PDF of the paper containing the regenerated figures, build the image with the additional LaTeX packages required by the paper (which increase the image size by approximately 2.2 GB) and pass `--with-paper` when running the container:

```bash
docker build --build-arg WITH_PAPER=1 -t sublime_eval .
docker run --user "$(id -u):$(id -g)" \
           -v "$(cd .. && pwd)/paper_results":/usr/local/paper_results \
           sublime_eval --with-paper
```

Sublime is intentionally compiled when the container runs rather than when the image is built, allowing `-march=native` to optimize the executable for the host CPU.
The Docker image is built for the host's architecture, so the same commands work on both x86-64 and ARM64 machines, including Apple Silicon Macs.
On Windows, these commands can be run under WSL2 with Docker Desktop.

*Note that Sublime relies on efficient implementations of rank and select using Intel's BMI2 instructions.
Consequently, performance measurements on platforms without these instructions may differ from those reported in the paper.*

## Natively

If the required dependencies are already installed on the host machine, the paper's results can be reproduced natively by running the same script used by the Docker container:

```bash
git clone https://github.com/---/Sublime.git
cd Sublime
./evaluate.sh
```

Before starting, the script checks for all required dependencies and reports any that are missing.
The complete list of dependencies is provided in the "Reproducibility in Detail" section below.

## Runtime Options

Both methods above support the same runtime options.

| | |
|---|---|
| `--quick` | Runs a reduced benchmark suite in a few minutes rather than hours, using only one dataset and two memory budgets. This option is intended for sanity checking the benchmarking setup; the resulting figures are *not* representative of the paper's results. |
| `--with-mg` | Also generates the Sublime_MG figures, which appear only in the journal version of the paper (Figs. 17--19). This option roughly triples the benchmarking time. |
| `-f accuracy,skew` | Runs only the benchmarks needed to generate the specified figures. The command `./evaluate.sh --help` lists the figure names. |
| `--skip-tests` | Skips the unit tests, which otherwise run before the benchmarks. |
| `--with-paper` | Also produces a new PDF of the paper containing the regenerated figures, saved as `paper.pdf` next to them. This option is off by default and requires additional LaTeX packages, listed under "Reproducibility in Detail" below. |

A full run requires approximately **12 GB of free disk space** and **16 GB of RAM**. Using `--quick` reduces the required disk space to approximately **3 GB**.

Reproducing the eight figures from the conference paper takes a little over two hours of benchmarking, in addition to the one-time cost of downloading the datasets and generating the workloads on the first run. Using `--with-mg` additionally generates the three figures from the journal extension.

# Platform Support

**Sublime.**
The headers in `include` compile with GCC and Clang, including Apple Clang, on Linux and macOS, on both x86-64 and ARM64 machines such as Apple Silicon Macs.
They also compile for Windows with MinGW-w64.
Code paths for MSVC are included as well, but we have not tested them.
Sublime uses BMI2 for rank and select, LZCNT and BMI for bit scans, and AVX-512 for Sublime_CS's l2-norm estimator where the CPU supports them, and falls back to portable code everywhere else.
These fallbacks may slightly reduce performance.

**The benchmark suite.**
The `evaluate.sh` script and the scripts it calls run on Linux and macOS, on both x86-64 and ARM64 machines.
On Windows, they run under Docker or the Windows Subsystem for Linux.

**Baselines.**
Every baseline we compare against builds on every platform above, except SALSA, whose implementation uses the x86 timestamp counter and AVX2 instructions.
SALSA is therefore built only on x86-64 machines that support AVX2.
Elsewhere, the build skips it with a message, the benchmarks skip it, and the figures are drawn without it.
Every version of Sublime runs on every platform.
On machines other than the one we used, which supports BMI2 and AVX-512, the timings are not comparable to the paper's.

# Reproducibility in Detail

The `evaluate.sh` script compiles every version of Sublime and the baselines, runs the unit tests, downloads the datasets, generates the workloads, runs the benchmarks and draws the figures and tables.
The Dockerfile installs the dependencies and then runs that same script.

## Hardware

Any 64-bit x86 or ARM machine running Linux or macOS, or Windows through Docker or the Windows Subsystem for Linux.
"Platform Support" above lists which baselines each platform runs.
Our results were produced on an x86-64 machine supporting BMI2 and AVX-512; Sublime falls back to portable code where these are unavailable, at the cost of timings that are not comparable to the paper's.
Because the container compiles Sublime at run time rather than at image build time, it picks up whichever of these the machine's CPU supports.

## Dependencies for a Native Run

The `evaluate.sh` script checks for the following dependencies before it starts and names any that are missing:
- CMake 3.5 or later, Make, and GCC 11 or later or Clang (on macOS, Apple Clang from the Xcode command-line tools)
- Git, curl, and Bash 3.2 or later, which is the version macOS ships
- Python 3.8 or later, with the `venv` module (`python3-venv` on Debian and Ubuntu).
  The `evaluate.sh` script creates a virtual environment and installs matplotlib onto it, unless matplotlib is already importable.
- A LaTeX installation, used to typeset the figures' text.
  The packages `texlive-latex-base`, `texlive-latex-recommended` and `texlive-fonts-recommended` are sufficient on Debian and Ubuntu.

## Regenerating the Paper

With `--with-paper`, the paper's source is fetched from the `camera_ready` branch of [its repository](https://github.com/n3slami/Sketchbook_Draft/tree/camera_ready), each figure of the paper that the run generated replaces the published one, and the paper is compiled into `paper_results/figures/<timestamp>/paper.pdf`.
A figure that the run did not generate, e.g., when using `-f`, keeps its published version, and the script lists which ones.
The figures of the Sublime_MG journal extension are not part of this paper.
The paper's repository includes the ACM document class it uses, so it compiles with TeX Live 2022 or later, given a few additional packages, all of which `evaluate.sh` checks for before it starts:
- On Debian and Ubuntu: `texlive-fonts-extra`, `texlive-plain-generic`, and `texlive-latex-extra`.
- On macOS: the full MacTeX distribution, as installed below.
- With Docker: `--build-arg WITH_PAPER=1`, as shown above.

On macOS, the compiler comes with the Xcode command-line tools (`xcode-select --install`), and the rest can be installed with [Homebrew](https://brew.sh):
```Bash
brew install cmake python git curl
brew install --cask mactex-no-gui    # The full TeX distribution, about 5 GB.
```

## Datasets

### Default Datasets

The datasets used in the paper are downloaded to `paper_results/real_datasets` and verified against recorded SHA-256 checksums.
They are downloaded only once and reused in subsequent runs.

**Note on CAIDA:**
We cannot distribute the full CAIDA 2018 dataset used in our evaluation because its use requires explicit approval from CAIDA.
Instead, we include a 10% subset and repeat it 10 times to construct a workload of the same size as the original.
The exact results reported in the paper can still be reproduced by adding the remaining data (files `1.dat` through `10.dat`) to `paper_results/real_datasets` before running `evaluate.sh`, using either Docker or the native setup.

### Custom Datasets

Any stream stored as a text file can be used to benchmark the sketches in our suite.
The workload generator converts the file into a workload that can be replayed by each sketch's benchmark.
Each benchmark reports the sketch's estimation error relative to the exact frequencies, memory consumption, and update and query times in JSON format.

From the `build` directory, run:

```bash
./bench/workload_gen -t standard --fdist real my_stream.txt --key-len-binary 0 -o my_stream
./bench/bench_SublimeCMS 1048576 -w my_stream     # 1 MB memory budget
./bench/bench_CMS 1048576 -w my_stream
```

By default, each whitespace-separated token in the input file is treated as a key.
Alternatively, `--text-keys lines` treats each non-empty line as a key, which is useful for keys containing spaces, such as flow identifiers.
Keys are interpreted as unsigned 64-bit integers if they contain only a decimal integer without a sign or leading zeros.
Otherwise, they are preserved as their original byte sequences.

The `-t` option selects the workload type, all of which support text files:

- `standard`: Inserts the entire stream, then measures the sketch once.
- `standard_string`: Same as `standard`, but always treats keys as strings.
- `expand`: Inserts the stream in periods of `--measurement-period` keys, measuring after each period. This is useful for sketches that expand as the stream grows, such as `bench_SublimeCMS` with `--size-function-power` and `--size-function-mult`. Keys beyond the last complete period are not replayed.
- `delete`: Inserts the entire stream, then deletes up to `--n-deletes` keys in periods of `--measurement-period`, measuring after each period. This workload is intended for sketches that support deletions.

Multiple files can be combined into a single stream using `--fdist real a.txt real b.txt`.
For binary streams containing fixed-length keys, set `--key-len-binary` to the key length in bytes.

By default, the workload generator shuffles the stream using the seed specified by `--seed`, as was done for all workloads in the paper.
To preserve the original stream order, for example, to study the effects of temporal locality, use `--no-shuffle`:

```bash
./bench/workload_gen -t expand --fdist real my_stream.txt --key-len-binary 0 --measurement-period 100000 --no-shuffle -o my_stream_ordered
```

With `--no-shuffle`, multiple input files are replayed sequentially in the order specified.
For `-t delete`, keys are deleted in reverse stream order.

## Sublime_MG, the Journal Extension

The three Sublime_MG figures (17, 18 and 19) belong to the journal extension rather than to the conference paper, and are **not generated by default**.
Adding `--with-mg` generates them as well, which roughly triples the running time of the benchmarks.

# Developing with Sublime
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

# Building Sublime
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
