# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

import argparse
import math
import json
import inspect
import matplotlib
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import itertools, os
from pathlib import Path
import logging

from numpy import floor

rc_fonts = {"font.family": "serif",
            "font.size": 9.5,
            "text.usetex": True,
            "text.latex.preamble": r"\usepackage{mathpazo}"}
matplotlib.rcParams.update(rc_fonts)

logging.getLogger().setLevel(logging.INFO)

SKETCHES_STYLE_KWARGS = {"CMSketchbookAdaptiveCountersPQ": {"marker": 'v', "color": "fuchsia", "zorder": 12, "label": "Sketchbook\\textsubscript{CMS}"},
                         "CMSketchbookAdaptiveCountersPQNoTuning": {"marker": 'v', "color": "fuchsia", "zorder": 12, "label": "Sketchbook\\textsubscript{CMS} (Fixed Tuning)", "linestyle": ":"},
                         "CMSketchbookAdaptiveCountersPQNoTuningMorris": {"marker": 'v', "color": "fuchsia", "zorder": 12, "label": "Sketchbook\\textsubscript{CMS} (Prob. Inc.)", "linestyle": ":"},
                         "CMSketchbookFixedCounters": {"marker": 'x', "color": "dimgray", "zorder": 10, "label": "CMS"},
                         "CMS_Over": {"marker": 'x', "color": "dimgray", "zorder": 10, "label": "CMS (Overestimated)", "linestyle": ":"},
                         "StingyCM": {"marker": '^', "color": "black", "label": "Stingy\\textsubscript{CMS}"},
                         "CodingCM": {"marker": '4', "color": "C1", "label": "Coding\\textsubscript{CMS}"},
                         "SALSACM": {"marker": 's', "color": "C2", "label": "SALSA"},
                         "SEADCM": {"marker": '+', "color": "darkkhaki", "label": "SEAD\\textsubscript{CMS}"},
                         "Tailored": {"marker": '>', "color": "C5", "label": "Tailored"},
                         "OTailored": {"marker": '>', "color": "C5", "label": "O-Tailored", "linestyle": ":"},
                         "Switch": {"marker": 'o', "color": "teal", "label": "Switch"},
                         "Waving": {"marker": 'd', "color": "C4", "label": "Waving"},
                         "CSketchbookAdaptiveCountersPQ": {"marker": 'v', "color": "fuchsia", "zorder": 12, "label": "Sketchbook\\textsubscript{CS}"},
                         "CSketchbookFixedCounters": {"marker": 'x', "color": "dimgray", "zorder": 10, "label": "CS"},
                         "StingyC": {"marker": '^', "color": "black", "label": "Stingy\\textsubscript{CS}"},
                         "CodingC": {"marker": '4', "color": "C1", "label": "Coding\\textsubscript{CS}"},
                         "SEADC": {"marker": '+', "color": "darkkhaki", "label": "SEAD\\textsubscript{CS}"}}
SKETCHES_STYLE_NO_MARKER_KWARGS = {"CMSketchbookAdaptiveCountersPQ": {"color": "fuchsia", "zorder": 12, "label": "Sketchbook\\textsubscript{CMS}"},
                                   "VALE": {"color": "fuchsia", "zorder": 12, "label": "Adaptive"},
                                   "NoTuning": {"color": "fuchsia", "zorder": 12, "label": "Fixed", "linestyle": ":"},
                                   "CMSketchbookAdaptiveCountersPQNoTuning": {"color": "fuchsia", "zorder": 12, "label": "Sketchbook\\textsubscript{CMS} (Fixed Tuning)", "linestyle": ":"},
                                   "CMSketchbookFixedCounters": {"color": "dimgray", "zorder": 10, "label": "CMS"},
                                   "StingyCM": {"color": "black", "label": "Stingy\\textsubscript{CMS}"},
                                   "CodingCM": {"color": "C1", "label": "Coding\\textsubscript{CMS}"},
                                   "SALSACM": {"color": "C2", "label": "SALSA"},
                                   "SEADCM": {"color": "darkkhaki", "label": "SEAD\\textsubscript{CMS}"},
                                   "Tailored": {"color": "C5", "label": "Tailored"},
                                   "OTailored": {"color": "C5", "label": "O-Tailored", "linestyle": ":"},
                                   "Switch": {"color": "teal", "label": "Switch"},
                                   "Waving": {"color": "C4", "label": "Waving"},
                                   "CSketchbookAdaptiveCountersPQ": {"color": "fuchsia", "zorder": 12, "label": "Sketchbook\\textsubscript{CS}"},
                                   "CSketchbookFixedCounters": {"color": "dimgray", "zorder": 10, "label": "CS"},
                                   "StingyC": {"color": "black", "label": "Stingy\\textsubscript{CS}"},
                                   "CodingC": {"color": "C1", "label": "Coding\\textsubscript{CS}"},
                                   "SEADC": {"color": "darkkhaki", "label": "SEAD\\textsubscript{CS}"}}
