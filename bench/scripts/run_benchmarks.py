import argparse, itertools, subprocess, inspect, os, sys, traceback
from pathlib import Path
from datetime import datetime

global build_dir
global workload_dir
global output_prefix

# Set by `--quick`, which runs one memory budget per workload instead of the
# whole sweep. Paired with the same flag in `generate_datasets.sh`, which
# generates only the smallest dataset, it takes the pipeline end to end in
# minutes -- enough to show that everything works, not enough to reproduce a
# number from the paper.
QUICK = False


def budgets(footprints):
    """The memory budgets to sweep for one workload, honouring `--quick`.

    Two rather than one: the figures plot error against memory on log axes, and
    a single point per series leaves them with nothing to scale.
    """
    return footprints[:2] if QUICK else footprints

SKETCHES_WITH_VALE = {"SublimeCMS",
                      "SublimeCS"}
SKETCHES_WITH_EXPANSION_RATE_FUNCTION = {"SublimeCMS",
                                         "SublimeCMSNoTuning",
                                         "SublimeCMSNoTuningMorris",
                                         "SublimeCS",
                                         "SublimeCSl2",
                                         "SublimeCSNoTuning"}

def execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, bpk, size_function_power=None, size_function_mult=None, force_counter_count=None, override_size=None, extra_args="", label=None):
    # `label` names the result file when one binary is run in several
    # configurations -- `SublimeMG --cuckoo --min-tree` is still bench_SublimeMG,
    # but its results have to land somewhere the plotter can tell apart.
    file_to_execute = f"bench/bench_{sketch}"
    size_function_power_option = f"--size-function-power {size_function_power}" if size_function_power != None else ""
    size_function_mult_option = f"--size-function-mult {size_function_mult}" if size_function_mult != None else ""
    counter_count_option = f"--counter-count {force_counter_count}" if force_counter_count != None else ""
    options = f"{size_function_power_option} {size_function_mult_option} {counter_count_option} {extra_args}"
    name = sketch if label is None else label
    if type(bpk) is tuple:
        bpk = [str(i) for i in bpk]
        command = f"{build_dir}/{file_to_execute} {' '.join(bpk)} -w {workload} {options} | tee {output_base}/{name}_{'_'.join(bpk) if override_size == None else override_size}_{workload.name}.json"
        cli_message_command = f"<build_dir>/{file_to_execute} {' '.join(bpk)} -w <workload_dir>/{workload_subdir}/{workload.name} {options} | tee <output_dir>/{workload_subdir}/{name}_{'_'.join(bpk) if override_size == None else override_size}_{workload.name}.json"
    else:
        command = f"{build_dir}/{file_to_execute} {bpk} -w {workload} {options} | tee {output_base}/{name}_{bpk if override_size == None else override_size}_{workload.name}.json"
        cli_message_command = f"<build_dir>/{file_to_execute} {bpk} -w <workload_dir>/{workload_subdir}/{workload.name} {options} | tee <output_dir>/{workload_subdir}/{name}_{bpk if override_size == None else override_size}_{workload.name}.json"

    print(f"[ Executing: {cli_message_command} ]")
    subprocess.run(command, shell=True)
    print("[ Command finished ]")


