import argparse, shutil, itertools, subprocess, inspect
from pathlib import Path
from datetime import datetime

global build_dir
global workload_dir
global output_prefix

SKETCHES_WITH_VALE = {"SublimeCMS",
                      "SublimeCS"}
SKETCHES_WITH_EXPANSION_RATE_FUNCTION = {"SublimeCMS",
                                         "SublimeCMSNoTuning",
                                         "SublimeCMSNoTuningMorris",
                                         "SublimeCS",
                                         "SublimeCSNoTuning"}

def execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, bpk, size_function_power=None, size_function_mult=None, force_counter_count=None, override_size=None):
    file_to_execute = f"bench/bench_{sketch}"
    size_function_power_option = f"--size-function-power {size_function_power}" if size_function_power != None else ""
    size_function_mult_option = f"--size-function-mult {size_function_mult}" if size_function_mult != None else ""
    counter_count_option = f"--counter-count {force_counter_count}" if force_counter_count != None else ""
    command = f"{build_dir}/{file_to_execute} {bpk} -w {workload} {size_function_power_option} {size_function_mult_option} {counter_count_option} | tee {output_base}/{sketch}_{bpk if override_size == None else override_size}_{workload.name}.json"
    cli_message_command = f"<build_dir>/{file_to_execute} {bpk} -w <workload_dir>/{workload_subdir}/{workload.name} {size_function_power_option} {size_function_mult_option} {counter_count_option} | tee <output_dir>/{workload_subdir}/{sketch}_{bpk if override_size == None else override_size}_{workload.name}.json"

    print(f"[ Executing: {cli_message_command} ]")
    subprocess.run(command, shell=True)
    print("[ Command finished ]")


def rebuild_execute_benchmark(build_dir, output_base, c, s, p, workload_subdir, workload, sketch, bpk, size_function_power=None, size_function_mult=None, force_counter_count=None, override_size=None):
    rebuild_command = f"cd {build_dir} && cmake .. -DCMAKE_BUILD_TYPE=Release -DFIXED_TUNING_C={c} -DFIXED_TUNING_S={s}"
    if p is not None:
        rebuild_command += f" -DLOG_REC_INC_PROB={p}"
    rebuild_command += "&& make -j8"
    subprocess.run(rebuild_command, shell=True)

    file_to_execute = f"bench/bench_{sketch}"
    size_function_power_option = f"--size-function-power {size_function_power}" if size_function_power != None else ""
    size_function_mult_option = f"--size-function-mult {size_function_mult}" if size_function_mult != None else ""
    counter_count_option = f"--counter-count {force_counter_count}" if force_counter_count != None else ""
    command = f"{build_dir}/{file_to_execute} {bpk} -w {workload} {size_function_power_option} {size_function_mult_option} {counter_count_option} | tee {output_base}/{sketch}_{bpk if override_size == None else override_size}_{workload.name}.json"
    cli_message_command = f"<build_dir>/{file_to_execute} {bpk} -w <workload_dir>/{workload_subdir}/{workload.name} {size_function_power_option} {size_function_mult_option} {counter_count_option} | tee <output_dir>/{workload_subdir}/{sketch}_{bpk if override_size == None else override_size}_{workload.name}.json"

    print(f"[ Executing: {cli_message_command} ]")
    subprocess.run(command, shell=True)
    print("[ Command finished ]")


def accuracy_bench():
    sketches = ["CMS", "StingyCM", "Tailored", "SALSACM", "CodingCM", "Waving"]
    memory_footprints = {"caida": [2 ** i for i in range(17, 23)],
                         "kosarak": [2 ** i for i in range(15, 21)],
                         "webdocs": [2 ** i for i in range(17, 23)]}
    vale_params = {"caida": [(39, 10), (42, 9), (47, 8), (53, 7), (69, 5), (80, 4)],
                   "kosarak": [(39, 10), (43, 9), (50, 7), (68, 5), (73, 5), (81, 4)],
                   "webdocs": [(31, 12), (34, 12), (38, 10), (41, 10), (50, 7), (64, 5)]}
    morris_params = [(80, 5, 9), (80, 5, 7), (80, 5, 6), (80, 5, 4), (81, 4, 1), (81, 4, 0)]
    workload_subdir = inspect.stack()[0][3]
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/real")
    for workload in workload_path.iterdir():
        if workload.name not in memory_footprints:
            continue

        sketch = "SublimeCMSNoTuning"
        for memory_footprint, (c, s) in zip(memory_footprints[workload.name], vale_params[workload.name]):
            rebuild_execute_benchmark(build_dir, output_base, c, s, None, workload_subdir, workload, sketch, memory_footprint)

        if workload.name == "caida":
            sketch = "SublimeCMSNoTuningMorris"
            for memory_footprint, (c, s, p) in zip(memory_footprints[workload.name], morris_params):
                rebuild_execute_benchmark(build_dir, output_base, c, s, p, workload_subdir, workload, sketch, memory_footprint)
        
        for sketch, memory_footprint in itertools.product(sketches, memory_footprints[workload.name]):
            execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, memory_footprint)