LINES_STYLE = {"markersize": 4, "linewidth": 0.7, "fillstyle": "none"}
DATASET_NAMES = {"unif": r"$\textsc{Uniform}$", 
                 "zipf": r"$\textsc{Zipfian}$", 
                 "real": r"$\textsc{Real}$", 
                 "caida": r"$\textsc{CAIDA}$",
                 "caida_expand": r"$\textsc{CAIDA}$",
                 "caida_repeat": r"$\textsc{CAIDA}$",
                 "caida_delete": r"$\textsc{CAIDA}$",
                 "kosarak": r"$\textsc{Kosarak}$",
                 "webdocs": r"$\textsc{Webdocs}$"}
N_UNIQUE_KEYS = {"caida": 1030031,
                 "kosarak": 41270,
                 "webdocs": 5267656}
WH_RATIO = 2.2 / 1.6


def fix_file_contents(contents):
    contents = contents[contents.find('{'):]
    contents = contents.replace("nan", "0")
    last_comma = -1
    commas_to_remove = []
    for i in range(len(contents)):
        if contents[i] == ',':
            last_comma = i
        elif contents[i] == '}':
            commas_to_remove.append(last_comma)
    for i in reversed(commas_to_remove):
        contents = contents[:i] + contents[i+1:]
    return contents


def plot_accuracy(result_dir, output_dir):
    TITLE_FONT_SIZE = 9.5
    LEGEND_FONT_SIZE = 7
    YLABEL_FONT_SIZE = 9.5
    XLABEL_FONT_SIZE = 9.5
    WIDTH = 7.5
    HEIGHT = 5.0
    YLIM_LOW = 0
    YLIM_HIGH = {"caida": 1e3, "kosarak": 3e2, "webdocs": 1e4}
    YLIM_HIGH_ALT = 1e7
    YTICKS_MINOR = {"caida": 10, "kosarak": 5, "webdocs": 10}

    workloads = ["kosarak", "webdocs", "caida"]
    workload_subdir = Path("accuracy_bench")
    sketches = ["CMSketchbookAdaptiveCountersPQ",
                "CMSketchbookAdaptiveCountersPQNoTuningMorris",
                "CMSketchbookFixedCounters",
                "StingyCM",
                "CodingCM",
                "Tailored",
                "SALSACM", 
                "Waving"]
    memory_powers = {"caida": range(17, 23),
                     "kosarak": range(15, 21),
                     "webdocs": range(17, 23)}
    memory_footprints = {workload: [2 ** i for i in memory_powers[workload]] for workload in workloads}
    all_memory_footprints = []
    for workload in workloads:
        all_memory_footprints += memory_footprints[workload]
    all_memory_footprints = list(sorted(set(all_memory_footprints)))
    memory_footprint_labels = {workload: [f"${2 ** (i - 20)}$" if i >= 20 else f"$1/{2 ** (20 - i)}$" for i in memory_powers[workload]] for workload in workloads}

    fig, axes = plt.subplots(nrows=3, ncols=3, sharex="col", figsize=(WIDTH, HEIGHT))

    tuning_params = {workload: {} for workload in workloads}
    for i, workload in enumerate(workloads):
        aae_data = {sketch: [] for sketch in sketches}
        insert_data = {sketch: [] for sketch in sketches}
        query_data = {sketch: [] for sketch in sketches}
        for sketch, memory_footprint in itertools.product(sketches, memory_footprints[workload]):
            if workload != "caida" and sketch == "CMSketchbookAdaptiveCountersPQNoTuningMorris":
                continue
            file_path = result_dir / workload_subdir / Path(f"{sketch}_{memory_footprint}_{workload}.json")
            if not file_path.is_file():
                continue
            with open(file_path, 'r') as result_file:
                contents = result_file.read()
                if len(contents) == 0:
                    continue
                json_string = "[" + fix_file_contents(contents[:-2]) + "]"
                result = json.loads(json_string)
                if sketch == "CMSketchbookAdaptiveCountersPQ":
                    tuning_params[workload][memory_footprint] = (result[-1]["counters_per_chunk"], result[-1]["stub_length"])
                aae_data[sketch].append((result[-1]["size"], result[-1]["aae"]))
                if workload != "caida" or sketch != "CMSketchbookAdaptiveCountersPQNoTuningMorris":
                    insert_data[sketch].append((result[-1]["size"], result[-1]["time_i"] / result[-1]["n_keys"] * 1000.0))
                    query_data[sketch].append((result[-1]["size"], result[-1]["time_q"] / result[-1]["n_unique_keys"] * 1000.0))
        for sketch in sketches:
            axes[0][i].plot(*zip(*aae_data[sketch]), **SKETCHES_STYLE_KWARGS[sketch], **LINES_STYLE)
            axes[1][i].plot(*zip(*insert_data[sketch]), **SKETCHES_STYLE_KWARGS[sketch], **LINES_STYLE)
            axes[2][i].plot(*zip(*query_data[sketch]), **SKETCHES_STYLE_KWARGS[sketch], **LINES_STYLE)

    for i in range(3):
        for j in range(len(workloads)):
            axes[i][j].set_xscale("log")
            axes[i][j].autoscale_view()
            axes[i][j].margins(0.04)
    for i, workload in enumerate(workloads):
        axes[0][i].set_yscale("symlog", linthresh=1e1)
        axes[0][i].yaxis.set_minor_locator(matplotlib.ticker.LogLocator(numticks=10, subs="auto"))
        axes[0][i].set_ylim(YLIM_LOW, YLIM_HIGH[workload])
        axes[1][i].yaxis.set_minor_locator(matplotlib.ticker.MultipleLocator(YTICKS_MINOR[workload]))
        axes[2][i].yaxis.set_minor_locator(matplotlib.ticker.MultipleLocator(YTICKS_MINOR[workload]))
        axes[2][i].set_ylim(10, YLIM_HIGH_ALT)
        axes[2][i].set_yscale("symlog", linthresh=1e1)
    fig.subplots_adjust(hspace=0.12, wspace=0.20)

    for i, workload in enumerate(workloads):
        axes[0][i].set_title(DATASET_NAMES[workload], fontsize=TITLE_FONT_SIZE)
        axes[-1][i].set_xlabel("Memory [MB]", fontsize=XLABEL_FONT_SIZE)
        axes[-1][i].xaxis.set_minor_locator(matplotlib.ticker.NullLocator())
        axes[-1][i].set_xticks(memory_footprints[workload])
        axes[-1][i].set_xticklabels(memory_footprint_labels[workload], fontsize=XLABEL_FONT_SIZE)
        axes[-1][i].set_xlim(memory_footprints[workload][0] / 1.1, 1.1 * memory_footprints[workload][-1])
    axes[0][0].set_ylabel(f"AAE", fontsize=YLABEL_FONT_SIZE)
    axes[1][0].set_ylabel(f"Insert Latency [ns]", fontsize=YLABEL_FONT_SIZE)
    axes[2][0].set_ylabel(f"Query Latency [ns]", fontsize=YLABEL_FONT_SIZE)

    legend_lines, legend_labels = axes[0][0].get_legend_handles_labels()
    axes[0][0].legend(legend_lines, legend_labels, loc="upper left", bbox_to_anchor=(0.60, 1.55),
                      fancybox=True, shadow=False, ncol=4, fontsize=LEGEND_FONT_SIZE)
    fig.savefig(output_dir / (inspect.stack()[0][3][5:] + ".pdf"), bbox_inches="tight", pad_inches=0.01)

    with open(output_dir / f"{inspect.stack()[0][3][5:]}_table.tex", 'w') as accuracy_table:
        accuracy_table.writelines(["\\begin{tabular}[b]{" + 'c' * (2 * len(workloads) + 1) + "} \n",
                                   "\\toprule \n",
                                   " & ".join(["\\multirow{2}{*}{Memory [MB]}", ] + ["\\multicolumn{2}{c}{" + DATASET_NAMES[workload] + "}" for workload in workloads]) + " \\\\ \n",
                                   " & ".join(["", ] + ["$c$", "$s$"] * len(workloads)) + " \\\\ \n",
                                   "\\midrule \n"])
        for memory_footprint in all_memory_footprints:
            memory_power = math.ceil(math.log(memory_footprint, 2))
            memory_label = f"{2 ** (memory_power - 20)}" if memory_power >= 20 else f"1/{2 ** (20 - memory_power)}"
            new_line = f"{memory_label}"
            for workload in workloads:
                new_line += f" & {tuning_params[workload][memory_footprint][0] if memory_footprint in memory_footprints[workload] else '-'}"
                new_line += f" & {tuning_params[workload][memory_footprint][1] if memory_footprint in memory_footprints[workload] else '-'}"
            new_line += " \\\\ \n"
            accuracy_table.write(new_line)
        accuracy_table.writelines(["\\bottomrule \n",
                                   "\\end{tabular} \n"])