def rebuild_execute_benchmark(build_dir, output_base, c, s, p, workload_subdir, workload, sketch, bpk, size_function_power=None, size_function_mult=None, force_counter_count=None, override_size=None, extra_args="", label=None, target=None):
    rebuild_command = f"cd {build_dir} && cmake .. -DCMAKE_BUILD_TYPE=Release -DFIXED_TUNING_C={c} -DFIXED_TUNING_S={s}"
    if p is not None:
        rebuild_command += f" -DLOG_REC_INC_PROB={p}"
    # Naming a target keeps a per-data-point rebuild to seconds instead of
    # rebuilding every sketch in the tree.
    rebuild_command += f" && make -j8 {target if target is not None else ''}"
    subprocess.run(rebuild_command, shell=True)

    file_to_execute = f"bench/bench_{sketch}"
    size_function_power_option = f"--size-function-power {size_function_power}" if size_function_power != None else ""
    size_function_mult_option = f"--size-function-mult {size_function_mult}" if size_function_mult != None else ""
    counter_count_option = f"--counter-count {force_counter_count}" if force_counter_count != None else ""
    options = f"{size_function_power_option} {size_function_mult_option} {counter_count_option} {extra_args}"
    name = sketch if label is None else label
    if type(bpk) is tuple:
        bpk = [str(i) for i in bpk]
        command = f"{build_dir}/{file_to_execute} {' '.join(bpk)} -w {workload} {options} | tee {output_base}/{name}_{'_'.join(bpk) if override_size == None else override_size}_{workload.name}.json"
        cli_message_command = f"<build_dir>/{file_to_execute} {' '.join(bpk)} -w <workload_dir>/{workload_subdir}/{workload.name} {options} | tee <output_dir>/{workload_subdir}/{name}_{'_'.join(bpk) if override_size == None else override_size}_{workload.name}.json"
    else:
        command = f"{build_dir}/{file_to_execute} {bpk} -w {workload} {options} | tee {output_base}/{name}_{bpk if override_size == None else override_size}_{workload.name}.json"
        cli_message_command = f"<build_dir>/{file_to_execute} {bpk} -w <workload_dir>/{workload_subdir}/{workload.name} {options} | tee <output_dir>/{workload_subdir}/{name}_{bpk if override_size == None else override_size}_{workload.name}.json"

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
        for memory_footprint, (c, s) in zip(budgets(memory_footprints[workload.name]), vale_params[workload.name]):
            rebuild_execute_benchmark(build_dir, output_base, c, s, None, workload_subdir, workload, sketch, memory_footprint)

        if workload.name == "caida":
            sketch = "SublimeCMSNoTuningMorris"
            for memory_footprint, (c, s, p) in zip(budgets(memory_footprints[workload.name]), morris_params):
                rebuild_execute_benchmark(build_dir, output_base, c, s, p, workload_subdir, workload, sketch, memory_footprint)
        
        for sketch, memory_footprint in itertools.product(sketches, budgets(memory_footprints[workload.name])):
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
        char_exp = workload.name[5:]
        # `generate_synthetic` makes eight Zipfian workloads and this figure
        # plots six of them -- `plot_skew_vale_tuning`'s `char_exps` is the same
        # six listed above. The other two (0.60 and 1.00) have no budget here,
        # and indexing the dict with them used to raise a KeyError that took the
        # whole run down with it. They are simply not part of this figure.
        if char_exp not in sketchbook_memory_footprints:
            continue
        for sketch in sketches:
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
        for memory_footprint, (c, s) in zip(budgets(memory_footprints), vale_params):
            rebuild_execute_benchmark(build_dir, output_base, c, s, None, workload_subdir, workload, sketch, memory_footprint)

        for sketch, memory_footprint in itertools.product(sketches, budgets(memory_footprints)):
            execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, memory_footprint)


def l2_size_function_bench():
    sketches = ["SublimeCS", "SublimeCSl2"]
    UNDERESTIMATE_MEMORY = 2 ** 15
    SIZE_FUNCTION_POWER = 0.5
    SIZE_FUNCTION_MULT = 0.5
    workload_subdir = inspect.stack()[0][3]
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/synthetic")
    for workload in workload_path.iterdir():
        for sketch in sketches:
            execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch,
                              UNDERESTIMATE_MEMORY, SIZE_FUNCTION_POWER , SIZE_FUNCTION_MULT, 
                              override_size=f"{UNDERESTIMATE_MEMORY}_{SIZE_FUNCTION_POWER:.2f}")

