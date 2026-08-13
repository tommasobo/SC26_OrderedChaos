# Reproduction files

Run the short local workflow from the repository root:

```bash
./reproduce.sh short
```

Run the complete workflow locally:

```bash
./reproduce.sh full
```

The local runner provisions the pinned Python environment, builds and tests
the simulator, and writes clearly named plots under `results/`.
In this host-only artifact, the full workflow additionally regenerates the
corrected section/tail probe compounding study and the periodic-probe incast
stress study. Their two panels are collected as one PDF/PNG figure in the
final result directory's `ExtraFigures/` subdirectory and are formatted for
single-column paper placement. A companion LaTeX caption defines the compact
probe notation and records the hidden experimental context.

Plotting uses the repository-level `matplotlibrc`, which prefers Liberation
Serif and falls back to Matplotlib's bundled DejaVu Serif without an extra
download. Both are embedded as TrueType outlines in PDF output. Collection runs
`artifact/scripts/check_pdf_fonts.py` before declaring success, so Type 3 or
unembedded fonts fail the reproduction.

Slurm is supported and recommended for the full workflow because independent
experiment groups can run concurrently:

```bash
export SCRATCH=/path/to/high-capacity/storage
./reproduce.sh short --slurm
./reproduce.sh full --slurm
```

Slurm account and partition values are supplied through
`ORDEREDCHAOS_SLURM_ACCOUNT` and `ORDEREDCHAOS_SLURM_PARTITION` when needed.