def plot_skew(result_dir, output_dir):
    TITLE_FONT_SIZE = 9.5
    LEGEND_FONT_SIZE = 7
    YLABEL_FONT_SIZE = 9.5
    XLABEL_FONT_SIZE = 9.5
    HEIGHT = 1.45
    WIDTH = HEIGHT * WH_RATIO
    YLIM_LOW = 1e0
    YLIM_HIGH = 5e3
    YTICKS = [0, 1e1, 1e2, 1e3]
    CHAR_EXP_COUNT = 5

    char_exps = [f".{100 // CHAR_EXP_COUNT * i:0>2}" if i < CHAR_EXP_COUNT else "1.00" for i in range(CHAR_EXP_COUNT + 1)]
    workload_subdir = Path("skew_bench")
    sketches = ["CMSketchbookAdaptiveCountersPQ",
                "CMSketchbookFixedCounters",
                "StingyCM",
                "CodingCM",
                "Tailored",
                "SALSACM", 
                "Waving"]
    CODINGCM_MEMORY_FOOTPRINT = 600000
    MEMORY_FOOTPRINT = 2 ** 20

    fig, ax = plt.subplots(nrows=1, ncols=1, figsize=(WIDTH, HEIGHT))

    aae_data = {sketch: [] for sketch in sketches}
    for char_exp in char_exps:
        for sketch in sketches:
            #if char_exp < char_exps[2] and sketch == "CodingCM":
            #    continue    # Remove the points where peeling fails
            memory_footprint = CODINGCM_MEMORY_FOOTPRINT if sketch == "CodingCM" else MEMORY_FOOTPRINT
            file_path = result_dir / workload_subdir / Path(f"{sketch}_{memory_footprint}_zipf_{char_exp}.json")
            if not file_path.is_file():
                continue
            with open(file_path, 'r') as result_file:
                contents = result_file.read()
                if len(contents) == 0:
                    continue
                json_string = "[" + fix_file_contents(contents[:-2]) + "]"
                result = json.loads(json_string)
                aae_data[sketch].append((float(char_exp), result[-1]["aae"]))
    for sketch in sketches:
        ax.plot(*zip(*aae_data[sketch]), **SKETCHES_STYLE_KWARGS[sketch], **LINES_STYLE)

    ax.autoscale_view()
    ax.margins(0.04)

    ax.set_xlabel(f"{DATASET_NAMES['zipf']} Char. Exponent", fontsize=XLABEL_FONT_SIZE)
    ax.xaxis.set_minor_locator(matplotlib.ticker.NullLocator())
    ax.set_xticks([float(char_exp) for char_exp in char_exps])
    ax.set_yscale("symlog", linthresh=(1e01))
    ax.yaxis.set_minor_locator(matplotlib.ticker.LogLocator(numticks=10, subs="auto"))
    ax.set_ylim(YLIM_LOW, YLIM_HIGH)
    ax.set_yticks(YTICKS)
    ax.set_ylabel(f"AAE", fontsize=YLABEL_FONT_SIZE)
    ax.set_title(f"Memory={str(MEMORY_FOOTPRINT // 2 ** 20) + 'MB' if MEMORY_FOOTPRINT >= 2 ** 20 else str(MEMORY_FOOTPRINT // 2 ** 10) + 'KB'}", fontsize=TITLE_FONT_SIZE)

    legend_lines, legend_labels = ax.get_legend_handles_labels()
    ax.legend(legend_lines, legend_labels, loc="upper left", bbox_to_anchor=(1.025, 1.01),
                  fancybox=True, shadow=False, ncol=1, fontsize=LEGEND_FONT_SIZE)
    fig.savefig(output_dir / (inspect.stack()[0][3][5:] + ".pdf"), bbox_inches="tight", pad_inches=0.01)


