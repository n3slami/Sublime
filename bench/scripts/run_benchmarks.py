import argparse, shutil, itertools, subprocess, sys
from pathlib import Path
from datetime import datetime

global build_dir
global workload_dir
global benchmarks_dir
global output_prefix

SKETCHS_WITH_EXPANSION_RATE_FUNCTION = {"CMSketchbookFixedCounters",
                                        "CMSketchbookAdaptiveCountersPQ",
                                        "CMSketchbookAdaptiveCountersPQNoTuning",
                                        "CSketchbookFixedCounters",
                                        "CSketchbookAdaptiveCountersPQ",
                                        "CSketchbookAdaptiveCountersPQNoTuning"}

def execute_benchmark(build_dir, output_base, workload_subdir, workload, filter, bpk, size_function_power=None, size_function_mult=None):
    file_to_execute = f"bench/bench_{filter}"
    size_function_power_option = f"--size-function-power {size_function_power}" if size_function_power != None else ""
    size_function_mult_option = f"--size-function-mult {size_function_mult}" if size_function_mult != None else ""
    command = f"{build_dir}/{file_to_execute} {bpk} -w {workload} {size_function_power_option} {size_function_mult_option} | tee {output_base}/{filter}_{bpk}_{workload.name}.json"
    cli_message_command = f"<build_dir>/{file_to_execute} {bpk} -w <workload_dir>/{workload_subdir}/{workload.name} {size_function_power_option} {size_function_mult_option} | tee <output_dir>/{workload_subdir}/{filter}_{bpk}_{workload.name}.json"

    print(f"[ Executing: {cli_message_command} ]")
    subprocess.run(command, shell=True)
    print("[ Command finished ]")


def accuracy_bench():
    sketches = ["CMSketchbookAdaptiveCountersPQ", "CMSketchbookFixedCounters",
                "StingyCM", "SALSACM", "CodingCM", "BitSenseCM", "SEADCM",
                "Tailored", "OTailored", "Switch", "Waving"]
    memory_footprints = [2 ** i for i in range(17, 23)]
    workload_subdir = sys._getframe()
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/real")
    for workload in workload_path.iterdir():
        for sketch, bpk in itertools.product(sketches, memory_footprints):
            execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, bpk)


def skew_bench():
    sketches = ["CMSketchbookAdaptiveCountersPQ", "CMSketchbookFixedCounters", "StingyCM", "SALSACM", "Tailored"]
    MEMORY_FOOTPRINT = 2 ** 18
    workload_subdir = sys._getframe()
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/synthetic")
    for workload in workload_path.iterdir():
        for sketch in sketches:
            execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, MEMORY_FOOTPRINT)


def vale_tuning_bench():
    sketches = ["CMSketchbookAdaptiveCountersPQ", "CMSketchbookAdaptiveCountersPQNoTuning"]
    MEMORY_FOOTPRINT = 2 ** 18
    workload_subdir = sys._getframe()
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/real")
    for workload in workload_path.iterdir():
        if "caida" not in workload.name:
            continue
        for sketch in sketches:
            execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, MEMORY_FOOTPRINT)


def expansion_bench():
    sketches = ["CMSketchbookAdaptiveCountersPQ", "CMSketchbookFixedCounters",
                "StingyCM", "SALSACM", "Tailored"]
    OVERESTIMATE_MEMORY = 2 ** 26
    UNDERESTIMATE_MEMORY = 2 ** 18
    workload_subdir = sys._getframe()
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/expand")
    for workload in workload_path.iterdir():
        for sketch in sketches:
            if sketch == "CMSketchbookAdaptiveCountersPQ":
                execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, UNDERESTIMATE_MEMORY, 1.0, 30)
            else:
                execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, UNDERESTIMATE_MEMORY)
                execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, OVERESTIMATE_MEMORY)


def size_function_bench():
    SKETCH = "CMSketchbookAdaptiveCountersPQ"
    START_MEMORY_FOOTPRINT = 2 ** 18
    size_function_powers = [i / 4 for i in range(5)]
    workload_subdir = sys._getframe()
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/expand")
    for workload in workload_path.iterdir():
        for size_function_power in size_function_powers:
            execute_benchmark(build_dir, output_base, workload_subdir, workload, SKETCH, START_MEMORY_FOOTPRINT, size_function_power, 1)


def contraction_bench():
    sketches = ["CMSketchbookAdaptiveCountersPQ", "CMSketchbookFixedCounters",
                "BitSenseCM", "SEADCM"]
    MEMORY_FOOTPRINT = 2 ** 24
    START_MEMORY_FOOTPRINT  = 2 ** 18
    workload_subdir = sys._getframe()
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/delete")
    for workload in workload_path.iterdir():
        for sketch in sketches:
            memory_footprint = START_MEMORY_FOOTPRINT if sketch == "CMSketchbookAdaptiveCountersPQ" else MEMORY_FOOTPRINT
            execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, memory_footprint, 1.0, 30)


def accuracy_unbiased_bench():
    sketches = ["CSketchbookAdaptiveCountersPQ", "CSketchbookFixedCounters", 
                "StingyC", "CodingC", "BitSenseC", "SEADC"]
    memory_footprints = [2 ** i for i in range(17, 23)]
    workload_subdir = sys._getframe()
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/real")
    for workload in workload_path.iterdir():
        if "caida" not in workload.name:
            continue
        for sketch, bpk in itertools.product(sketches, memory_footprints):
            execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, bpk)


RUNNERS = {accuracy_bench.__name__[:-6]: accuracy_bench,
           skew_bench.__name__[:-6]: skew_bench,
           vale_tuning_bench.__name__[:-6]: vale_tuning_bench,
           expansion_bench.__name__[:-6]: expansion_bench,
           size_function_bench.__name__[:-6]: size_function_bench,
           contraction_bench.__name__[:-6]: contraction_bench,
           accuracy_unbiased_bench.__name__[:-6]: accuracy_unbiased_bench}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(prog="run_benchmarks")
    parser.add_argument("build_dir", type=Path, help="The directory containing the benchmark binaries")
    parser.add_argument("workload_dir", type=Path, help="The directory containing the generated workloads")
    parser.add_argument("-b", "--benchmarks", nargs="+", choices=["all",] + list(RUNNERS.keys()),
                        default=["all"], type=str, help="The benchmarks to run")

    args = parser.parse_args()
    build_dir = args.build_dir
    workload_dir = args.workload_dir

    if not workload_dir.exists():
        raise FileNotFoundError("The workload directory does not exist")
    if not build_dir.exists():
        raise FileNotFoundError("The build directory does not exist")

    benchmarks_dir = Path(f"{build_dir}/bench/")
    output_prefix = Path(f"results/{datetime.now().strftime('%Y-%m-%d.%H:%M:%S')}")

    try:
        for benchmark in (RUNNERS if "all" in args.benchmarks else args.benchmarks):
            RUNNERS[benchmark]()
    except Exception as e:
        print(f"Received exception: {str(e)}, cleaning up output and closing")
        shutil.rmtree(output_prefix)