def join_size_bench():
    sketches = ["SublimeCMS", "CMS", "SublimeCS", "CS"]
    MEMORY_FOOTPRINTS = (3 * 2 ** 21, 2 ** 21)  # Distribute the memory budget 3:1 among the tables
    #cms_vale_params = [(52, 8), (53, 8), (54, 8), (54, 8), (54, 8), (55, 7), (55, 7), (55, 7), (55, 7)]
    #cs_vale_params = [(52, 8), (53, 8), (53, 8), (53, 8), (54, 7), (55, 7), (55, 7), (55, 7), (55, 7)]
    workload_subdir = inspect.stack()[0][3]
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/real")
    for workload in workload_path.iterdir():
        if workload.name != "join_size":
            continue

        #sketch = "SublimeCMSNoTuning"
        #for memory_footprint, (c, s) in zip(memory_footprints, cms_vale_params):
        #    rebuild_execute_benchmark(build_dir, output_base, c, s, None, workload_subdir, workload, sketch, memory_footprint)

        #sketch = "SublimeCSNoTuning"
        #for memory_footprint, (c, s) in zip(memory_footprints, cs_vale_params):
        #    rebuild_execute_benchmark(build_dir, output_base, c, s, None, workload_subdir, workload, sketch, memory_footprint)

        for sketch in sketches:
            execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, MEMORY_FOOTPRINTS)
            if sketch in SKETCHES_WITH_EXPANSION_RATE_FUNCTION:
                execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, MEMORY_FOOTPRINTS,
                                  size_function_power=0.75, size_function_mult=0.05, override_size="expand")
            

def mg_accuracy_bench():
    SEED = 12345                            # Fixed for the sketches that support it (reproducibility).
    # The baselines, which tune nothing: (binary, result label, extra flags).
    baselines = [("MG", "MG", f"--seed {SEED}"),
                 ("SpaceSaving", "SpaceSaving", ""),
                 ("Waving", "Waving", "")]
    # Sublime_MG runs in both of its configurations -- the decrement sweep and
    # the min tree -- with VALE's tuning *baked in at compile time*, as the
    # Sublime_CMS and Sublime_CS accuracy figures do. So each of these points is
    # its own configure-and-build of `bench_SublimeMGNoTuning`, which is why the
    # target is named: rebuilding the whole tree 22 times over would cost more
    # than the runs.
    #
    # The pairs are `(counters_per_chunk, stub_length)` per budget, ascending,
    # and they are what the auto-tuning build settled on in the sweep of
    # 2026-09-28. Each configuration gets its own: the tree's counters carry
    # whatever of the lazy decrement their group still owes, so they run a
    # little wider and tune a notch differently.
    vale_params = {
        ("kosarak", "SublimeMG"):      [(39, 9), (37, 10), (41, 9), (46, 8), (52, 7)],
        ("kosarak", "SublimeMG_tree"): [(37, 10), (39, 10), (38, 10), (47, 8), (55, 6)],
        ("caida", "SublimeMG"):        [(40, 9), (49, 7), (46, 8), (56, 6), (68, 5), (68, 5)],
        ("caida", "SublimeMG_tree"):   [(39, 10), (42, 9), (44, 8), (50, 7), (58, 6), (68, 5)],
        ("webdocs", "SublimeMG"):      [(31, 12), (33, 11), (39, 9), (45, 8), (44, 8), (55, 6)],
        ("webdocs", "SublimeMG_tree"): [(28, 13), (32, 11), (35, 10), (40, 9), (46, 8), (56, 6)],
    }
    sublime_flags = {"SublimeMG": f"--seed {SEED}",
                     "SublimeMG_tree": f"--seed {SEED} --min-tree"}
    memory_footprints = {"caida": [2 ** i for i in range(17, 23)],
                         "kosarak": [2 ** i for i in range(14, 19)],
                         "webdocs": [2 ** i for i in range(17, 23)]}
    workload_subdir = inspect.stack()[0][3]
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/real")
    for workload in workload_path.iterdir():
        if workload.name not in memory_footprints:
            continue
        for (sketch, label, extra_args), memory_footprint in \
                itertools.product(baselines, budgets(memory_footprints[workload.name])):
            execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, memory_footprint,
                              extra_args=extra_args, label=label)
        for label, flags in sublime_flags.items():
            params = vale_params[(workload.name, label)]
            assert len(params) == len(memory_footprints[workload.name])
            for memory_footprint, (c, s) in zip(budgets(memory_footprints[workload.name]), params):
                rebuild_execute_benchmark(build_dir, output_base, c, s, None, workload_subdir,
                                          workload, "SublimeMGNoTuning", memory_footprint,
                                          extra_args=flags, label=label,
                                          target="bench_SublimeMGNoTuning")