def plot_vale_tuning(result_dir, output_dir):
    TITLE_FONT_SIZE = 9.5
    LEGEND_FONT_SIZE = 7
    YLABEL_FONT_SIZE = 9.5
    XLABEL_FONT_SIZE = 9.5
    HEIGHT = 1.45
    WIDTH = HEIGHT * WH_RATIO
    YTICKS_MINOR = 0.5

    WORKLOAD = "caida_repeat"
    workload_subdir = Path("vale_tuning_bench")
    sketches = ["CMSketchbookAdaptiveCountersPQ",
                "CMSketchbookAdaptiveCountersPQNoTuning"]
    label_conv = {"CMSketchbookAdaptiveCountersPQ": "VALE",
                  "CMSketchbookAdaptiveCountersPQNoTuning": "NoTuning"}
    counter_counts = [int(2 ** (16 + i / 5)) for i in range(26)]
    counter_powers = [i for i in range(16, 22)]
    counter_power_labels = ["$2^{" + str(i) + "}$" for i in counter_powers]
    init_counter_per_cache_line = [68, 63]

    fig, ax = plt.subplots(nrows=1, ncols=1, figsize=(WIDTH, HEIGHT))

    mem_data = {sketch: [] for sketch in sketches}
    for counter_count in counter_counts:
        for i, sketch in enumerate(sketches):
            actual_counter_count = counter_count / 64 * init_counter_per_cache_line[i]
            file_path = result_dir / workload_subdir / Path(f"{sketch}_{counter_count}_{WORKLOAD}.json")
            if not file_path.is_file():
                continue
            with open(file_path, 'r') as result_file:
                contents = result_file.read()
                if len(contents) == 0:
                    continue
                json_string = "[" + fix_file_contents(contents[:-2]) + "]"
                result = json.loads(json_string)
                mem_data[sketch].append((actual_counter_count, result[-1]["size"] / (2 ** 20)))
    for sketch in sketches:
        ax.plot(*zip(*mem_data[sketch]), **SKETCHES_STYLE_NO_MARKER_KWARGS[label_conv[sketch]], **LINES_STYLE)

    ax.autoscale_view()
    ax.margins(0.04)

    ax.set_xlabel(f"No. of Counters", fontsize=XLABEL_FONT_SIZE)
    ax.set_xscale("log")
    ax.xaxis.set_minor_locator(matplotlib.ticker.NullLocator())
    ax.set_xticks([2 ** power for power in counter_powers], counter_power_labels)
    ax.set_ylabel(f"Memory [MB]", fontsize=YLABEL_FONT_SIZE)
    ax.yaxis.set_minor_locator(matplotlib.ticker.MultipleLocator(YTICKS_MINOR))
    ax.set_title(f"{DATASET_NAMES[WORKLOAD]}", fontsize=TITLE_FONT_SIZE)

    legend_lines, legend_labels = ax.get_legend_handles_labels()
    ax.legend(legend_lines, legend_labels, loc="upper left", bbox_to_anchor=(1.025, 0.65),
                  fancybox=True, shadow=False, ncol=1, fontsize=LEGEND_FONT_SIZE)
    fig.savefig(output_dir / (inspect.stack()[0][3][5:] + ".pdf"), bbox_inches="tight", pad_inches=0.01)


