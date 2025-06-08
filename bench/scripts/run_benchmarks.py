import argparse, shutil, itertools, subprocess
from pathlib import Path
from datetime import datetime

global build_dir
global workload_dir
global benchmarks_dir
global output_prefix

SKETCHS_WITH_EXPANSION_RATE_FUNCTION = {"CMSketchbookFixedCounters",
                                        "CMSketchbookSemiAdaptiveCounters",
                                        "CMSketchbookAdaptiveCounters",
                                        "CMSketchbookAdaptiveCountersPQ",
                                        "CSketchbookFixedCounters",
                                        "CSketchbookSemiAdaptiveCounters",
                                        "CSketchbookAdaptiveCounters",
                                        "CSketchbookAdaptiveCountersPQ"}

def execute_benchmark(build_dir, output_base, workload_subdir, workload, filter, bpk, expansion_power=None):
    file_to_execute = f"bench/bench_{filter}"
    expansion_power_option = f"--expansion-power {expansion_power}" if expansion_power != None else ""
    command = f"{build_dir}/{file_to_execute} {bpk} -w {workload} {expansion_power_option} | tee {output_base}/{filter}_{bpk}_{workload.name}.json"
    cli_message_command = f"<build_dir>/{file_to_execute} {bpk} -w <workload_dir>/{workload_subdir}/{workload.name} {expansion_power_option} | tee <output_dir>/{workload_subdir}/{filter}_{bpk}_{workload.name}.json"

    print(f"[ Executing: {cli_message_command} ]")
    subprocess.run(command, shell=True)
    print("[ Command finished ]")

def classical_bench():
    sketches = ["CMSketchbookFixedCounters", "CSketchbookFixedCounters", "MGDummy"]
    memory_footprints = [2 ** i for i in range(15, 21)]
    MEMORY_FOOTPRINT = 2 ** 17
    workload_subdir = "classical_comparison_bench"
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/{workload_subdir}")
    for workload in workload_path.iterdir():
        for sketch, bpk in itertools.product(sketches, memory_footprints):
            if bpk == MEMORY_FOOTPRINT or "1.00" in workload.name:
                execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, bpk)

def accuracy_bench():
    sketches = ["CMSketchbookFixedCounters", "CSketchbookFixedCounters", "MGDummy",
                "CMSketchbookAdaptiveCounters", "CMSketchbookAdaptiveCountersPQ",
                "CSketchbookAdaptiveCounters", "CSketchbookAdaptiveCountersPQ",
                "StingyCM", "StingyC",
                "CodingCM", "CodingC",
                "BitSenseCM", "BitSenseC",
                "SEADCM", "SEADC",
                "Switch",
                "Tailored", "OTailored"]
    memory_footprints = [2 ** i for i in range(17, 18)]
    workload_subdir = "accuracy_bench"
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/{workload_subdir}")
    for workload in workload_path.iterdir():
        for sketch, bpk in itertools.product(sketches, memory_footprints):
            execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, bpk)


RUNNERS = {"classical": classical_bench,
           "accuracy": accuracy_bench}


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

