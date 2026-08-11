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

This host-only artifact uses one freshly built simulator binary for every
baseline. Its switch and queue sources are byte-identical to the earlier main
revision `e20b7dd`; it retains the existing `-no_droping_low_header`
configuration but does not add the switch-assisted variant's reserved probe
FIFO or switch-side coalescing.
At the receiver, explicit trim evidence is recorded in the existing PFLD NACK
bitmap so that the following ordinary proactive probe cannot generate a
duplicate NACK. A probe promoted out of the low FIFO is rejected as unsafe
ordering evidence because it may have overtaken its named packet;
retransmission probes remain able to refresh a NACK if the repair is also
lost. At the sender, retransmissions are sent before optional probes. Figures
7--12 and both camera-ready experiments use a proactive probe
interval of one data packet (`X=1`); experiments outside that scope retain the
original artifact configuration. These choices are recorded in
`artifact/config/paper_experiments.json`.

Archived artifact: [https://doi.org/10.5281/zenodo.21542397](https://doi.org/10.5281/zenodo.21542397)

## Requirements

- Linux, Git, curl, CMake 3.16 or newer, GCC/G++, and GNU time
- Liberation Serif TrueType fonts (commonly provided by `fonts-liberation`)
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
The full workflow also regenerates the host-only section/tail compounding and
32-to-1 periodic-probe stress studies. It presents them as two panels of one
camera-ready probe-ablation figure, whose PDF and PNG are collected under
`results/full-<run-id>/ExtraFigures/`. The numbered and established
camera-ready results remain in the parent result directory.
Figures 11 and 12 use checksum-verified binary traces from the public SPCL
server. If one of these input families cannot be downloaded, the workflow
continues with every independent result and places a clearly named
`Figure_11_SKIPPED.txt` or `Figure_12_SKIPPED.txt` record in the result
directory.

All generated PDF plots use Liberation Serif embedded as TrueType outlines.
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