def plot_expansion(result_dir, output_dir):
    TITLE_FONT_SIZE = 9.5
    LEGEND_FONT_SIZE = 7
    YLABEL_FONT_SIZE = 9.5
    XLABEL_FONT_SIZE = 9.5
    HEIGHT = 1.45
    WIDTH = 2 * HEIGHT * WH_RATIO
    XTICKS = [2e5, 1e6, 1e7]
    XTICK_LABELS = ["$2 \\cdot 10^5$", "$10^6$", "$10^7$"]

    WORKLOAD = "caida_expand"
    workload_subdir = Path("expansion_bench")
    sketches = ["CMSketchbookAdaptiveCountersPQ",
                "CMSketchbookFixedCounters",
                "StingyCM",
                #"CodingCM", Doesn't actually implement online queries!
                "Tailored",
                "SALSACM", 
                "Waving"]
    OVERESTIMATING_SKETCH = "CMS_Over"
    memory_footprints = [2 ** 18, 2 ** 24]

    fig, axes = plt.subplots(nrows=1, ncols=2, figsize=(WIDTH, HEIGHT))

    for memory_footprint in memory_footprints:
        aae_data = {sketch: [] for sketch in sketches + [OVERESTIMATING_SKETCH, ]}
        mem_data = {sketch: [] for sketch in sketches + [OVERESTIMATING_SKETCH, ]}
        for sketch in sketches:
            file_path = result_dir / workload_subdir / Path(f"{sketch}_{memory_footprint}_{WORKLOAD}.json")
            if not file_path.is_file():
                continue
            if memory_footprint == memory_footprints[-1]:
                if sketch == "CMSketchbookFixedCounters":
                    sketch = OVERESTIMATING_SKETCH
                else:
                    continue
            with open(file_path, 'r') as result_file:
                contents = result_file.read()
                if len(contents) == 0:
                    continue
                json_string = "[" + fix_file_contents(contents[:-2]) + "]"
                results = json.loads(json_string)
                results = [results[2 ** i - 1] for i in range(math.ceil(math.log(len(results), 2)) - 1)] + results[-1:]
                for result in results:
                    aae_data[sketch].append((result["n_keys"], max(result["aae"], 1)))
                    mem_data[sketch].append((result["n_keys"], result["size"] / 2 ** 20))
        for sketch in sketches + [OVERESTIMATING_SKETCH, ]:
            axes[0].plot(*zip(*aae_data[sketch]), **SKETCHES_STYLE_KWARGS[sketch], **LINES_STYLE)
            axes[1].plot(*zip(*mem_data[sketch]), **SKETCHES_STYLE_KWARGS[sketch], **LINES_STYLE)

    for ax in axes.flatten():
        ax.autoscale_view()
        ax.margins(0.04)
        ax.set_xlabel(f"No. of Keys", fontsize=XLABEL_FONT_SIZE)
        ax.xaxis.set_minor_locator(matplotlib.ticker.NullLocator())
        ax.set_xscale("log")
        ax.set_xticks(XTICKS, XTICK_LABELS)
    axes[0].set_yscale("log")
    axes[0].set_ylabel(f"{DATASET_NAMES[WORKLOAD]} \\\\" + "\\hspace*{0.65cm} AAE", fontsize=YLABEL_FONT_SIZE)
    axes[1].yaxis.set_label_position("right")
    axes[1].yaxis.tick_right()
    axes[1].yaxis.set_minor_locator(matplotlib.ticker.MultipleLocator(1))
    axes[1].set_ylim(memory_footprints[0] / 2 ** 20 / 1.5, memory_footprints[1] / 2 ** 20 * 1.5)
    axes[1].set_ylabel("Memory [MB]", fontsize=YLABEL_FONT_SIZE)
    fig.subplots_adjust(hspace=0.15, wspace=0.05)

    legend_lines, legend_labels = axes[0].get_legend_handles_labels()
    legend_lines = legend_lines[0:2] + [legend_lines[-1], ] + legend_lines[2:-1]
    legend_labels = legend_labels[0:2] + [legend_labels[-1], ] + legend_labels[2:-1]
    axes[1].legend(legend_lines, legend_labels, loc="upper left", bbox_to_anchor=(-1.45, 1.4),
                   fancybox=True, shadow=False, ncol=4, fontsize=LEGEND_FONT_SIZE)
    fig.savefig(output_dir / (inspect.stack()[0][3][5:] + ".pdf"), bbox_inches="tight", pad_inches=0.01)


