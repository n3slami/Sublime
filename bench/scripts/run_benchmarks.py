import argparse, shutil, itertools, subprocess, inspect
from pathlib import Path
from datetime import datetime

global build_dir
global workload_dir
global output_prefix

SKETCHES_WITH_VALE = {"CMSketchbookAdaptiveCountersPQ",
                      "CSketchbookAdaptiveCountersPQ"}
SKETCHES_WITH_EXPANSION_RATE_FUNCTION = {"CMSketchbookAdaptiveCountersPQ",
                                         "CMSketchbookAdaptiveCountersPQNoTuning",
                                         "CSketchbookAdaptiveCountersPQ"}

def execute_benchmark(build_dir, output_base, workload_subdir, workload, filter, bpk, size_function_power=None, size_function_mult=None, override_size=None):
    file_to_execute = f"bench/bench_{filter}"
    size_function_power_option = f"--size-function-power {size_function_power}" if size_function_power != None else ""
    size_function_mult_option = f"--size-function-mult {size_function_mult}" if size_function_mult != None else ""
    command = f"{build_dir}/{file_to_execute} {bpk} -w {workload} {size_function_power_option} {size_function_mult_option} | tee {output_base}/{filter}_{bpk if override_size == None else override_size}_{workload.name}.json"
    cli_message_command = f"<build_dir>/{file_to_execute} {bpk} -w <workload_dir>/{workload_subdir}/{workload.name} {size_function_power_option} {size_function_mult_option} | tee <output_dir>/{workload_subdir}/{filter}_{bpk if override_size == None else override_size}_{workload.name}.json"

    print(f"[ Executing: {cli_message_command} ]")
    subprocess.run(command, shell=True)
    print("[ Command finished ]")


def accuracy_bench():
    sketches = ["CMSketchbookAdaptiveCountersPQ", "CMSketchbookAdaptiveCountersPQNoTuning",
                "CMSketchbookFixedCounters",
                "StingyCM", "SALSACM", "CodingCM", "SEADCM", "Waving"]
    memory_footprints = {"caida": [2 ** i for i in range(17, 23)],
                         "kosarak": [2 ** i for i in range(15, 21)],
                         "webdocs": [2 ** i for i in range(17, 23)]}
    sketchbook_memory_reduction = {"caida": (2 ** 19, 1.5),
                                   "kosarak": (2 ** 17, 1.5),
                                   "webdocs": (2 ** 21, 1.8)}
    workload_subdir = inspect.stack()[0][3]
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/real")
    for workload in workload_path.iterdir():
        if "repeat" in workload.name:
            continue
        for sketch, memory_footprint in itertools.product(sketches, memory_footprints[workload.name]):
            if sketch in SKETCHES_WITH_EXPANSION_RATE_FUNCTION:
                threshold, mult = sketchbook_memory_reduction[workload.name]
                tweaked_memory_footprint = int(memory_footprint / (mult if memory_footprint <= threshold else 1.0))
                execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, tweaked_memory_footprint, override_size=memory_footprint)
            else:
                execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, memory_footprint)


def skew_bench():
    sketches = ["CMSketchbookAdaptiveCountersPQ", "CMSketchbookFixedCounters",
                "StingyCM", "SALSACM", "CodingCM", "SEADCM", "Waving"]
    MEMORY_FOOTPRINT = 2 ** 20
    baseline_memory_footprints = [1697728, 1739136, 1658584, 1912352, 1658240, 1400592]
    sketchbook_memory_footprints = [640000, 620000, 610000, 590000, 570000, 650000]
    workload_subdir = inspect.stack()[0][3]
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/synthetic")
    for workload in workload_path.iterdir():
        for sketch in sketches:
            workload_ind = int(float(workload.name[5:]) * 5)
            if sketch in SKETCHES_WITH_VALE:
                #execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, MEMORY_FOOTPRINT)
                execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, sketchbook_memory_footprints[workload_ind], override_size=MEMORY_FOOTPRINT)
            else:
                #execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, baseline_memory_footprints[workload_ind])
                execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, MEMORY_FOOTPRINT)


