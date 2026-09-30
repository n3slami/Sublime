# Reproduces the paper's figures in a container.
#
#   docker build -t sublime_eval .
#   mkdir -p ../paper_results
#   docker run -v "$(cd .. && pwd)/paper_results":/usr/local/paper_results sublime_eval
#
# Alpine, so the image is far smaller than the `ubuntu` + `texlive-full` it
# replaced. The TeX set is the minimum that actually renders these figures,
# arrived at by trying it: `texlive` for latex, `texlive-dvi` for dvipng and
# dvips, `texmf-dist-fontsrecommended` for mathpazo (the plots are set in
# Palatino), and `texmf-dist-latexextra`, without which matplotlib's usetex
# preamble fails to typeset.
#
# Note that Sublime is *not* compiled here. The build happens when the container
# runs, so that `-march=native` sees the CPU the benchmarks will run on -- which
# is the point of the exercise. That also means the image is portable: it picks
# up BMI2, LZCNT and AVX-512 where the host has them, and falls back where it
# does not.
FROM alpine:3.21

WORKDIR /usr/local/

RUN apk add --no-cache \
        bash coreutils git wget curl \
        build-base cmake make g++ \
        python3 py3-pip \
        texlive texlive-dvi texmf-dist-fontsrecommended texmf-dist-latexextra ghostscript

# matplotlib at image build time, so that running the container needs the
# network only for the datasets.
COPY bench/scripts/requirements.txt /tmp/requirements.txt
RUN pip install --break-system-packages --no-cache-dir -r /tmp/requirements.txt

# The artifact itself, minus what .dockerignore excludes.
COPY . /usr/local/Sublime/
WORKDIR /usr/local/Sublime

# Vendor the two baseline sketches now, so a run does not have to fetch them.
RUN bash bench/scripts/setup_includes.sh

# So the container can run as the invoking user rather than as root. Without
# this, everything it writes into the mounted results directory comes out owned
# by root on the host, which a reviewer then has to chown before they can even
# delete it. The build tree and the results both need to be writable by whatever
# uid `docker run --user` is given.
RUN chmod -R a+rwX /usr/local/Sublime && mkdir -p /usr/local/paper_results && chmod a+rwX /usr/local/paper_results
# A uid with no passwd entry has no home; matplotlib and git both want one.
ENV HOME=/tmp
ENV MPLCONFIGDIR=/tmp/matplotlib

# Results land in /usr/local/paper_results, which is the directory next to the
# clone that evaluate.sh writes to. Mount it to keep them.
ENTRYPOINT ["./evaluate.sh"]