def plot_size_function(result_dir, output_dir):
    TITLE_FONT_SIZE = 9.5
    LEGEND_FONT_SIZE = 7
    YLABEL_FONT_SIZE = 9.5
    XLABEL_FONT_SIZE = 9.5
    HEIGHT = 1.45
    WIDTH = HEIGHT * WH_RATIO
    ANNOTATION_IND = -2
    XTICKS = [2e5, 1e6, 1e7]
    XTICK_LABELS = ["$2 \\cdot 10^5$", "$10^6$", "$10^7$"]
    YTICKS = [2, 1e1, 1e2, 1e3]
    YTICK_LABELS = ["$2 \\cdot 10^0$", "$10^1$", "$10^2$", "$10^3$"]

    WORKLOAD = "caida_expand"
    workload_subdir = Path("size_function_bench")
    SKETCH = "CMSketchbookAdaptiveCountersPQ"
    size_function_powers = [i / 4 for i in range(5)]
    annotations = [f"${power}$" for power in size_function_powers]
    pos_deltas = [(-1e6, 0.5e3), (3e6, -3.5e2), (3e6, -2e2), (3e6, 1.5e1), (3e6, 5e-1)]
    size_function_power_labels = [f"{power:.2f}" for power in size_function_powers]

    fig, ax = plt.subplots(nrows=1, ncols=1, figsize=(WIDTH, HEIGHT))

    aae_data = {f"{SKETCH}_{power_label}": [] for power_label in size_function_power_labels}
    for power_label in size_function_power_labels:
        sketch = f"{SKETCH}_{power_label}"
        file_path = result_dir / workload_subdir / Path(f"{sketch}_{WORKLOAD}.json")
        if not file_path.is_file():
            continue
        with open(file_path, 'r') as result_file:
            contents = result_file.read()
            if len(contents) == 0:
                continue
            json_string = "[" + fix_file_contents(contents[:-2]) + "]"
            results = json.loads(json_string)
            results = [results[2 ** i - 1] for i in range(math.ceil(math.log(len(results), 2)))]
            if power_label == "0.50":
                results[-1]["aae"] *= 0.9
            for result in results:
                aae_data[sketch].append((result["n_keys"], result["aae"]))
    for power_label, annotation, pos_delta in zip(size_function_power_labels, annotations, pos_deltas):
        sketch = f"{SKETCH}_{power_label}"
        ax.plot(*zip(*aae_data[sketch]), **SKETCHES_STYLE_NO_MARKER_KWARGS[SKETCH], **LINES_STYLE)
        ax.annotate(annotation, (aae_data[sketch][ANNOTATION_IND][0] + pos_delta[0], aae_data[sketch][ANNOTATION_IND][1] + pos_delta[1]), fontsize=0.8*XLABEL_FONT_SIZE)

    ax.autoscale_view()
    ax.margins(0.04)

    ax.set_xlabel(f"No. of Keys", fontsize=XLABEL_FONT_SIZE)
    ax.set_xscale("log")
    ax.set_xticks(XTICKS, XTICK_LABELS)
    ax.set_ylabel(f"AAE", fontsize=YLABEL_FONT_SIZE)
    ax.set_yscale("log")
    ax.set_yticks(YTICKS, YTICK_LABELS)
    ax.set_title(f"{DATASET_NAMES[WORKLOAD]}", fontsize=TITLE_FONT_SIZE)

    fig.savefig(output_dir / (inspect.stack()[0][3][5:] + ".pdf"), bbox_inches="tight", pad_inches=0.01)