def mg_tail_latency_bench():
    """The worst insertion, rather than the average one.

    Its own benchmark because it has to be its own *run*: timing every single
    insertion costs about as much as a cuckoo-table insertion itself, so the
    average latency a run like this reports is meaningless and must not be
    confused with `mg_accuracy`'s. The tail is unharmed by the instrumentation
    -- what it measures is microseconds of eviction sweep against tens of
    nanoseconds of two clock reads -- which is the whole reason this is
    separable. See `bench_template.hpp`'s `measure_insert_latency`.

    Same sketches, same budgets and the same baked-in VALE tunings as
    `mg_accuracy`, so the two figures' rows line up point for point.
    """
    SEED = 12345
    baselines = [("MG", "MG", f"--seed {SEED}"),
                 ("SpaceSaving", "SpaceSaving", ""),
                 ("Waving", "Waving", "")]
    vale_params = {
        ("kosarak", "SublimeMG"):      [(39, 9), (37, 10), (41, 9), (46, 8), (52, 7)],
        ("kosarak", "SublimeMG_tree"): [(37, 10), (39, 10), (38, 10), (47, 8), (55, 6)],
        ("caida", "SublimeMG"):        [(40, 9), (49, 7), (46, 8), (56, 6), (68, 5), (68, 5)],
        ("caida", "SublimeMG_tree"):   [(39, 10), (42, 9), (44, 8), (50, 7), (58, 6), (68, 5)],
        ("webdocs", "SublimeMG"):      [(31, 12), (33, 11), (39, 9), (45, 8), (44, 8), (55, 6)],
        ("webdocs", "SublimeMG_tree"): [(28, 13), (32, 11), (35, 10), (40, 9), (46, 8), (56, 6)],
    }
    sublime_flags = {"SublimeMG": f"--seed {SEED}",
                     "SublimeMG_tree": f"--seed {SEED} --min-tree"}
    memory_footprints = {"caida": [2 ** i for i in range(17, 23)],
                         "kosarak": [2 ** i for i in range(14, 19)],
                         "webdocs": [2 ** i for i in range(17, 23)]}
    workload_subdir = inspect.stack()[0][3]
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/real")
    for workload in workload_path.iterdir():
        if workload.name not in memory_footprints:
            continue
        for (sketch, label, extra_args), memory_footprint in \
                itertools.product(baselines, budgets(memory_footprints[workload.name])):
            execute_benchmark(build_dir, output_base, workload_subdir, workload, sketch, memory_footprint,
                              extra_args=f"{extra_args} --tail-latency", label=label)
        for label, flags in sublime_flags.items():
            params = vale_params[(workload.name, label)]
            assert len(params) == len(memory_footprints[workload.name])
            for memory_footprint, (c, s) in zip(budgets(memory_footprints[workload.name]), params):
                rebuild_execute_benchmark(build_dir, output_base, c, s, None, workload_subdir,
                                          workload, "SublimeMGNoTuning", memory_footprint,
                                          extra_args=f"{flags} --tail-latency", label=label,
                                          target="bench_SublimeMGNoTuning")