def skew_bench():
    sketches = ["SublimeCMS", "CMS", "StingyCM", "Tailored", "SALSACM", "Waving"]
    CODINGCM_MEMORY_FOOTPRINT = 600000
    MEMORY_FOOTPRINT = 2 ** 20
    sketchbook_memory_footprints = {"0.00": 640000,
                                    "0.20": 620000,
                                    "0.40": 610000,
                                    "0.80": 570000,
                                    "1.60": MEMORY_FOOTPRINT,
                                    "3.20": MEMORY_FOOTPRINT}
    workload_subdir = inspect.stack()[0][3]
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/synthetic")
    for workload in workload_path.iterdir():
        for sketch in sketches:
            char_exp = workload.name[5:]
            if sketch in SKETCHES_WITH_VALE:
                execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, sketchbook_memory_footprints[char_exp],
                                  override_size=MEMORY_FOOTPRINT)
            else:
                memory_footprint = CODINGCM_MEMORY_FOOTPRINT if sketch == "CodingCM" else MEMORY_FOOTPRINT
                execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, memory_footprint, override_size=MEMORY_FOOTPRINT)


def vale_tuning_bench():
    sketches = ["SublimeCMS", "SublimeCMSNoTuning"]
    COUNTER_COUNT = 2 ** 20
    workload_subdir = inspect.stack()[0][3]
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/synthetic")
    for workload in workload_path.iterdir():
        for sketch in sketches:
            execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, COUNTER_COUNT,
                              force_counter_count=COUNTER_COUNT)


def expansion_bench():
    sketches = ["SublimeCMS", "CMS", "StingyCM", "Tailored", "SALSACM", "Waving"]
    OVERESTIMATE_MEMORY = 2 ** 24
    UNDERESTIMATE_MEMORY = 2 ** 15
    size_function_powers = [0.5, 0.75, 1.0]
    size_function_mults = [5.0, 50.0, 100.0]
    workload_subdir = inspect.stack()[0][3]
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/expand")
    for workload in workload_path.iterdir():
        for sketch in sketches:
            if sketch in SKETCHES_WITH_EXPANSION_RATE_FUNCTION:
                for power, mult in zip(size_function_powers, size_function_mults):
                    execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, UNDERESTIMATE_MEMORY, power, mult, 
                                      override_size=f"{UNDERESTIMATE_MEMORY}_{power:.2f}")
            else:
                execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, UNDERESTIMATE_MEMORY)
                if sketch == "CMS":
                    execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, OVERESTIMATE_MEMORY)


def contraction_bench():
    sketches = ["SublimeCMS", "CMS"]
    MEMORY_FOOTPRINT = 3947584
    START_COUNTER_COUNT = 2 ** 22
    workload_subdir = inspect.stack()[0][3]
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/delete")
    for workload in workload_path.iterdir():
        for sketch in sketches:
            if sketch in SKETCHES_WITH_EXPANSION_RATE_FUNCTION:
                execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, MEMORY_FOOTPRINT, 
                                  1.0, 20.0, force_counter_count=START_COUNTER_COUNT, override_size=START_COUNTER_COUNT)
            else:
                execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, MEMORY_FOOTPRINT)


def accuracy_unbiased_bench():
    sketches = ["CS", "StingyC", "CodingC", "Waving"]
    memory_footprints = [2 ** i for i in range(17, 23)]
    vale_params = [(39, 10), (42, 9), (45, 8), (51, 7), (64, 5), (75, 4)]
    workload_subdir = inspect.stack()[0][3]
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/real")
    for workload in workload_path.iterdir():
        if workload.name != "caida":
            continue

        sketch = "SublimeCSNoTuning"
        for memory_footprint, (c, s) in zip(memory_footprints, vale_params):
            rebuild_execute_benchmark(build_dir, output_base, c, s, None, workload_subdir, workload, sketch, memory_footprint)

        for sketch, memory_footprint in itertools.product(sketches, memory_footprints):
            execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, memory_footprint)


RUNNERS = {accuracy_bench.__name__[:-6]: accuracy_bench,
           skew_bench.__name__[:-6]: skew_bench,
           vale_tuning_bench.__name__[:-6]: vale_tuning_bench,
           expansion_bench.__name__[:-6]: expansion_bench,
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