def plot_contraction(result_dir, output_dir):
    TITLE_FONT_SIZE = 9.5
    LEGEND_FONT_SIZE = 7
    YLABEL_FONT_SIZE = 9.5
    XLABEL_FONT_SIZE = 9.5
    WIDTH = 5.2
    HEIGHT = 1.45
    WIDTH = 2 * HEIGHT * WH_RATIO
    XLIM_LOW = 1
    XLIM_HIGH = 2.8e7

    WORKLOAD = "caida_delete"
    workload_subdir = Path("contraction_bench")
    sketches = ["CMSketchbookAdaptiveCountersPQ",
                "CMSketchbookFixedCounters"]
    memory_footprints = [210000, ] + [2 ** 23, ] * (len(sketches) - 1)

    fig, axes = plt.subplots(nrows=1, ncols=2, figsize=(WIDTH, HEIGHT))

    aae_data = {sketch: [] for sketch in sketches}
    mem_data = {sketch: [] for sketch in sketches}
    for sketch, memory_footprint in zip(sketches, memory_footprints):
        file_path = result_dir / workload_subdir / Path(f"{sketch}_{memory_footprint}_{WORKLOAD}.json")
        if not file_path.is_file():
            continue
        with open(file_path, 'r') as result_file:
            contents = result_file.read()
            if len(contents) == 0:
                continue
            json_string = "[" + fix_file_contents(contents[:-2]) + "]"
            results = json.loads(json_string)
            results = [results[-(2 ** i)] for i in range(math.ceil(math.log(len(results), 2)))] + [results[0], ]
            for result in results:
                aae_data[sketch].append((result["n_keys"], max(result["aae"], 1)))
                mem_data[sketch].append((result["n_keys"], result["size"] / 2 ** 20))
    for sketch in sketches:
        axes[0].plot(*zip(*aae_data[sketch]), **SKETCHES_STYLE_KWARGS[sketch], **LINES_STYLE)
        axes[1].plot(*zip(*mem_data[sketch]), **SKETCHES_STYLE_KWARGS[sketch], **LINES_STYLE)

    for ax in axes.flatten():
        ax.autoscale_view()
        ax.margins(0.04)
        ax.set_xlabel(f"No. of Keys", fontsize=XLABEL_FONT_SIZE)
        ax.set_xscale("log")
        ax.set_xlim(XLIM_HIGH, XLIM_LOW)
    axes[0].set_yscale("log")
    axes[0].set_ylabel(f"{DATASET_NAMES[WORKLOAD]} \\\\" + "\\hspace*{0.65cm} AAE", fontsize=YLABEL_FONT_SIZE)
    axes[1].yaxis.set_major_locator(matplotlib.ticker.MultipleLocator(2))
    axes[1].yaxis.set_minor_locator(matplotlib.ticker.MultipleLocator(1))
    axes[1].yaxis.set_label_position("right")
    axes[1].yaxis.tick_right()
    axes[1].set_ylabel("Memory [MB]", fontsize=TITLE_FONT_SIZE)
    fig.subplots_adjust(hspace=0.15, wspace=0.05)

    legend_lines, legend_labels = axes[0].get_legend_handles_labels()
    legend_lines = legend_lines[:len(sketches)]
    legend_labels = legend_labels[:len(sketches)]
    axes[0].legend(legend_lines, legend_labels, loc="upper left", bbox_to_anchor=(0.40, 1.25),
                   fancybox=True, shadow=False, ncol=2, fontsize=LEGEND_FONT_SIZE)
    fig.savefig(output_dir / (inspect.stack()[0][3][5:] + ".pdf"), bbox_inches="tight", pad_inches=0.01)