def mg_expansion_bench():
    # SublimeMG starts here and grows; MG holds this size for the whole stream.
    START_MEMORY = 2 ** 15
    MG_MEMORY = 2 ** 15
    FINGERPRINT_LENGTH = 32                 # The sketches' default; long fingerprints cut collision over-estimation.
    SEED = 12345                            # Fixed so error and total are the same run bar the measure.
    size_function_powers = [0.5, 0.75, 1.0]
    # Mults are shared between the two measures at each power (W(N) =
    # (N/mult)^power), which is what guarantees error <= total: the error measure
    # is a subset of every insertion, so with the same size function it never
    # expands more than the total measure. Larger mult => later/less expansion;
    # these keep the power-0.5 (sqrt) line hugging the fixed baseline and the
    # power-1.0 total line bounded. Calibrated for these datasets' lengths.
    size_function_mults = [12, 2, 8]
    measures = ["error", "total"]
    workload_subdir = inspect.stack()[0][3]
    output_base = Path(f"./{output_prefix}/{workload_subdir}/")
    output_base.mkdir(parents=True, exist_ok=True)

    workload_path = Path(f"{workload_dir}/expand")
    for workload in workload_path.iterdir():
        # The dense-start workloads add early checkpoints so the sketches share a
        # visible common starting memory; the paper's plain expand workloads are
        # left to the CMS/CS expansion benchmark.
        if not workload.name.endswith("_mg_expand"):
            continue
        for measure in measures:
            for power, mult in zip(size_function_powers, size_function_mults):
                execute_benchmark(build_dir, output_base, workload_subdir, workload, "SublimeMG", START_MEMORY,
                                  power, mult, override_size=f"{START_MEMORY}_{measure}_{power:.2f}",
                                  extra_args=f"--expand-measure {measure} --growth-coefficient 4 "
                                             f"--fingerprint-length {FINGERPRINT_LENGTH} --seed {SEED}")
        execute_benchmark(build_dir, output_base, workload_subdir, workload, "MG", MG_MEMORY,
                          extra_args=f"--fingerprint-length {FINGERPRINT_LENGTH} --seed {SEED}")


RUNNERS = {accuracy_bench.__name__[:-6]: accuracy_bench,
           skew_bench.__name__[:-6]: skew_bench,
           vale_tuning_bench.__name__[:-6]: vale_tuning_bench,
           expansion_bench.__name__[:-6]: expansion_bench,
           contraction_bench.__name__[:-6]: contraction_bench,
           accuracy_unbiased_bench.__name__[:-6]: accuracy_unbiased_bench,
           l2_size_function_bench.__name__[:-6]: l2_size_function_bench,
           join_size_bench.__name__[:-6]: join_size_bench,
           mg_accuracy_bench.__name__[:-6]: mg_accuracy_bench,
           mg_tail_latency_bench.__name__[:-6]: mg_tail_latency_bench,
           mg_expansion_bench.__name__[:-6]: mg_expansion_bench}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(prog="run_benchmarks")
    parser.add_argument("build_dir", type=Path, help="The directory containing the benchmark binaries")
    parser.add_argument("workload_dir", type=Path, help="The directory containing the generated workloads")
    parser.add_argument("-b", "--benchmarks", nargs="+", choices=["all",] + list(RUNNERS.keys()),
                        default=["all"], type=str, help="The benchmarks to run")
    parser.add_argument("--quick", action="store_true",
                        help="One memory budget per workload, for checking the pipeline works")

    args = parser.parse_args()
    QUICK = args.quick or os.environ.get("SUBLIME_QUICK") == "1"
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
    except Exception:
        # Keep whatever finished. This used to `shutil.rmtree(output_prefix)`,
        # which threw away every completed benchmark because a later one raised
        # -- on a run of this length that is hours of work destroyed by a
        # KeyError, and the results of the figures that *did* finish are still
        # worth having. It also used to swallow the exception and exit 0, so
        # `evaluate.sh` carried on to the plotter and drew stale results.
        traceback.print_exc()
        print(f"\nBenchmarks failed. What completed is kept in {output_prefix}")
        sys.exit(1)

