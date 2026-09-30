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

rc_fonts = {"font.family": "serif",
            "font.size": 10,
            "text.usetex": True,
            "text.latex.preamble": r"\usepackage{mathpazo}"}
matplotlib.rcParams.update(rc_fonts)

logging.getLogger().setLevel(logging.INFO)

SKETCHES_STYLE_KWARGS = {"SublimeCMS": {"marker": 'v', "color": "fuchsia", "zorder": 12, "label": "Sublime\\textsubscript{CMS}"},
                         "SublimeCMS_0.50": {"marker": 'v', "color": "fuchsia", "zorder": 12, "label": "Sublime\\textsubscript{CMS}"},
                         "SublimeCMS_0.75": {"marker": 'v', "color": "fuchsia", "zorder": 12, "label": "Sublime\\textsubscript{CMS} ($\\alpha=0.75$)", "linestyle": "--"},
                         "SublimeCMS_1.00": {"marker": 'v', "color": "fuchsia", "zorder": 12, "label": "Sublime\\textsubscript{CMS} ($\\alpha=1.0$)", "linestyle": ":"},
                         "SublimeCMS_fixed": {"marker": '^', "color": "fuchsia", "zorder": 12, "label": "Sublime\\textsubscript{CMS} (Fixed Size)"},
                         "SublimeCMS_expand": {"marker": 'v', "color": "fuchsia", "zorder": 12, "label": "Sublime\\textsubscript{CMS} (Expand)"},
                         "SublimeCMSNoTuning": {"marker": 'v', "color": "fuchsia", "zorder": 12, "label": "Sublime\\textsubscript{CMS} (Fixed Tuning)", "linestyle": ":"},
                         "SublimeCMSNoTuningMorris": {"marker": 'v', "color": "fuchsia", "zorder": 12, "label": "Sublime\\textsubscript{CMS} (Prob. Inc.)", "linestyle": ":"},
                         "VALE": {"marker": 'v', "color": "fuchsia", "zorder": 12, "label": "Adaptive"},
                         "NoTuning": {"marker": 'v', "color": "fuchsia", "zorder": 12, "label": "Fixed", "linestyle": ":"},
                         "l1SizeFunction": {"marker": 'v', "color": "fuchsia", "zorder": 12, "label": "No. of Keys"},
                         "l2SizeFunction": {"marker": 'v', "color": "fuchsia", "zorder": 12, "label": "Variance", "linestyle": ":"},
                         "SublimeMG": {"marker": 'v', "color": "fuchsia", "zorder": 12, "label": "Sublime\\textsubscript{MG}"},
                         "SublimeMG_error_0.50": {"marker": 'v', "color": "fuchsia", "zorder": 12, "label": "Sublime\\textsubscript{MG}", "linestyle": "-"},
                         "SublimeMG_error_0.75": {"marker": 'v', "color": "fuchsia", "zorder": 12, "linestyle": "--"},
                         "SublimeMG_error_1.00": {"marker": 'v', "color": "fuchsia", "zorder": 12, "linestyle": ":"},
                         "SublimeMG_total_0.50": {"marker": '^', "color": "purple", "zorder": 11, "label": "Sublime\\textsubscript{MG} (Stream Length)", "linestyle": "-"},
                         "SublimeMG_total_0.75": {"marker": '^', "color": "purple", "zorder": 11, "linestyle": "--"},
                         "SublimeMG_total_1.00": {"marker": '^', "color": "purple", "zorder": 11, "linestyle": ":"},
                         # Sublime_MG's two ways of applying the decrement: the sweep solid,
                         # the min tree dashed.
                         "SublimeMG_tree": {"marker": '^', "color": "purple", "zorder": 11, "label": "Sublime\\textsubscript{MG} (Segment Tree)"},
                         "MG": {"marker": 'x', "color": "dimgray", "zorder": 10, "label": "MG"},
                         "SpaceSaving": {"marker": 's', "color": "C2", "zorder": 9, "label": "Space-Saving"},
                         "CMS": {"marker": 'x', "color": "dimgray", "zorder": 10, "label": "CMS"},
                         "CMS_Over": {"marker": 'x', "color": "dimgray", "zorder": 10, "label": "CMS (Overestimated)", "linestyle": ":"},
                         "StingyCM": {"marker": '^', "color": "black", "label": "Stingy\\textsubscript{CMS}"},
                         "CodingCM": {"marker": '4', "color": "C1", "label": "Coding\\textsubscript{CMS}"},
                         "SALSACM": {"marker": 's', "color": "C2", "label": "SALSA"},
                         "SEADCM": {"marker": '+', "color": "darkkhaki", "label": "SEAD\\textsubscript{CMS}"},
                         "Tailored": {"marker": '>', "color": "C5", "label": "Tailored"},
                         "OTailored": {"marker": '>', "color": "C5", "label": "O-Tailored", "linestyle": ":"},
                         "Switch": {"marker": 'o', "color": "teal", "label": "Switch"},
                         "Waving": {"marker": 'd', "color": "C4", "label": "Waving"},
                         "SublimeCS": {"marker": 'v', "color": "fuchsia", "zorder": 12, "label": "Sublime\\textsubscript{CS}"},
                         "SublimeCS_fixed": {"marker": '^', "color": "fuchsia", "zorder": 12, "label": "Sublime\\textsubscript{CS} (Fixed Size)", "linestyle": ":"},
                         "SublimeCS_expand": {"marker": 'v', "color": "fuchsia", "zorder": 12, "label": "Sublime\\textsubscript{CS} (Expand)", "linestyle": ":"},
                         "CS": {"marker": 'x', "color": "dimgray", "zorder": 10, "label": "CS"},
                         "StingyC": {"marker": '^', "color": "black", "label": "Stingy\\textsubscript{CS}"},
                         "CodingC": {"marker": '4', "color": "C1", "label": "Coding\\textsubscript{CS}"},
                         "SEADC": {"marker": '+', "color": "darkkhaki", "label": "SEAD\\textsubscript{CS}"},
                         "SublimeCSDotted": {"marker": 'v', "color": "fuchsia", "zorder": 12, "label": "Sublime\\textsubscript{CS}", "linestyle": ":"},
                         "CSDotted": {"marker": 'x', "color": "dimgray", "zorder": 10, "label": "CS", "linestyle": ":"},
                         "StingyCDotted": {"marker": '^', "color": "black", "label": "Stingy\\textsubscript{CS}", "linestyle": ":"}}
SKETCHES_STYLE_NO_MARKER_KWARGS = {"SublimeCMS": {"color": "fuchsia", "zorder": 12, "label": "Sublime\\textsubscript{CMS}"},
                                   "SublimeCMSNoTuning": {"color": "fuchsia", "zorder": 12, "label": "Sublime\\textsubscript{CMS} (Fixed Tuning)", "linestyle": ":"},
                                   "CMS": {"color": "dimgray", "zorder": 10, "label": "CMS"},
                                   "StingyCM": {"color": "black", "label": "Stingy\\textsubscript{CMS}"},
                                   "CodingCM": {"color": "C1", "label": "Coding\\textsubscript{CMS}"},
                                   "SALSACM": {"color": "C2", "label": "SALSA"},
                                   "SEADCM": {"color": "darkkhaki", "label": "SEAD\\textsubscript{CMS}"},
                                   "Tailored": {"color": "C5", "label": "Tailored"},
                                   "OTailored": {"color": "C5", "label": "O-Tailored", "linestyle": ":"},
                                   "Switch": {"color": "teal", "label": "Switch"},
                                   "Waving": {"color": "C4", "label": "Waving"},
                                   "SublimeCS": {"color": "fuchsia", "zorder": 12, "label": "Sublime\\textsubscript{CS}"},
                                   "CS": {"color": "dimgray", "zorder": 10, "label": "CS"},
                                   "StingyC": {"color": "black", "label": "Stingy\\textsubscript{CS}"},
                                   "CodingC": {"color": "C1", "label": "Coding\\textsubscript{CS}"},
                                   "SEADC": {"color": "darkkhaki", "label": "SEAD\\textsubscript{CS}"}}