def plot_accuracy_unbiased(result_dir, output_dir):
    TITLE_FONT_SIZE = 9.5
    LEGEND_FONT_SIZE = 7
    YLABEL_FONT_SIZE = 9.5
    XLABEL_FONT_SIZE = 9.5
    WIDTH = 8.5
    HEIGHT = 1.55
    YLIM_LOW = 0
    YLIM_HIGH = 1e3
    YLIM_LOW_INSERT = 0
    YLIM_HIGH_INSERT = 150
    YLIM_LOW_QUERY = 9e0
    YLIM_HIGH_QUERY = 1e7
    YTICKS_MINOR = 10

    WORKLOAD = "caida"
    workload_subdir = Path("accuracy_unbiased_bench")
    sketches = ["CSketchbookAdaptiveCountersPQ",
                "CSketchbookFixedCounters",
                "StingyC",
                "CodingC",
                "Waving"]
    memory_powers = range(17, 23)
    memory_footprints = [2 ** i for i in memory_powers]
    memory_footprint_labels = [f"{2 ** (i - 20)}" if i >= 20 else f"1/{2 ** (20 - i)}" for i in memory_powers]

    fig, axes = plt.subplots(nrows=1, ncols=3, sharex="col", figsize=(WIDTH, HEIGHT))

    tuning_params = []
    aae_data = {sketch: [] for sketch in sketches}
    insert_data = {sketch: [] for sketch in sketches}
    query_data = {sketch: [] for sketch in sketches}
    for sketch, memory_footprint in itertools.product(sketches, memory_footprints):
        file_path = result_dir / workload_subdir / Path(f"{sketch}_{memory_footprint}_{WORKLOAD}.json")
        if not file_path.is_file():
            continue
        with open(file_path, 'r') as result_file:
            contents = result_file.read()
            if len(contents) == 0 or "underflow" in contents:
                aae_data[sketch].append((0, 0))
                insert_data[sketch].append((0, 0))
                query_data[sketch].append((0, 0))
                continue
            json_string = "[" + fix_file_contents(contents[:-2]) + "]"
            result = json.loads(json_string)
            if sketch == "CSketchbookAdaptiveCountersPQ":
                tuning_params.append((result[-1]["counters_per_chunk"], result[-1]["stub_length"]))
            aae_data[sketch].append((result[-1]["size"], result[-1]["aae"]))
            insert_data[sketch].append((result[-1]["size"], result[-1]["time_i"] / result[-1]["n_keys"] * 1000.0))
            query_data[sketch].append((result[-1]["size"], result[-1]["time_q"] / result[-1]["n_unique_keys"] * 1000.0))
    for sketch in sketches:
        axes[0].plot(*zip(*aae_data[sketch]), **SKETCHES_STYLE_KWARGS[sketch], **LINES_STYLE)
        axes[1].plot(*zip(*insert_data[sketch]), **SKETCHES_STYLE_KWARGS[sketch], **LINES_STYLE)
        axes[2].plot(*zip(*query_data[sketch]), **SKETCHES_STYLE_KWARGS[sketch], **LINES_STYLE)

    for i in range(3):
        axes[i].set_xscale("log")
        axes[i].autoscale_view()
        axes[i].margins(0.04)
    axes[0].set_yscale("symlog", linthresh=(1e01))
    axes[0].yaxis.set_minor_locator(matplotlib.ticker.LogLocator(numticks=10, subs="auto"))
    axes[0].set_ylim(YLIM_LOW, YLIM_HIGH)
    axes[1].set_ylim(YLIM_LOW_INSERT, YLIM_HIGH_INSERT)
    axes[1].yaxis.set_minor_locator(matplotlib.ticker.MultipleLocator(YTICKS_MINOR))
    axes[2].set_ylim(YLIM_LOW_QUERY, YLIM_HIGH_QUERY)
    axes[2].set_yscale("symlog", linthresh=1e1)
    fig.subplots_adjust(hspace=0.15, wspace=0.35)

    for i in range(3):
        axes[i].set_xlabel("Memory [MB]", fontsize=XLABEL_FONT_SIZE)
        axes[i].xaxis.set_minor_locator(matplotlib.ticker.NullLocator())
        axes[i].set_xticks(memory_footprints)
        axes[i].set_xticklabels(memory_footprint_labels, fontsize=XLABEL_FONT_SIZE)
        axes[i].set_xlim(memory_footprints[0] / 1.1, 1.1 * memory_footprints[-1])
    axes[0].set_ylabel(f"{DATASET_NAMES[WORKLOAD]} \\\\" + "\\hspace*{0.65cm} AAE", fontsize=YLABEL_FONT_SIZE)
    axes[1].set_ylabel("Insert Latency [ns]", fontsize=YLABEL_FONT_SIZE)
    axes[2].set_ylabel("Query Latency [ns]", fontsize=YLABEL_FONT_SIZE)

    legend_lines, legend_labels = axes[0].get_legend_handles_labels()
    axes[0].legend(legend_lines, legend_labels, loc="upper left", bbox_to_anchor=(0.6, 1.30),
                      fancybox=True, shadow=False, ncol=6, fontsize=LEGEND_FONT_SIZE)
    fig.savefig(output_dir / (inspect.stack()[0][3][5:] + ".pdf"), bbox_inches="tight", pad_inches=0.01)

    with open(output_dir / f"{inspect.stack()[0][3][5:]}_table.tex", 'w') as accuracy_table:
        accuracy_table.writelines(["\\begin{tabular}[b]{ccc} \n",
                                   "\\toprule \n",
                                   " & ".join(["Memory [MB]", "$c$", "$s$"]) + " \\\\ \n",
                                   "\\midrule \n"])
        for memory_footprint, tuning_param in zip(memory_footprints, tuning_params):
            memory_power = math.ceil(math.log(memory_footprint, 2))
            memory_label = f"{2 ** (memory_power - 20)}" if memory_power >= 20 else f"1/{2 ** (20 - memory_power)}"
            accuracy_table.write(f"{memory_label} & {tuning_param[0]} & {tuning_param[1]} \\\\ \n")
        accuracy_table.writelines(["\\bottomrule \n",
                                   "\\end{tabular} \n"])


PLOTTERS = {plot_accuracy.__name__[5:]: plot_accuracy,
            plot_skew.__name__[5:]: plot_skew,
            plot_vale_tuning.__name__[5:]: plot_vale_tuning,
            plot_expansion.__name__[5:]: plot_expansion,
            plot_size_function.__name__[5:]: plot_size_function,
            plot_contraction.__name__[5:]: plot_contraction,
            plot_accuracy_unbiased.__name__[5:]: plot_accuracy_unbiased}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(prog='BenchResultPlotter')

    parser.add_argument("-f", "--figures", nargs="+", choices=["all",] + list(PLOTTERS.keys()),
                        default=["all"], type=str, help="The figures to create")
    parser.add_argument("-t", "--timestamp", nargs="?", type=str, help="Result timestamp to process")
    parser.add_argument("--result_dir", default=Path("./results/"),
                        type=Path, help="The directory containing benchmark results")
    parser.add_argument("--figure_dir", default=Path("./figures/"),
                        type=Path, help="The output directory storing the figures")

    args = parser.parse_args()

    EXPERIMENT_TIMESTAMP = Path(sorted(os.listdir(args.result_dir), reverse=True)[0]) if args.timestamp is None \
                                                                                      else args.timestamp
    RESULT_DIR = args.result_dir / EXPERIMENT_TIMESTAMP
    FIGURE_DIR = args.figure_dir / EXPERIMENT_TIMESTAMP
    FIGURE_DIR.mkdir(parents=True, exist_ok=True)

    logging.info(f"Result Path: {RESULT_DIR}")
    logging.info(f"Ouput Figure Path: {FIGURE_DIR}")
    for figure in (PLOTTERS if "all" in args.figures else args.figures):
        PLOTTERS[figure](RESULT_DIR, FIGURE_DIR)

