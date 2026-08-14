# Ordered Chaos: Towards Adaptive Multipathed Load Balancing with Precise Fast Loss Detection

## Abstract

Load balancing in datacenter networks faces a fundamental tradeoff:
high-spread load balancing approaches, such as Random Packet Spraying (RPS),
achieve high utilization but destroy in-order guarantees, making loss
detection nontrivial. On the other hand, Equal-Cost Multipath (ECMP) and
similar schemes preserve order but underutilize network capacity. We propose a
unified solution, Ordered Chaos, that combines the best of both worlds.
Reactive Subflow Spraying (RSS) splits a flow into a small set of logical
subflows and dynamically reroutes the worst one, achieving near-uniform
utilization under asymmetric and shifting conditions. Precise Fast Loss
Detection (PFLD) leverages probing and piecewise in-order delivery within
subflows, allowing the receiver to distinguish reordering from true loss.
Packet-level simulations show that RSS+PFLD improves tail flow completion time
by up to 3x over ECMP, PLB, Flowlet, RPS, and REPS.

## Reproduction

This repository contains the simulator and complete reproduction workflow.
Both local and Slurm execution are supported. Local execution is the default
and is convenient for the short check on a laptop or workstation. Slurm is
recommended for the full workflow because independent experiment groups can
run concurrently. Neither backend requires a GPU.

All experiments use one freshly built simulator binary. This artifact
evaluates the end-host implementation and does not include the optional
switch-assisted reserved probe FIFO or switch-side coalescing. Complete
experiment parameters are recorded in
`artifact/config/paper_experiments.json`.

Archived artifact: [https://doi.org/10.5281/zenodo.21542397](https://doi.org/10.5281/zenodo.21542397)

## Requirements

- Linux, Git, curl, CMake 3.16 or newer, GCC/G++, and GNU time
- Liberation Serif TrueType fonts are preferred (commonly provided by
  `fonts-liberation`). If unavailable, the workflow automatically uses DejaVu
  Serif, which is bundled with the pinned Matplotlib package.
- 16 GiB of memory for the short workflow
- 32 GiB of memory and 100 GiB of free disk space for the full workflow

## Expected runtime

For planning, we assume a recent laptop with 8 CPU cores, 32 GiB of memory, a
local SSD, and the default two workers. The estimated runtime is 15 to 30
minutes for the short workflow and 48 to 96 hours for the full workflow.
Actual time depends on processor speed and available memory. The Slurm
workflow is substantially faster because independent experiment groups run
concurrently.

## Reproduce locally

```bash
git clone --branch main --single-branch \
  https://github.com/tommasobo/SC26_OrderedChaos.git
cd SC26_OrderedChaos
./reproduce.sh short
```

Run the complete paper workflow with:

```bash
./reproduce.sh full
```

The local runner uses two simulator workers and executes large experiment
groups sequentially. Set `ORDEREDCHAOS_JOBS` to change the worker count:

```bash
ORDEREDCHAOS_JOBS=4 ./reproduce.sh short
```

Generated plots are placed in `results/short-<run-id>` or
`results/full-<run-id>`. Successful runs contain a `PASS` file.
The full workflow also regenerates the section/tail compounding and
32-to-1 periodic-probe stress studies. All supplementary camera-ready outputs
are collected under `results/full-<run-id>/ExtraFigures/`: the Google RPC FCT
distributions, the timeout-event plots, and the two-panel probe-ablation
figure. Numbered `Figure_*` results remain in the parent result directory.
Figures 11 and 12 use checksum-verified binary traces from the public SPCL
server. If one of these input families cannot be downloaded, the workflow
continues with every independent result and places a clearly named
`Figure_11_SKIPPED.txt` or `Figure_12_SKIPPED.txt` record in the result
directory.

Generated plots prefer Liberation Serif and automatically fall back to the
Matplotlib-bundled DejaVu Serif when Liberation is unavailable. Both are
embedded as TrueType outlines, and the fallback needs no extra system package
or font download.
Before writing `PASS`, the collection workflow runs
`artifact/scripts/check_pdf_fonts.py` and rejects any PDF containing a Type 3
or unembedded font.

## Slurm execution

For the recommended full reproduction, set a high-capacity work directory and
add `--slurm`:

```bash
export SCRATCH=/path/to/high-capacity/storage
./reproduce.sh short --slurm
./reproduce.sh full --slurm
```

The Slurm files used by the standard reproduction contain no account or
partition names. If required by a site, provide them at launch time:

```bash
ORDEREDCHAOS_SLURM_ACCOUNT=my_account \
ORDEREDCHAOS_SLURM_PARTITION=compute \
./reproduce.sh full --slurm
```