def vale_tuning_bench():
    sketches = ["CMSketchbookAdaptiveCountersPQ", "CMSketchbookAdaptiveCountersPQNoTuning"]
    memory_footprints = [2 ** i for i in range(16, 22)]
    workload_subdir = inspect.stack()[0][3]
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/real")
    for workload in workload_path.iterdir():
        if "caida_repeat" not in workload.name:
            continue
        for sketch, memory_footprint in itertools.product(sketches, memory_footprints):
            execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, memory_footprint)


def expansion_bench():
    sketches = ["CMSketchbookAdaptiveCountersPQ", "CMSketchbookFixedCounters",
                "StingyCM", "SALSACM", "CodingCM", "Waving"]
    OVERESTIMATE_MEMORY = 2 ** 24
    UNDERESTIMATE_MEMORY = 2 ** 18
    workload_subdir = inspect.stack()[0][3]
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/expand")
    for workload in workload_path.iterdir():
        for sketch in sketches:
            if sketch in SKETCHES_WITH_EXPANSION_RATE_FUNCTION:
                execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, UNDERESTIMATE_MEMORY, 1.0, 19.7)
            else:
                execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, UNDERESTIMATE_MEMORY)
                execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, OVERESTIMATE_MEMORY)


def size_function_bench():
    SKETCH = "CMSketchbookAdaptiveCountersPQ"
    START_MEMORY_FOOTPRINT = 2 ** 14
    size_function_powers = [i / 4 for i in range(5)]
    size_function_mults = [1e0, 1e-4, 1e1, 1e1, 1e1]
    workload_subdir = inspect.stack()[0][3]
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/expand")
    for workload in workload_path.iterdir():
        for size_function_power, size_function_mult in zip(size_function_powers, size_function_mults):
            execute_benchmark(build_dir, output_base, workload_subdir, workload, SKETCH, START_MEMORY_FOOTPRINT, size_function_power, size_function_mult, override_size=f"{size_function_power:.2f}")


def contraction_bench():
    sketches = ["CMSketchbookAdaptiveCountersPQ", "CMSketchbookFixedCounters"]
    MEMORY_FOOTPRINT = 2 ** 22
    START_MEMORY_FOOTPRINT  = 2 ** 18
    workload_subdir = inspect.stack()[0][3]
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/delete")
    for workload in workload_path.iterdir():
        for sketch in sketches:
            if sketch in SKETCHES_WITH_EXPANSION_RATE_FUNCTION:
                execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, START_MEMORY_FOOTPRINT, 1.0, 19.7)
            else:
                execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, MEMORY_FOOTPRINT)


def accuracy_unbiased_bench():
    sketches = ["CSketchbookAdaptiveCountersPQ", "CSketchbookFixedCounters", 
                "StingyC", "CodingC", "Waving"]
    memory_footprints = [2 ** i for i in range(17, 23)]
    workload_subdir = inspect.stack()[0][3]
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)
    SKETCHBOOK_MEMORY_REDUCTION_THRESHOLD = 2 ** 20
    SKETCHBOOK_MEMORY_REDUCTION_MULT = 1.5

    workload_path = Path(f"{workload_dir}/real")
    for workload in workload_path.iterdir():
        if "repeat" in workload.name or "caida" not in workload.name:
            continue
        for sketch, memory_footprint in itertools.product(sketches, memory_footprints):
            if sketch in SKETCHES_WITH_EXPANSION_RATE_FUNCTION:
                tweaked_memory_footprint = int(memory_footprint / (SKETCHBOOK_MEMORY_REDUCTION_MULT if memory_footprint <= SKETCHBOOK_MEMORY_REDUCTION_THRESHOLD else 1.0))
                execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, tweaked_memory_footprint, override_size=memory_footprint)
            else:
                execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, memory_footprint)


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

    output_prefix = Path(f"results/{datetime.now().strftime('%Y-%m-%d.%H:%M:%S')}")

    try:
        for benchmark in (RUNNERS if "all" in args.benchmarks else args.benchmarks):
            RUNNERS[benchmark]()
    except Exception as e:
        print(f"Received exception: {str(e)}, cleaning up output and closing")
        shutil.rmtree(output_prefix)

