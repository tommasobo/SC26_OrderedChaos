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

Slurm is supported and recommended for the full workflow because independent
experiment groups can run concurrently:

```bash
export SCRATCH=/path/to/high-capacity/storage
./reproduce.sh short --slurm
./reproduce.sh full --slurm
```

Slurm account and partition values are supplied through
`ORDEREDCHAOS_SLURM_ACCOUNT` and `ORDEREDCHAOS_SLURM_PARTITION` when needed.
