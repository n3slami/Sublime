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
import json
import matplotlib
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import itertools, os
from pathlib import Path
import logging


rc_fonts = {
    "font.family": "serif",
    "font.size": 9.5,
    "text.usetex": True,
    'text.latex.preamble': r'\usepackage{mathpazo}'}
matplotlib.rcParams.update(rc_fonts)

logging.getLogger().setLevel(logging.INFO)

CLASSIC_SKETCHES_STYLE_KWARGS = {"CMSketchbookFixedCounters": {"marker": 'v', "color": "fuchsia", "zorder": 12, "label": "Count-Min Sketch"},
                                 "CSketchbookFixedCounters": {"marker": 'x', "color": "dimgray", "zorder": 10, "label": "Count Sketch"},
                                 "MGDummy": {"marker": '+', "color": "C1", "zorder": 11, "label": "Misra-Gries"}}
LINES_STYLE = {"markersize": 4, "linewidth": 0.7, "fillstyle": "none"}
DATASET_NAMES = {"unif": r"$\textsc{Uniform}$", 
                 "norm": r"$\textsc{Normal}$",
                 "corr": r"$\textsc{Correlated}$", 
                 "zipf": r"$\textsc{Zipfian}$", 
                 "real": r"$\textsc{Real}$", 
                 "books": r"$\textsc{Books}$",
                 "osm": r"$\textsc{OSM}$",
                 "fb": r"$\textsc{FB}$"}


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


def plot_classical_comparison(result_dir, output_dir):
    TITLE_FONT_SIZE = 9.5
    LEGEND_FONT_SIZE = 7
    YLABEL_FONT_SIZE = 9.5
    XLABEL_FONT_SIZE = 9.5
    XTICK_FONT_SIZE = 8
    WIDTH = 5
    HEIGHT = 2
    YTICKS_SMALL = [1, 1e-01, 1e-02, 1e-03]
    YTICKS = [1, 1e-01, 1e-02, 1e-03, 1e-04, 1e-05]

    WORKLOAD = "zipf"
    workload_subdir = Path("classical_comparison_bench")
    sketches = ["CMSketchbookFixedCounters", "CSketchbookFixedCounters", "MGDummy"]
    memory_powers = range(15, 21)
    memory_footprints = [2 ** i for i in memory_powers]
    memory_footprint_labels = [f"${2 ** (i - (20 if i == 20 else 10))}${'KB' if i < 20 else 'MB'}" for i in memory_powers]
    char_exps = [0.10 + 0.20 * i for i in range(13)]
    CHAR_EXP = 1.00
    MEMORY_FOOTPRINT = 2 ** 17

    fig, axes = plt.subplots(nrows=1, ncols=2, sharey=True, figsize=(WIDTH, HEIGHT))

    # Memory footprint plot
    aae_data = {sketch: [] for sketch in sketches}
    are_data = {sketch: [] for sketch in sketches}
    for memory_footprint in memory_footprints:
        for sketch in sketches:
            file_path = result_dir / workload_subdir / Path(f"{sketch}_{memory_footprint}_{WORKLOAD}_{CHAR_EXP:.2f}.json")
            if not file_path.is_file():
                continue
            with open(file_path, 'r') as result_file:
                contents = result_file.read()
                if len(contents) == 0:
                    continue
                json_string = "[" + fix_file_contents(contents[:-2]) + "]"
                result = json.loads(json_string)
                aae_data[sketch].append((result[-1]["size"], result[-1]["aae"]))
                are_data[sketch].append((result[-1]["size"], result[-1]["are"]))
    for sketch in sketches:
        axes[1].plot(*zip(*aae_data[sketch]), **CLASSIC_SKETCHES_STYLE_KWARGS[sketch], **LINES_STYLE)
        #axes[1].plot(*zip(*are_data[sketch]), **CLASSIC_SKETCHES_STYLE_KWARGS[sketch], **LINES_STYLE, linestyle=':')

    aae_data = {sketch: [] for sketch in sketches}
    are_data = {sketch: [] for sketch in sketches}
    for char_exp in char_exps:
        for sketch in sketches:
            char_exp_str = f"{char_exp:.2f}"
            if char_exp < 1:
                char_exp_str = char_exp_str[1:]
            file_path = result_dir / workload_subdir / Path(f"{sketch}_{MEMORY_FOOTPRINT}_{WORKLOAD}_{char_exp_str}.json")
            if not file_path.is_file():
                continue
            with open(file_path, 'r') as result_file:
                contents = result_file.read()
                if len(contents) == 0:
                    continue
                json_string = "[" + fix_file_contents(contents[:-2]) + "]"
                result = json.loads(json_string)
                aae_data[sketch].append((char_exp, result[-1]["aae"]))
                are_data[sketch].append((char_exp, result[-1]["are"]))
    for sketch in sketches:
        axes[0].plot(*zip(*aae_data[sketch]), **CLASSIC_SKETCHES_STYLE_KWARGS[sketch], **LINES_STYLE)
        #axes[0].plot(*zip(*are_data[sketch]), **CLASSIC_SKETCHES_STYLE_KWARGS[sketch], **LINES_STYLE, linestyle=':')
    
    axes[1].set_xlabel("Memory Footprint [Bytes]", fontsize=XLABEL_FONT_SIZE)
    axes[0].set_xlabel("Char. Exponent ($a$)", fontsize=XLABEL_FONT_SIZE)
    axes[0].set_ylabel(f"{DATASET_NAMES['zipf']} AAE", fontsize=YLABEL_FONT_SIZE)
    axes[1].set_title("Char. Exponent ($a=1.0$)", fontsize=TITLE_FONT_SIZE)
    axes[0].set_title("Memory Footprint=128KB", fontsize=TITLE_FONT_SIZE)

    axes[1].set_xscale("log")
    axes[1].set_xticks(memory_footprints, memory_footprint_labels, fontsize=0.93*XTICK_FONT_SIZE)
    axes[1].minorticks_off()
    axes[0].set_xticks(char_exps[::2], [f"{val:.2f}" for val in char_exps[::2]], fontsize=XTICK_FONT_SIZE)
    for i in range(2):
        axes[i].yaxis.set_minor_locator(matplotlib.ticker.MultipleLocator(1000))
        axes[i].autoscale_view()
        axes[i].margins(0.04)
    fig.subplots_adjust(hspace=0.05, wspace=0.1)

    legend_lines, legend_labels = axes[0].get_legend_handles_labels()
    axes[0].legend(legend_lines, legend_labels, loc='upper left', bbox_to_anchor=(0.2, 1.3),
                  fancybox=True, shadow=False, ncol=6, fontsize=LEGEND_FONT_SIZE)
    fig.savefig(output_dir / "classical_comparison.pdf", bbox_inches='tight', pad_inches=0.01)


PLOTTERS = {"classical_comparison": plot_classical_comparison}


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