LINES_STYLE = {"markersize": 4, "linewidth": 0.7, "fillstyle": "none"}
DATASET_NAMES = {"unif": r"$\textsc{Uniform}$", 
                 "zipf": r"$\textsc{Zipfian}$", 
                 "real": r"$\textsc{Real}$", 
                 "caida": r"$\textsc{CAIDA}$",
                 "caida_expand": r"$\textsc{CAIDA}$",
                 "webdocs_expand": r"$\textsc{WebDocs}$",
                 "caida_repeat": r"$\textsc{CAIDA}$",
                 "caida_delete": r"$\textsc{CAIDA}$",
                 "kosarak": r"$\textsc{Kosarak}$",
                 "webdocs": r"$\textsc{WebDocs}$"}
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
    TITLE_FONT_SIZE = 11
    LEGEND_FONT_SIZE = 11
    XLABEL_FONT_SIZE = 10
    YLABEL_FONT_SIZE = 11
    WIDTH = 7.75
    HEIGHT = 5.0
    YLIM_LOW = 0
    YLIM_HIGH = {"caida": 1e3, "kosarak": 3e2, "webdocs": 1e4}
    YLIM_LOW_QUERY = 1e1
    YLIM_HIGH_QUERY = 1.04e2
    YLIM_LOW_QUERY_OUTLIER = 1.8e6
    YLIM_HIGH_QUERY_OUTLIER = 1e8
    CUT_LINES_MULT = .5  # Proportion of vertical to horizontal extent of the cut lines
    CUT_LINES_D = .5  # Proportion of vertical to horizontal extent of the cut lines
    YTICKS_MINOR_INSERT = {"caida": 10, "kosarak": 5, "webdocs": 10}
    YTICKS_MINOR_QUERY = {"caida": 5, "kosarak": 5, "webdocs": 5}

    workloads = ["kosarak", "webdocs", "caida"]
    workload_subdir = Path("accuracy_bench")
    sketches = ["SublimeCMSNoTuning",
                "SublimeCMSNoTuningMorris",
                "CMS",
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

    fig, axes = plt.subplots(nrows=4, ncols=3, sharex="col", figsize=(WIDTH, HEIGHT),
                             gridspec_kw=dict(width_ratios=[1, 1, 1], height_ratios=[1, 1, 0.2, 0.75]))

    result_found = False
    tuning_params = {workload: {} for workload in workloads}
    for i, workload in enumerate(workloads):
        aae_data = {sketch: [] for sketch in sketches}
        insert_data = {sketch: [] for sketch in sketches}
        query_data = {sketch: [] for sketch in sketches}
        for sketch, memory_footprint in itertools.product(sketches, memory_footprints[workload]):
            if workload != "caida" and sketch == "SublimeCMSNoTuningMorris":
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
                if sketch == "SublimeCMSNoTuning":
                    tuning_params[workload][memory_footprint] = (0, 0)
                    #tuning_params[workload][memory_footprint] = (result[-1]["counters_per_chunk"], result[-1]["stub_length"])
                aae_data[sketch].append((result[-1]["size"], result[-1]["aae"]))
                if workload != "caida" or sketch != "SublimeCMSNoTuningMorris":
                    insert_data[sketch].append((result[-1]["size"], result[-1]["time_i"] / result[-1]["n_keys"] * 1000.0))
                    query_data[sketch].append((result[-1]["size"], result[-1]["time_q"] / result[-1]["n_unique_keys"] * 1000.0))
        if len(aae_data) == 0:
            axes[0][i].plot([0, 0], [0, 0])
            axes[2][i].plot([0, 0], [0, 0])
        for sketch in sketches:
            result_found = True
            sketch_style_kwargs = SKETCHES_STYLE_KWARGS[sketch] if sketch != "SublimeCMSNoTuning" else SKETCHES_STYLE_KWARGS["SublimeCMS"]
            axes[0][i].plot(*zip(*aae_data[sketch]), **sketch_style_kwargs, **LINES_STYLE)
            axes[1][i].plot(*zip(*insert_data[sketch]), **sketch_style_kwargs, **LINES_STYLE)
            axes[2][i].plot(*zip(*query_data[sketch]), **sketch_style_kwargs, **LINES_STYLE)
            axes[3][i].plot(*zip(*query_data[sketch]), **sketch_style_kwargs, **LINES_STYLE)
    if not result_found:
        logging.info(inspect.stack()[0][3][5:] + ": Figure not generated due to no benchmark results being found to include")
        return

    for i in range(3):
        for j in range(len(workloads)):
            axes[i][j].set_xscale("log")
            axes[i][j].autoscale_view()
            axes[i][j].margins(0.04)
    for i, workload in enumerate(workloads):
        axes[0][i].set_yscale("symlog", linthresh=1e1)
        axes[0][i].yaxis.set_minor_locator(matplotlib.ticker.LogLocator(numticks=10, subs="auto"))
        axes[0][i].set_ylim(YLIM_LOW, YLIM_HIGH[workload])
        axes[1][i].yaxis.set_minor_locator(matplotlib.ticker.MultipleLocator(YTICKS_MINOR_INSERT[workload]))
        axes[2][i].set_ylim(YLIM_LOW_QUERY_OUTLIER, YLIM_HIGH_QUERY_OUTLIER)
        axes[2][i].set_yscale("log")
        axes[3][i].yaxis.set_minor_locator(matplotlib.ticker.MultipleLocator(YTICKS_MINOR_QUERY[workload]))
        axes[3][i].set_ylim(YLIM_LOW_QUERY, YLIM_HIGH_QUERY)
    fig.subplots_adjust(hspace=0.12, wspace=0.25)

    for i, workload in enumerate(workloads):
        axes[0][i].set_title(DATASET_NAMES[workload], fontsize=TITLE_FONT_SIZE)
        axes[-1][i].set_xlabel("Memory [MB]", fontsize=XLABEL_FONT_SIZE)
        axes[-1][i].xaxis.set_minor_locator(matplotlib.ticker.NullLocator())
        axes[-1][i].set_xticks(memory_footprints[workload])
        axes[-1][i].set_xticklabels(memory_footprint_labels[workload], fontsize=XLABEL_FONT_SIZE)
        axes[-1][i].set_xlim(memory_footprints[workload][0] / 1.1, 1.1 * memory_footprints[workload][-1])
        axes[2][i].spines.bottom.set_visible(False)
        axes[2][i].get_xaxis().set_visible(False)
        axes[3][i].spines.top.set_visible(False)
        cut_line_kwargs = dict(marker=[(-1, -CUT_LINES_D), (1, CUT_LINES_D)], markersize=5,
                               linestyle="none", color='k', mec='k', mew=1, clip_on=False)
        axes[2][i].plot([0, 1], [0, 0], transform=axes[2][i].transAxes, **cut_line_kwargs)
        axes[3][i].plot([0, 1], [1, 1], transform=axes[3][i].transAxes, **cut_line_kwargs)
    axes[0][0].set_ylabel("AAE", fontsize=YLABEL_FONT_SIZE)
    axes[1][0].set_ylabel("Insert Latency [ns]", fontsize=YLABEL_FONT_SIZE)
    axes[1][0].yaxis.set_label_coords(-0.20, 0.10 * HEIGHT)
    axes[3][0].set_ylabel("Query Latency [ns]", fontsize=YLABEL_FONT_SIZE)
    axes[3][0].yaxis.set_label_coords(-0.20, 0.13 * HEIGHT)

    legend_lines, legend_labels = axes[0][2].get_legend_handles_labels()
    axes[0][2].legend(legend_lines, legend_labels, loc="upper left", bbox_to_anchor=(-2.75, 1.75),
                      fancybox=True, shadow=False, ncol=4, fontsize=LEGEND_FONT_SIZE, frameon=False)
    fig.savefig(output_dir / (inspect.stack()[0][3][5:] + "_(Fig_10).pdf"), bbox_inches="tight", pad_inches=0.01)

    with open(output_dir / f"{inspect.stack()[0][3][5:]}_table_(Fig_10).tex", 'w') as accuracy_table:
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


def plot_skew_vale_tuning(result_dir, output_dir):
    TITLE_FONT_SIZE = 13
    LEGEND_FONT_SIZE = 13
    YLABEL_FONT_SIZE = 13
    XLABEL_FONT_SIZE = 13
    HEIGHT = 1.6
    WH_RATIO = 2.50 / 1.6
    WIDTH = 2 * HEIGHT * WH_RATIO
    YTICKS_MINOR = 0.5
    YTICKS = [10 ** i for i in range(-2, 6)]
    MEMORY_YTICKS = [i for i in range(1, 6)]

    fig, axes = plt.subplots(nrows=1, ncols=2, figsize=(WIDTH, HEIGHT))

    char_exps = ["0.00", "0.20", "0.40", "0.80", "1.60", "3.20"]
    workload_subdir = Path("skew_bench")
    sketches = ["SublimeCMS",
                "CMS",
                "StingyCM",
                "Tailored",
                "SALSACM", 
                "Waving"]
    MEMORY_FOOTPRINT = 2 ** 20

    aae_data = {sketch: [] for sketch in sketches}
    for char_exp in char_exps:
        for sketch in sketches:
            file_path = result_dir / workload_subdir / Path(f"{sketch}_{MEMORY_FOOTPRINT}_zipf_{char_exp}.json")
            if not file_path.is_file():
                continue
            with open(file_path, 'r') as result_file:
                contents = result_file.read()
                if len(contents) == 0:
                    continue
                json_string = "[" + fix_file_contents(contents[:-2]) + "]"
                result = json.loads(json_string)
                x_pos = 0 if float(char_exp) == 0 else (math.log(float(char_exp) * 5, 2) + 1) * (3.2 / 4) ** 2
                aae_data[sketch].append((x_pos, max(0.01, result[-1]["aae"])))
    for sketch in sketches:
        axes[0].plot(*zip(*aae_data[sketch]), **SKETCHES_STYLE_KWARGS[sketch], **LINES_STYLE)

    workload_subdir = Path("vale_tuning_bench")
    sketches = ["SublimeCMS",
                "SublimeCMSNoTuning"]
    label_conv = {"SublimeCMS": "VALE",
                  "SublimeCMSNoTuning": "NoTuning"}
    COUNTER_COUNT = 2 ** 20

    mem_data = {sketch: [] for sketch in sketches}
    for char_exp in char_exps:
        for sketch in sketches:
            file_path = result_dir / workload_subdir / Path(f"{sketch}_{COUNTER_COUNT}_zipf_{char_exp}.json")
            if not file_path.is_file():
                continue
            with open(file_path, 'r') as result_file:
                contents = result_file.read()
                if len(contents) == 0:
                    continue
                json_string = "[" + fix_file_contents(contents[:-2]) + "]"
                result = json.loads(json_string)
                x_pos = 0 if float(char_exp) == 0 else (math.log(float(char_exp) * 5, 2) + 1) * (3.2 / 4) ** 2
                mem_data[sketch].append((x_pos, result[-1]["size"] / (2 ** 20)))
    for sketch in sketches:
        axes[1].plot(*zip(*mem_data[sketch]), **SKETCHES_STYLE_KWARGS[label_conv[sketch]], **LINES_STYLE)

    if len(aae_data) == 0 and len(mem_data) == 0:
        logging.info(inspect.stack()[0][3][5:] + ": Figure not generated due to no benchmark results being found to include")
        return

    fig.subplots_adjust(wspace=0.35)
    for ax in axes:
        ax.autoscale_view()
        ax.margins(0.04)
        ax.set_xlabel(f"{DATASET_NAMES['zipf']} Exponent", fontsize=XLABEL_FONT_SIZE)
        ax.set_xticks([3.2 * i / (len(char_exps) - 1) for i in range(len(char_exps))], [float(char_exp) for char_exp in char_exps])

    axes[0].text(0, 1e5, "_")

    axes[0].set_yscale("log")
    axes[0].yaxis.set_minor_locator(matplotlib.ticker.LogLocator(numticks=10, subs="auto"))
    axes[0].set_yticks(YTICKS)
    axes[0].set_ylabel("AAE", fontsize=YLABEL_FONT_SIZE)
    axes[0].text(0.05, 1.5e-2, f"Memory={str(MEMORY_FOOTPRINT // 2 ** 20) + 'MB' if MEMORY_FOOTPRINT >= 2 ** 20 else str(MEMORY_FOOTPRINT // 2 ** 10) + 'KB'}", fontsize=TITLE_FONT_SIZE - 1)
    axes[1].set_ylabel("Memory [MB]", fontsize=YLABEL_FONT_SIZE)
    axes[1].yaxis.set_minor_locator(matplotlib.ticker.MultipleLocator(YTICKS_MINOR))
    axes[1].set_yticks(MEMORY_YTICKS)

    legend_lines_skew, legend_labels_skew = axes[0].get_legend_handles_labels()
    legend_lines_vale_tuning, legend_labels_vale_tuning = axes[1].get_legend_handles_labels()
    legend_lines = legend_lines_skew + legend_lines_vale_tuning
    legend_labels = legend_labels_skew + legend_labels_vale_tuning
    axes[0].legend(legend_lines, legend_labels, loc="upper left", bbox_to_anchor=(2.35, 0.9),
                   fancybox=True, shadow=False, ncol=3, fontsize=LEGEND_FONT_SIZE, frameon=False)

    legend_sep = matplotlib.lines.Line2D([14.90, 14.90], [1.0e-1, 5.0e3], linestyle=':', color="grey")
    legend_sep.set_clip_on(False)
    axes[0].add_line(legend_sep)

    fig.savefig(output_dir / (inspect.stack()[0][3][5:] + "_(Fig_11).pdf"), bbox_inches="tight", pad_inches=0.01)


def plot_expansion(result_dir, output_dir):
    TITLE_FONT_SIZE = 10
    LEGEND_FONT_SIZE = 9
    YLABEL_FONT_SIZE = 10
    XLABEL_FONT_SIZE = 10
    HEIGHT = 1.45
    WH_RATIO = 2.65 / 1.6
    WIDTH = 2 * HEIGHT * WH_RATIO
    XTICKS = [5e6, 1e7, 1e8]
    XTICK_LABELS = ["$5 \\cdot 10^6$", "$10^7$", "$10^8$"]
    YTICKS = [10 ** i for i in range(1, 6)]
    MEMORY_YTICKS = [10 ** i for i in range(-4, 1)]

    WORKLOAD = "webdocs_expand"
    workload_subdir = Path("expansion_bench")
    sketches = ["SublimeCMS",
                "CMS",
                "StingyCM",
                #"CodingCM", Doesn't actually implement online queries!
                "Tailored",
                "SALSACM", 
                "Waving"]
    OVERESTIMATING_SKETCH = "CMS_Over"
    memory_footprints = [2 ** 15, 2 ** 24]
    size_function_powers = [0.5, 0.75, 1.0]
    sublime_power_names = [f"{sketches[0]}_{power:.2f}" for power in size_function_powers]

    fig, axes = plt.subplots(nrows=1, ncols=2, figsize=(WIDTH, HEIGHT))

    aae_data = {sketch: [] for sketch in sketches[1:] + [OVERESTIMATING_SKETCH, ] + sublime_power_names}
    mem_data = {sketch: [] for sketch in sketches[1:] + [OVERESTIMATING_SKETCH, ] + sublime_power_names}
    for sketch in sketches:
        memory_footprint = memory_footprints[-1] if sketch == OVERESTIMATING_SKETCH else memory_footprints[0]
        if sketch == "SublimeCMS":
            for power in size_function_powers:
                file_path = result_dir / workload_subdir / Path(f"{sketch}_{memory_footprint}_{power:.2f}_{WORKLOAD}.json")
                if not file_path.is_file():
                    continue
                with open(file_path, 'r') as result_file:
                    contents = result_file.read()
                    if len(contents) == 0:
                        continue
                    json_string = "[" + fix_file_contents(contents[:-2]) + "]"
                    results = json.loads(json_string)
                    results = [results[2 ** i - 1] for i in range(math.ceil(math.log(len(results), 2)) - 1)] + results[-1:]
                    for result in results:
                        aae_data[sketch + f"_{power:.2f}"].append((result["n_keys"], max(result["aae"], 1)))
                        mem_data[sketch + f"_{power:.2f}"].append((result["n_keys"], result["size"] / result["n_keys"]))
        else:
            sketch_alias = sketch if sketch != OVERESTIMATING_SKETCH else "CMS"
            file_path = result_dir / workload_subdir / Path(f"{sketch_alias}_{memory_footprint}_{WORKLOAD}.json")
            if not file_path.is_file():
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
                    mem_data[sketch].append((result["n_keys"], result["size"] / result["n_keys"]))
    if len(aae_data) == 0:
        logging.info(inspect.stack()[0][3][5:] + ": Figure not generated due to no benchmark results being found to include")
        return
    for sketch in sublime_power_names + [sketches[1]] + sketches[2:]:
        axes[0].plot(*zip(*aae_data[sketch]), **SKETCHES_STYLE_KWARGS[sketch], **LINES_STYLE)
        axes[1].plot(*zip(*mem_data[sketch]), **SKETCHES_STYLE_KWARGS[sketch], **LINES_STYLE)

    annotation_off_x = [1 / 1.77 for i in range(len(sublime_power_names))]
    annotation_off_y = [1 / 4.5, 1 / 3.4, 1 / 3]
    for i, (power, sketch) in enumerate(zip(size_function_powers, sublime_power_names)):
        axes[0].annotate(power, (aae_data[sketch][-1][0] * annotation_off_x[i], aae_data[sketch][-1][1] * annotation_off_y[i]), fontsize=0.8*XLABEL_FONT_SIZE)
    annotation_off_x = [1 / 2 for i in range(len(sublime_power_names))]
    annotation_off_y = [2, 1.1, 1.5]
    for i, (power, sketch) in enumerate(zip(size_function_powers, sublime_power_names)):
        axes[1].annotate(power, (mem_data[sketch][-1][0] * annotation_off_x[i], mem_data[sketch][-1][1] * annotation_off_y[i]), fontsize=0.8*XLABEL_FONT_SIZE)

    for ax in axes.flatten():
        ax.autoscale_view()
        ax.margins(0.04)
        ax.set_xlabel(f"No. of Keys", fontsize=XLABEL_FONT_SIZE)
        ax.xaxis.set_minor_locator(matplotlib.ticker.NullLocator())
        ax.set_xscale("log")
        #ax.set_xticks(XTICKS, XTICK_LABELS)
        ax.set_yscale("log")
        ax.yaxis.set_minor_locator(matplotlib.ticker.LogLocator(numticks=10, subs="auto"))
    axes[0].set_ylim(0.8 * YTICKS[0], 2 * YTICKS[-1])
    axes[0].set_yticks(YTICKS)
    axes[0].set_ylabel("AAE", fontsize=YLABEL_FONT_SIZE)
    axes[1].set_yticks(MEMORY_YTICKS)
    axes[1].set_ylabel("Memory [B/Key]", fontsize=YLABEL_FONT_SIZE)
    fig.subplots_adjust(hspace=0.15, wspace=0.45)

    legend_lines, legend_labels = axes[0].get_legend_handles_labels()
    legend_lines = [legend_lines[0], ] + legend_lines[3:]
    legend_labels = [legend_labels[0], ] + legend_labels[3:]
    axes[0].legend(legend_lines, legend_labels, loc="upper left", bbox_to_anchor=(-0.05, 1.5),
                   fancybox=True, shadow=False, ncol=3, fontsize=LEGEND_FONT_SIZE, frameon=False)
    fig.savefig(output_dir / (inspect.stack()[0][3][5:] + "_(Fig_12).pdf"), bbox_inches="tight", pad_inches=0.01)


def plot_contraction(result_dir, output_dir):
    TITLE_FONT_SIZE = 10
    LEGEND_FONT_SIZE = 9
    YLABEL_FONT_SIZE = 10
    XLABEL_FONT_SIZE = 10
    WIDTH = 5.2
    HEIGHT = 1.45
    WH_RATIO = 2.60 / 1.6
    WIDTH = 2 * HEIGHT * WH_RATIO
    XLIM_LOW = 0.5
    XLIM_HIGH = 4e7
    XTICKS = [1e6, 1e3, 1e0]
    MEMORY_YTICKS = [0, 2, 4]

    WORKLOAD = "caida_delete"
    workload_subdir = Path("contraction_bench")
    sketches = ["SublimeCMS",
                "CMS"]
    memory_footprints = [2 ** 22, 3947584]

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
            results = [results[-1], ] + [results[-(2 ** i)] for i in range(2, math.ceil(math.log(len(results), 2)))] + [results[0], ]
            for result in results:
                aae_data[sketch].append((max(result["n_keys"], 1), max(result["aae"], 0.1)))
                mem_data[sketch].append((max(result["n_keys"], 1), result["size"] / 2 ** 20))
    if len(aae_data) == 0:
        logging.info(inspect.stack()[0][3][5:] + ": Figure not generated due to no benchmark results being found to include")
        return
    for sketch in sketches:
        axes[0].plot(*zip(*aae_data[sketch]), **SKETCHES_STYLE_KWARGS[sketch], **LINES_STYLE)
        axes[1].plot(*zip(*mem_data[sketch]), **SKETCHES_STYLE_KWARGS[sketch], **LINES_STYLE)

    for ax in axes.flatten():
        ax.autoscale_view()
        ax.margins(0.04)
        ax.set_xlabel(f"No. of Keys", fontsize=XLABEL_FONT_SIZE)
        ax.set_xscale("log")
        ax.set_xlim(XLIM_HIGH, XLIM_LOW)
        ax.set_xticks(XTICKS)
    axes[0].set_yscale("log")
    axes[0].set_ylabel("AAE", fontsize=YLABEL_FONT_SIZE)
    axes[1].yaxis.set_major_locator(matplotlib.ticker.MultipleLocator(2))
    axes[1].yaxis.set_minor_locator(matplotlib.ticker.MultipleLocator(1))
    axes[1].set_yticks(MEMORY_YTICKS)
    axes[1].set_ylabel("Memory [MB]", fontsize=TITLE_FONT_SIZE)
    fig.subplots_adjust(hspace=0.15, wspace=0.45)

    legend_lines, legend_labels = axes[0].get_legend_handles_labels()
    legend_lines = legend_lines[:len(sketches)]
    legend_labels = legend_labels[:len(sketches)]
    axes[0].legend(legend_lines, legend_labels, loc="upper left", bbox_to_anchor=(0.375, 1.3),
                   fancybox=True, shadow=False, ncol=2, fontsize=LEGEND_FONT_SIZE, frameon=False)
    fig.savefig(output_dir / (inspect.stack()[0][3][5:] + "_(Fig_13).pdf"), bbox_inches="tight", pad_inches=0.01)


def plot_accuracy_unbiased(result_dir, output_dir):
    TITLE_FONT_SIZE = 10
    LEGEND_FONT_SIZE = 9
    YLABEL_FONT_SIZE = 10
    XLABEL_FONT_SIZE = 10
    HEIGHT = 1.55
    WH_RATIO = 2.75 / 1.6
    WIDTH = 3 * HEIGHT * WH_RATIO
    YLIM_LOW = 0
    YLIM_HIGH = 1e3
    YLIM_LOW_INSERT = 0
    YLIM_HIGH_INSERT = 150
    YLIM_LOW_QUERY = 9e0
    YLIM_HIGH_QUERY = 1e7
    YTICKS_MINOR = 10

    WORKLOAD = "caida"
    workload_subdir = Path("accuracy_unbiased_bench")
    sketches = ["SublimeCSNoTuning",
                "CS",
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
            if sketch == "SublimeCSNoTuning":
                tuning_params.append((result[-1]["counters_per_chunk"], result[-1]["stub_length"]))
            aae_data[sketch].append((result[-1]["size"], result[-1]["aae"]))
            insert_data[sketch].append((result[-1]["size"], result[-1]["time_i"] / result[-1]["n_keys"] * 1000.0))
            query_data[sketch].append((result[-1]["size"], result[-1]["time_q"] / result[-1]["n_unique_keys"] * 1000.0))
    if len(aae_data) == 0:
        logging.info(inspect.stack()[0][3][5:] + ": Figure not generated due to no benchmark results being found to include")
        return
    for sketch in sketches:
        sketch_style_kwargs = SKETCHES_STYLE_KWARGS[sketch] if sketch != "SublimeCSNoTuning" else SKETCHES_STYLE_KWARGS["SublimeCS"]
        axes[0].plot(*zip(*aae_data[sketch]), **sketch_style_kwargs, **LINES_STYLE)
        axes[1].plot(*zip(*insert_data[sketch]), **sketch_style_kwargs, **LINES_STYLE)
        axes[2].plot(*zip(*query_data[sketch]), **sketch_style_kwargs, **LINES_STYLE)

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
    axes[0].set_ylabel("AAE", fontsize=YLABEL_FONT_SIZE)
    axes[1].set_ylabel("Insert Latency [ns]", fontsize=YLABEL_FONT_SIZE)
    axes[2].set_ylabel("Query Latency [ns]", fontsize=YLABEL_FONT_SIZE)

    legend_lines, legend_labels = axes[0].get_legend_handles_labels()
    axes[0].legend(legend_lines, legend_labels, loc="upper left", bbox_to_anchor=(0.2, 1.33),
                      fancybox=True, shadow=False, ncol=6, fontsize=LEGEND_FONT_SIZE, frameon=False)
    fig.savefig(output_dir / (inspect.stack()[0][3][5:] + "_(Fig_14).pdf"), bbox_inches="tight", pad_inches=0.01)

    with open(output_dir / f"{inspect.stack()[0][3][5:]}_table_(Fig_14).tex", 'w') as accuracy_table:
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


def plot_l2_size_function(result_dir, output_dir):
    TITLE_FONT_SIZE = 12
    LEGEND_FONT_SIZE = 12
    YLABEL_FONT_SIZE = 12
    XLABEL_FONT_SIZE = 12
    HEIGHT = 1.6
    WH_RATIO = 2.60 / 1.6
    WIDTH = 2 * HEIGHT * WH_RATIO
    YTICKS_MINOR_ERROR = 250
    YTICKS_MINOR_MEM = 0.1
    MEMORY_YTICKS = [0.2 * i for i in range(1, 6)]

    fig, axes = plt.subplots(nrows=1, ncols=2, figsize=(WIDTH, HEIGHT))

    char_exps = ["0.00", "0.20", "0.40", "0.60", "0.80", "1.00"]
    workload_subdir = Path("l2_size_function_bench")
    sketches = ["SublimeCS", "SublimeCSl2"]
    label_conv = {"SublimeCS": "l1SizeFunction",
                  "SublimeCSl2": "l2SizeFunction"}
    INIT_MEMORY_FOOTPRINT = 2 ** 15
    SIZE_FUNCTION_POWER = 0.5

    are_data = {sketch: [] for sketch in sketches}
    mem_data = {sketch: [] for sketch in sketches}
    for char_exp in char_exps:
        for sketch in sketches:
            file_path = result_dir / workload_subdir / Path(f"{sketch}_{INIT_MEMORY_FOOTPRINT}_{SIZE_FUNCTION_POWER:.2f}_zipf_{char_exp}.json")
            if not file_path.is_file():
                continue
            with open(file_path, 'r') as result_file:
                contents = result_file.read()
                if len(contents) == 0:
                    continue
                json_string = "[" + fix_file_contents(contents[:-2]) + "]"
                result = json.loads(json_string)
                x_pos = float(char_exp)
                are_data[sketch].append((x_pos, max(0.01, result[-1]["are"])))
                mem_data[sketch].append((x_pos, result[-1]["size"] / (2 ** 20)))

    if len(are_data) == 0 and len(mem_data) == 0:
        logging.info(inspect.stack()[0][3][5:] + ": Figure not generated due to no benchmark results being found to include")
        return
    for sketch in sketches:
        axes[0].plot(*zip(*are_data[sketch]), **SKETCHES_STYLE_KWARGS[label_conv[sketch]], **LINES_STYLE)
        axes[1].plot(*zip(*mem_data[sketch]), **SKETCHES_STYLE_KWARGS[label_conv[sketch]], **LINES_STYLE)

    fig.subplots_adjust(wspace=0.4)
    for ax in axes:
        ax.autoscale_view()
        ax.margins(0.04)
        ax.set_xlabel(f"{DATASET_NAMES['zipf']} Exponent", fontsize=XLABEL_FONT_SIZE)
        ax.set_xticks([i / (len(char_exps) - 1) for i in range(len(char_exps))], [float(char_exp) for char_exp in char_exps])

    axes[0].text(0, 6.38, "_")

    axes[0].yaxis.set_minor_locator(matplotlib.ticker.MultipleLocator(YTICKS_MINOR_ERROR))
    axes[0].set_ylabel("P-99 Absolute Error", fontsize=YLABEL_FONT_SIZE)
    axes[1].set_ylabel("Memory [MB]", fontsize=YLABEL_FONT_SIZE)
    axes[1].yaxis.set_minor_locator(matplotlib.ticker.MultipleLocator(YTICKS_MINOR_MEM))
    axes[1].set_yticks(MEMORY_YTICKS)

    legend_lines, legend_labels = axes[0].get_legend_handles_labels()
    axes[0].legend(legend_lines, legend_labels, loc="upper left", bbox_to_anchor=(-0.055, 1.08),
                   fancybox=True, shadow=False, ncol=1, fontsize=LEGEND_FONT_SIZE, frameon=False)

    fig.savefig(output_dir / (inspect.stack()[0][3][5:] + "_(Fig_15).pdf"), bbox_inches="tight", pad_inches=0.01)



def plot_join_size(result_dir, output_dir):
    TITLE_FONT_SIZE = 10
    LEGEND_FONT_SIZE = 9
    YLABEL_FONT_SIZE = 10
    XLABEL_FONT_SIZE = 10
    HEIGHT = 1.55
    WH_RATIO = 2.75 / 1.6
    WIDTH = 3 * HEIGHT * WH_RATIO
    XTICKS = [1e6, 5e7, 1e8, 1.5e8]
    XTICK_LABELS = ["0", "50M", "100M", "150M"]

    WORKLOAD = "join_size"
    workload_subdir = Path("join_size_bench")
    sketches = ["CMS", "CS", 
                "SublimeCMS_fixed", "SublimeCS_fixed",
                "SublimeCMS_expand", "SublimeCS_expand"]
    MEMORY_FOOTPRINT = 2 ** 23
    MEMORY_RATIO_1 = 3
    MEMORY_RATIO_2 = 1

    fig, axes = plt.subplots(nrows=1, ncols=3, sharex="col", figsize=(WIDTH, HEIGHT))

    aae_data = [{sketch: [] for sketch in sketches} for i in range(3)]
    for sketch in sketches:
        memory_1 = MEMORY_FOOTPRINT * MEMORY_RATIO_1 // (MEMORY_RATIO_1 + MEMORY_RATIO_2)
        memory_2 = MEMORY_FOOTPRINT * MEMORY_RATIO_2 // (MEMORY_RATIO_1 + MEMORY_RATIO_2)
        file_path = result_dir / workload_subdir / Path(f"{sketch if "fixed" not in sketch else sketch[:-6]}_{memory_1}_{memory_2}_{WORKLOAD}.json")
        if "expand" in sketch:
            file_path = result_dir / workload_subdir / Path(f"{sketch}_{WORKLOAD}.json")
        if not file_path.is_file():
            continue
        with open(file_path, 'r') as result_file:
            contents = result_file.read()
            if len(contents) == 0 or "underflow" in contents:
                for i in range(len(aae_data)):
                    aae_data[i][sketch].append((0, 0))
                continue
            json_string = "[" + fix_file_contents(contents[:-2]) + "]"
            json_string = json_string.replace(", ]", "]")
            result = json.loads(json_string)
            result = result[::2]
            for i in range(len(aae_data)):
                for measurement in result:
                    aae_data[i][sketch].append((measurement["n_keys"], measurement["aae"][i]))
    if len(aae_data) == 0:
        logging.info(inspect.stack()[0][3][5:] + ": Figure not generated due to no benchmark results being found to include")
        return
    for sketch in sketches:
        sketch_style_kwargs = SKETCHES_STYLE_KWARGS[sketch if sketch != "CS" else sketch + "Dotted"]
        for i in range(len(aae_data)):
            axes[i].plot(*zip(*aae_data[i][sketch]), **sketch_style_kwargs, **LINES_STYLE)

    for i in range(3):
        axes[i].set_xticks(XTICKS)
        axes[i].set_xticklabels(XTICK_LABELS, fontsize=XLABEL_FONT_SIZE)
        axes[i].xaxis.set_minor_locator(matplotlib.ticker.MultipleLocator(1e7))
        axes[i].set_yscale("symlog", linthresh=(1e01))
        axes[i].yaxis.set_minor_locator(matplotlib.ticker.LogLocator(numticks=10, subs="auto"))
        axes[i].autoscale_view()
        axes[i].margins(0.04)
    fig.subplots_adjust(hspace=0.15, wspace=0.35)

    for i in range(3):
        axes[i].set_xlabel("No. of Rows", fontsize=XLABEL_FONT_SIZE)
    axes[0].set_ylabel("\\texttt{lineitem} AAE", fontsize=YLABEL_FONT_SIZE)
    axes[1].set_ylabel("\\texttt{orders} AAE", fontsize=YLABEL_FONT_SIZE)
    axes[2].set_ylabel("Join AAE", fontsize=YLABEL_FONT_SIZE)

    legend_lines, legend_labels = axes[0].get_legend_handles_labels()
    axes[0].legend(legend_lines, legend_labels, loc="upper left", bbox_to_anchor=(3.75, 1.03),
                      fancybox=True, shadow=False, ncol=1, fontsize=LEGEND_FONT_SIZE, frameon=False)
    fig.savefig(output_dir / (inspect.stack()[0][3][5:] + "_(Fig_16).pdf"), bbox_inches="tight", pad_inches=0.01)

def plot_mg_accuracy(result_dir, output_dir):
    LEGEND_FONT_SIZE = 10
    LABEL_FONT_SIZE = 10
    # Sized so each of the four rows is the same panel width/height as a full
    # (ratio-1) row of the original Sublime_CMS accuracy figure (WIDTH 7.75,
    # HEIGHT 5.0 over height ratios [1, 1, 0.2, 0.75], wspace 0.25, hspace 0.12).
    WIDTH = 7.75
    HEIGHT = 6.72
    # Both error axes are logarithmic, which has no room for the exact answers
    # (AAE 0) a summary large enough to monitor every key gives. Those are
    # clamped to the floor so the line still reaches its last budget, the same
    # convention the Sublime_MG expansion figure uses.
    AAE_FLOOR = 1e-2

    workloads = ["kosarak", "webdocs", "caida"]
    workload_subdir = Path("mg_accuracy_bench")
    # Sublime_MG's two configurations and the three baselines. The min tree is
    # the same sketch with a different decrement, so it rides beside it.
    sketches = ["SublimeMG", "SublimeMG_tree", "MG", "SpaceSaving", "Waving"]
    memory_powers = {"caida": range(17, 23), "kosarak": range(14, 19), "webdocs": range(17, 23)}
    memory_footprints = {w: [2 ** i for i in memory_powers[w]] for w in workloads}

    # Four rows: the error over every distinct key, the error over the heaviest
    # `Capacity()` of them, and then insert and query latency. The two error rows
    # say different things and the second is the one Misra-Gries' guarantee is
    # about -- over all distinct keys the mean is dominated by the tail no
    # summary of this size can hold, so it moves far less than the capacity does.
    fig, axes = plt.subplots(nrows=4, ncols=3, sharex="col", figsize=(WIDTH, HEIGHT))
    for row in (2, 3):                                  # Share the insert/query rows' y across datasets.
        axes[row][1].sharey(axes[row][0])
        axes[row][2].sharey(axes[row][0])
        axes[row][1].tick_params(labelleft=True)        # ...but keep the tick labels on every column.
        axes[row][2].tick_params(labelleft=True)

    result_found = False
    p99_data = {w: {s: {} for s in sketches} for w in workloads}
    vale_params = {w: {} for w in workloads}            # (counters-per-chunk, stub-length) for SublimeMG.
    for i, workload in enumerate(workloads):
        aae = {s: [] for s in sketches}
        top_aae = {s: [] for s in sketches}
        insert = {s: [] for s in sketches}
        query = {s: [] for s in sketches}
        for sketch, memory_footprint in itertools.product(sketches, memory_footprints[workload]):
            file_path = result_dir / workload_subdir / Path(f"{sketch}_{memory_footprint}_{workload}.json")
            if not file_path.is_file():
                continue
            with open(file_path, 'r') as result_file:
                contents = result_file.read()
                if len(contents) == 0:
                    continue
                result = json.loads("[" + fix_file_contents(contents[:-2]) + "]")[-1]
                aae[sketch].append((result["size"], max(result["aae"], AAE_FLOOR)))
                top_aae[sketch].append((result["size"], max(result["top_aae"], AAE_FLOOR)))
                insert[sketch].append((result["size"], result["time_i"] / result["n_keys"] * 1000.0))
                query[sketch].append((result["size"], result["time_q"] / result["n_unique_keys"] * 1000.0))
                p99_data[workload][sketch][memory_footprint] = result.get("p99", 0)
                if sketch == "SublimeMG" and "counters_per_chunk" in result:
                    vale_params[workload][memory_footprint] = (result["counters_per_chunk"], result["stub_length"])
        for sketch in sketches:
            if not aae[sketch]:
                continue
            result_found = True
            style = SKETCHES_STYLE_KWARGS[sketch]
            axes[0][i].plot(*zip(*aae[sketch]), **style, **LINES_STYLE)
            axes[1][i].plot(*zip(*top_aae[sketch]), **style, **LINES_STYLE)
            axes[2][i].plot(*zip(*insert[sketch]), **style, **LINES_STYLE)
            axes[3][i].plot(*zip(*query[sketch]), **style, **LINES_STYLE)
    if not result_found:
        logging.info(inspect.stack()[0][3][5:] + ": Figure not generated due to no benchmark results being found to include")
        return

    memory_footprint_labels = {w: [f"${2 ** (p - 20)}$" if p >= 20 else f"$1/{2 ** (20 - p)}$"
                                   for p in memory_powers[w]] for w in workloads}
    for i, workload in enumerate(workloads):
        for row in range(4):
            axes[row][i].set_xscale("log")
            axes[row][i].margins(0.04)
        for row in (0, 1):                              # Both error rows.
            axes[row][i].set_yscale("log")
            # An explicit `numticks` on the major locator too: the default one
            # thins the decades out once a panel spans more than a few (Kosarak
            # runs from the AAE floor to ~1e3), dropping every other label.
            axes[row][i].yaxis.set_major_locator(matplotlib.ticker.LogLocator(base=10, numticks=15))
            axes[row][i].yaxis.set_minor_locator(
                    matplotlib.ticker.LogLocator(base=10, numticks=15, subs="auto"))
        axes[2][i].yaxis.set_minor_locator(matplotlib.ticker.MultipleLocator(10))
        axes[3][i].yaxis.set_minor_locator(matplotlib.ticker.MultipleLocator(5))
        axes[0][i].set_title(DATASET_NAMES[workload], fontsize=LABEL_FONT_SIZE + 1)
        axes[3][i].set_xlabel("Memory [MB]", fontsize=LABEL_FONT_SIZE)
        axes[3][i].xaxis.set_minor_locator(matplotlib.ticker.NullLocator())
        axes[3][i].set_xticks(memory_footprints[workload])
        axes[3][i].set_xticklabels(memory_footprint_labels[workload], fontsize=LABEL_FONT_SIZE - 2)
        axes[3][i].set_xlim(memory_footprints[workload][0] / 1.1, 1.1 * memory_footprints[workload][-1])
    axes[2][0].set_ylim(bottom=0)                       # Shared -> applies across the insert row.
    axes[3][0].set_ylim(bottom=0)                       # Shared -> applies across the query row.
    axes[0][0].set_ylabel("AAE", fontsize=LABEL_FONT_SIZE)
    axes[1][0].set_ylabel("Top-$k$ AAE", fontsize=LABEL_FONT_SIZE)
    axes[2][0].set_ylabel("Insert Latency [ns]", fontsize=LABEL_FONT_SIZE)
    axes[3][0].set_ylabel("Query Latency [ns]", fontsize=LABEL_FONT_SIZE)
    fig.subplots_adjust(hspace=0.12, wspace=0.25)

    legend_lines, legend_labels = axes[0][0].get_legend_handles_labels()
    fig.legend(legend_lines, legend_labels, loc="lower center", bbox_to_anchor=(0.5, 1.0),
                      fancybox=True, shadow=False, ncol=4, fontsize=LEGEND_FONT_SIZE, frameon=False)
    fig.savefig(output_dir / (inspect.stack()[0][3][5:] + "_(Fig_17).pdf"), bbox_inches="tight", pad_inches=0.01)

    with open(output_dir / f"{inspect.stack()[0][3][5:]}_p99_table_(Fig_17).tex", 'w') as table:
        table.writelines(["\\begin{tabular}{cc" + 'c' * len(sketches) + "} \n", "\\toprule \n",
                          " & ".join(["Dataset", "Mem [B]"] + [SKETCHES_STYLE_KWARGS[s]["label"] for s in sketches]) + " \\\\ \n",
                          "\\midrule \n"])
        for workload in workloads:
            for memory_footprint in memory_footprints[workload]:
                cells = [f"{p99_data[workload][s].get(memory_footprint, '-'):.3f}"
                         if isinstance(p99_data[workload][s].get(memory_footprint), float) else "-" for s in sketches]
                table.write(" & ".join([DATASET_NAMES[workload], str(memory_footprint)] + cells) + " \\\\ \n")
        table.writelines(["\\bottomrule \n", "\\end{tabular} \n"])

    # The final VALE tuning (c = counters per chunk, s = stub length) SublimeMG
    # settled on at each memory budget, as in the Sublime_CMS accuracy figure.
    all_memory_footprints = sorted({mf for w in workloads for mf in memory_footprints[w]})
    with open(output_dir / f"{inspect.stack()[0][3][5:]}_vale_table_(Fig_17).tex", 'w') as table:
        table.writelines(["\\begin{tabular}[b]{" + 'c' * (2 * len(workloads) + 1) + "} \n", "\\toprule \n",
                          " & ".join(["\\multirow{2}{*}{Memory [MB]}"]
                                     + ["\\multicolumn{2}{c}{" + DATASET_NAMES[w] + "}" for w in workloads]) + " \\\\ \n",
                          " & ".join([""] + ["$c$", "$s$"] * len(workloads)) + " \\\\ \n",
                          "\\midrule \n"])
        for memory_footprint in all_memory_footprints:
            memory_power = math.ceil(math.log(memory_footprint, 2))
            memory_label = f"{2 ** (memory_power - 20)}" if memory_power >= 20 else f"1/{2 ** (20 - memory_power)}"
            line = memory_label
            for workload in workloads:
                cs = vale_params[workload].get(memory_footprint)
                line += f" & {cs[0]} & {cs[1]}" if cs is not None else " & - & -"
            table.write(line + " \\\\ \n")
        table.writelines(["\\bottomrule \n", "\\end{tabular} \n"])


def plot_mg_tail_latency(result_dir, output_dir):
    """The worst insertion any single update suffered, and a companion quantile.

    Mirrors the insert-latency row of the Sublime_MG accuracy figure -- same
    sketches, same budgets, same panel geometry -- but from `mg_tail_latency`'s
    own runs, because measuring it means timing every insertion and that is not
    a thing to do to a run whose *average* latency anyone will read. The y-axis
    is logarithmic: an `O(w)` decrement sweep and an `O(log w)` eviction are two
    orders of magnitude apart, which is the point of the figure.

    **Fig. 18 draws the max on caida and webdocs and the quantile on kosarak**,
    and mixing them is deliberate. The max is where the gap shows -- the sweep's
    worst insertion is its decrement pass and scales with the summary, the
    segment tree's does not -- but a max is only evidence where it *repeats*. It
    does on caida and webdocs, climbing monotonically over six budgets. It does
    not on kosarak: the summary is rarely full there, nobody does enough work to
    rise above the machine's noise floor, and five repeats of one binary span a
    factor of 16. So that panel draws `p9999`, which is stable, and which on
    kosarak at 16 KB is also the one place a quantile can see the sweep's pass
    at all -- elsewhere passes are rarer than one insertion in 10,000 and it
    reports ordinary ones. The two statistics live orders of magnitude apart, so
    the kosarak panel keeps a scale and a label of its own.

    A companion drawing `p9999` everywhere is emitted beside it, for checking
    what the mixed figure is standing on.
    """
    # Kosarak is the quantile and the other two are the max; see above.
    _plot_mg_tail_row(result_dir, output_dir,
                      lambda w: "p9999_i" if w == "kosarak" else "max_i",
                      "mg_tail_latency_(Fig_18).pdf")
    _plot_mg_tail_row(result_dir, output_dir, lambda w: "p9999_i",
                      "mg_tail_latency_p9999.pdf")


def _plot_mg_tail_row(result_dir, output_dir, metric_for, file_name):
    """One row of per-dataset panels, for `plot_mg_tail_latency`.

    `metric_for` picks the statistic per dataset, because the figure does not
    draw the same one everywhere: see the note there.
    """
    LEGEND_FONT_SIZE = 10
    LABEL_FONT_SIZE = 10
    # Panels of the same proportions as the accuracy figure's, which is WIDTH
    # 7.75 over ACCURACY_ROWS rows of ACCURACY_HEIGHT at ACCURACY_HSPACE. Same
    # width and column count, so matching the proportions means matching the
    # height of one of its panels -- and with a single row here, that is the
    # whole of this figure's axes region, whatever margins it ends up with.
    ACCURACY_HEIGHT, ACCURACY_ROWS, ACCURACY_HSPACE = 6.72, 4, 0.12
    WIDTH = 7.75
    HEIGHT = ACCURACY_HEIGHT / (ACCURACY_ROWS + (ACCURACY_ROWS - 1) * ACCURACY_HSPACE)

    workloads = ["kosarak", "webdocs", "caida"]
    workload_subdir = Path("mg_tail_latency_bench")
    sketches = ["SublimeMG", "SublimeMG_tree", "MG", "SpaceSaving", "Waving"]
    memory_powers = {"caida": range(17, 23), "kosarak": range(14, 19), "webdocs": range(17, 23)}
    memory_footprints = {w: [2 ** i for i in memory_powers[w]] for w in workloads}
    metrics = {w: metric_for(w) for w in workloads}
    # Which statistic a panel draws goes in its title rather than beside its
    # axis: a second y-label would land in the gap between panels, on top of
    # the tick labels already there, and widening that gap would take the
    # panels out of proportion with the accuracy figure's.
    metric_names = {"max_i": "Max", "p9999_i": "P99.99"}

    fig, axes = plt.subplots(nrows=1, ncols=3, sharex="col", figsize=(WIDTH, HEIGHT))
    # One scale across the datasets drawing the same statistic, with the labels
    # kept on each. A panel drawing a different one gets its own scale, since
    # the two live orders of magnitude apart.
    for i in range(1, 3):
        if metrics[workloads[i]] == metrics[workloads[0]]:
            axes[i].sharey(axes[0])
            axes[i].tick_params(labelleft=True)

    result_found = False
    for i, workload in enumerate(workloads):
        worst = {sketch: [] for sketch in sketches}
        for sketch, memory_footprint in itertools.product(sketches, memory_footprints[workload]):
            file_path = result_dir / workload_subdir / Path(f"{sketch}_{memory_footprint}_{workload}.json")
            if not file_path.is_file():
                continue
            with open(file_path, 'r') as result_file:
                contents = result_file.read()
                if len(contents) == 0:
                    continue
                result = json.loads("[" + fix_file_contents(contents[:-2]) + "]")[-1]
                if metrics[workload] not in result:     # Not a tail-latency run.
                    continue
                worst[sketch].append((result["size"], result[metrics[workload]]))
        for sketch in sketches:
            if not worst[sketch]:
                continue
            result_found = True
            axes[i].plot(*zip(*worst[sketch]), **SKETCHES_STYLE_KWARGS[sketch], **LINES_STYLE)
    if not result_found:
        logging.info("mg_tail_latency: Figure not generated due to no benchmark "
                     "results being found to include")
        return

    memory_footprint_labels = {w: [f"${2 ** (p - 20)}$" if p >= 20 else f"$1/{2 ** (20 - p)}$"
                                   for p in memory_powers[w]] for w in workloads}
    for i, workload in enumerate(workloads):
        axes[i].set_xscale("log")
        axes[i].set_yscale("log")
        axes[i].margins(0.04)
        axes[i].yaxis.set_major_locator(matplotlib.ticker.LogLocator(base=10, numticks=15))
        axes[i].yaxis.set_minor_locator(matplotlib.ticker.LogLocator(base=10, numticks=15, subs="auto"))
        axes[i].set_title(f"{DATASET_NAMES[workload]} ({metric_names[metrics[workload]]})",
                          fontsize=LABEL_FONT_SIZE + 1)
        axes[i].set_xlabel("Memory [MB]", fontsize=LABEL_FONT_SIZE)
        axes[i].xaxis.set_minor_locator(matplotlib.ticker.NullLocator())
        axes[i].set_xticks(memory_footprints[workload])
        axes[i].set_xticklabels(memory_footprint_labels[workload], fontsize=LABEL_FONT_SIZE - 2)
        axes[i].set_xlim(memory_footprints[workload][0] / 1.1, 1.1 * memory_footprints[workload][-1])
    axes[0].set_ylabel("Insert [ns]", fontsize=LABEL_FONT_SIZE)
    fig.subplots_adjust(wspace=0.25)

    # Centred on the *figure*, not on the middle panel: the panels sit inside
    # margins that are not symmetric, so their centre is not the figure's.
    legend_lines, legend_labels = axes[0].get_legend_handles_labels()
    fig.legend(legend_lines, legend_labels, loc="lower center", bbox_to_anchor=(0.5, 1.0),
               fancybox=True, shadow=False, ncol=4, fontsize=LEGEND_FONT_SIZE, frameon=False)
    fig.savefig(output_dir / file_name, bbox_inches="tight", pad_inches=0.01)


def plot_mg_expansion(result_dir, output_dir):
    LEGEND_FONT_SIZE = 9
    LABEL_FONT_SIZE = 10
    HEIGHT = 1.55
    WH_RATIO = 3.2 / 1.6
    WIDTH = 2 * HEIGHT * WH_RATIO

    workload_subdir = Path("mg_expansion_bench")
    START_MEMORY = 2 ** 15
    # The size-function power is swept, encoded by line style: 0.5 solid, 0.75
    # dashed, 1.0 dotted (as in the Sublime_CMS expansion figure).
    size_function_powers = [0.5, 0.75, 1.0]
    measures = ["error", "total"]
    workload_candidates = ["webdocs_mg_expand", "caida_mg_expand", "webdocs_expand", "caida_expand"]

    workload = None
    for candidate in workload_candidates:
        if any((result_dir / workload_subdir).glob(f"*{candidate}.json")):
            workload = candidate
            break
    if workload is None:
        logging.info(inspect.stack()[0][3][5:] + ": Figure not generated due to no benchmark results being found to include")
        return

    series = {}
    for measure in measures:
        for power in size_function_powers:
            series[f"SublimeMG_{measure}_{power:.2f}"] = result_dir / workload_subdir / \
                Path(f"SublimeMG_{START_MEMORY}_{measure}_{power:.2f}_{workload}.json")
    series["MG"] = result_dir / workload_subdir / Path(f"MG_{START_MEMORY}_{workload}.json")

    fig, axes = plt.subplots(nrows=1, ncols=2, figsize=(WIDTH, HEIGHT))
    aae_data = {s: [] for s in series}
    mem_data = {s: [] for s in series}
    for sketch, file_path in series.items():
        if not file_path.is_file():
            continue
        with open(file_path, 'r') as result_file:
            contents = result_file.read()
            if len(contents) == 0:
                continue
            results = json.loads("[" + fix_file_contents(contents[:-2]) + "]")
            # Start the plot where the fixed baselines sit at one byte per key
            # (n = START_MEMORY), dropping the extreme early points, then subsample.
            results = [r for r in results if r["n_keys"] >= START_MEMORY]
            if len(results) < 2:
                continue
            results = [results[2 ** i - 1] for i in range(math.ceil(math.log(len(results), 2)) - 1)] + results[-1:]
            for result in results:
                aae_data[sketch].append((result["n_keys"], max(result["aae"], 1e-2)))
                mem_data[sketch].append((result["n_keys"], result["size"] / result["n_keys"]))
    if all(len(v) == 0 for v in aae_data.values()):
        logging.info(inspect.stack()[0][3][5:] + ": Figure not generated due to no benchmark results being found to include")
        return
    for sketch in series:
        if not aae_data[sketch]:
            continue
        axes[0].plot(*zip(*aae_data[sketch]), **SKETCHES_STYLE_KWARGS[sketch], **LINES_STYLE)
        axes[1].plot(*zip(*mem_data[sketch]), **SKETCHES_STYLE_KWARGS[sketch], **LINES_STYLE)

    for ax in axes.flatten():
        ax.set_xscale("log")
        ax.set_yscale("log")
        ax.autoscale_view()
        ax.margins(0.04)
        ax.set_xlabel("No. of Keys", fontsize=LABEL_FONT_SIZE)
        # All major decades, plus minor ticks, on both the x and y log axes.
        ax.yaxis.set_major_locator(matplotlib.ticker.LogLocator(base=10, numticks=15))
        ax.yaxis.set_minor_locator(matplotlib.ticker.LogLocator(base=10, subs="auto", numticks=15))
        ax.xaxis.set_major_locator(matplotlib.ticker.LogLocator(base=10, numticks=15))
        ax.xaxis.set_minor_locator(matplotlib.ticker.LogLocator(base=10, subs="auto", numticks=15))
        # A little (~2pt) padding below the lowest data so bottom markers breathe.
        ylo, yhi = ax.get_ylim()
        axes_height_pt = fig.get_size_inches()[1] * ax.get_position().height * 72
        pad = 2.0 / axes_height_pt * (math.log10(yhi) - math.log10(ylo))
        ax.set_ylim(10 ** (math.log10(ylo) - pad), yhi)
    axes[0].set_ylabel("AAE", fontsize=LABEL_FONT_SIZE)
    axes[1].set_ylabel("Memory [B/Key]", fontsize=LABEL_FONT_SIZE)

    # Label the size-function power on each panel, on both the error and total
    # lines, in a single column just inside the axes. The labels are spread in
    # log-y with a minimum gap (and clamped to the axis) so that no two overlap,
    # even where lines coincide (e.g. the identical power-0.5 error/total lines).
    for ax, data in ((axes[0], aae_data), (axes[1], mem_data)):
        xmin, xmax = ax.get_xlim()
        ax.set_xlim(xmin, xmax * 2.3)
        ylo, yhi = (math.log10(v) for v in ax.get_ylim())
        entries, label_x = [], 0.0
        for measure in measures:
            for power in size_function_powers:
                sketch = f"SublimeMG_{measure}_{power:.2f}"
                if not data[sketch]:
                    continue
                x, y = data[sketch][-1]
                label_x = max(label_x, x)
                entries.append([math.log10(y), f"${power:.2f}$", SKETCHES_STYLE_KWARGS[sketch]["color"],
                                f"{measure}_{power:.2f}"])
        entries.sort()
        # Just-enough vertical gap so coincident labels stack almost touching
        # (~2pt apart): convert the target point spacing to log-y using the axes
        # height, rather than a fixed fraction of the range.
        FONT_PT = 0.68 * LABEL_FONT_SIZE
        axes_height_pt = fig.get_size_inches()[1] * ax.get_position().height * 72
        gap = (0.72 * FONT_PT + 1) / axes_height_pt * (yhi - ylo)
        gap = min(gap, 0.9 * (yhi - ylo) / max(len(entries) - 1, 1))
        for i in range(1, len(entries)):                # Push overlapping labels up.
            entries[i][0] = max(entries[i][0], entries[i - 1][0] + gap)
        if entries and entries[-1][0] > yhi:            # Clamp to the top, push the rest down.
            entries[-1][0] = yhi
            for i in range(len(entries) - 2, -1, -1):
                entries[i][0] = min(entries[i][0], entries[i + 1][0] - gap)
        # Shift each stacked (coincident) pair/group down by ~4pt so it sits
        # centred on its lines rather than pushed above them.
        shift = 4.0 / axes_height_pt * (yhi - ylo)
        i = 0
        while i < len(entries):
            j = i
            while j + 1 < len(entries) and entries[j + 1][0] - entries[j][0] <= gap * 1.05:
                j += 1
            if j > i:
                for k in range(i, j + 1):
                    entries[k][0] -= shift
            i = j + 1
        # On the AAE panel, nudge the error-version 0.75 label up ~2pt.
        if ax is axes[0]:
            for e in entries:
                if e[3] == "error_0.75":
                    e[0] += 2.0 / axes_height_pt * (yhi - ylo)
        # Extra horizontal clearance so the plotted lines never occlude the text.
        for logy, text, color, _ in entries:
            ax.annotate(text, (label_x * 1.32, 10 ** logy), fontsize=0.68 * LABEL_FONT_SIZE,
                        va="center", ha="left", color=color)
    fig.subplots_adjust(wspace=0.4)

    legend_lines, legend_labels = axes[0].get_legend_handles_labels()
    axes_h_pt = fig.get_size_inches()[1] * axes[0].get_position().height * 72
    fig.legend(legend_lines, legend_labels, loc="lower center",
                   bbox_to_anchor=(0.5, 1.0 + 2.0 / axes_h_pt),
                   fancybox=True, shadow=False, ncol=3, fontsize=LEGEND_FONT_SIZE, frameon=False)
    fig.savefig(output_dir / (inspect.stack()[0][3][5:] + "_(Fig_19).pdf"), bbox_inches="tight", pad_inches=0.01)


PLOTTERS = {plot_accuracy.__name__[5:]: plot_accuracy,
            plot_skew_vale_tuning.__name__[5:]: plot_skew_vale_tuning,
            "skew": plot_skew_vale_tuning,
            "vale_tuning": plot_skew_vale_tuning,
            plot_expansion.__name__[5:]: plot_expansion,
            plot_contraction.__name__[5:]: plot_contraction,
            plot_accuracy_unbiased.__name__[5:]: plot_accuracy_unbiased,
            plot_l2_size_function.__name__[5:]: plot_l2_size_function,
            plot_join_size.__name__[5:]: plot_join_size,
            plot_mg_accuracy.__name__[5:]: plot_mg_accuracy,
            plot_mg_tail_latency.__name__[5:]: plot_mg_tail_latency,
            plot_mg_expansion.__name__[5:]: plot_mg_expansion}


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

