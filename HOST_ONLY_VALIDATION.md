# Host-only PFLD variant

This artifact was developed separately from the earlier `final-artifact`
revision, starting from commit `1d54d60c1bb34d1446219ffc869f770be58791fe`.
Its `sim/queue` and `sim/switch` trees are byte-identical to the earlier main commit
`e20b7dd78ddfcf3c70565ec4668b5f563d34f1d1`.

The host-only behavior has two receiver rules:

1. A trim records its NACK in PFLD's existing per-PSN bitmap, preventing the
   paired ordinary probe from producing a duplicate NACK.
2. If an unchanged queue promotes a probe into the high-priority header FIFO,
   the receiver does not use that probe as ordering evidence. Such a marker
   may have overtaken the data packet it names. A later in-order probe, trim,
   PSN gap, or timeout can still establish loss.

The final artifact's sender ordering (retransmissions before optional probes)
is retained. No queue or switch source change is required.

## Camera-ready follow-up (2026-08-11)

Clean commit `b27e9dd639043343c1f2e346bd37a638f0e96c40` was validated by
Slurm jobs `3051129` and `3051206`. Job `3051129` passed all 2,400 runs
(1,600 Camera Ready Image 2 and 800 probe compounding); all 69 unit tests pass.
The packaged outputs and full numerical notes are under:

`/users/btommaso/OrderedChaos_Artifact/Paper/host_only_followup_20260811`

Camera Ready Image 2 now uses all 16,256 directed pairs of 128 nodes, with
1 MiB flows in balanced round-robin stages and at most 32 active sender
streams. At corruption probability `5e-3` over 100 matched seeds, mean RTOs
off/on are PFLD `0.95/49.85` versus TLP-RACK `207.15/161.87`. All eight
baselines were freshly rerun; bars report means with normal 95% confidence
intervals.

The unsafe-probe rule was separately ablated with 12 matched seeds per mode on
the exact selected workload. Accepting header-promoted probes is exactly
identical with trimming off. With trimming on it reduces counted RTOs by 70%,
but increases NACKs by 11.6%, retransmissions by 4.6%, makespan by 1.9%, and
mean maximum FCT by 147%. The rule therefore remains enabled.

The redesigned probe figure uses a trim-off, 128-node, 8 MiB tornado workload.
Across 100 seeds, mean RTOs fall from `112.81` with PSN gaps only to `56.68`
with tail probes and to `0` after RTX probes. Holding RTX probes off, the
RTT/16/8/4/1 periodic-frequency variants are statistically flat (`55.86` to
`56.99` RTOs), an important negative result rather than a plotting artifact.

Coalescing introduces no new per-flow allocation relative to remote main: it
reuses the two existing receive/NACK vectors. Relative to the paper's abstract
single 512-bit bitmap (64 B/flow), a conservative two-bit packed production
representation would total 128 B/flow. The simulator's larger byte-per-slot
representation is pre-existing on main.

## Four-times-larger camera and periodic-probe stress follow-up (2026-08-11)

The packaged report is:

`/users/btommaso/OrderedChaos_Artifact/Paper/host_only_followup_4mib_20260811/orderedchaos_camera_4mib_probe_followup.pdf`

Clean commit `82d0f1488b6157f1ef7d3416ab0325a7902c714d` was validated by
Slurm job `3051492`. All 2,200 fresh simulations passed (1,600 camera and 600
periodic-stress runs), as did all 69 compiled unit tests.

Camera Ready Image 2 retains the 128-node, 16,256-flow, 32-active-source
balanced all-to-all schedule and changes only the exact flow size from
1,000,000 to 4,000,000 bytes. Across 100 seeds at corruption probability
`5e-3`, mean RTOs off/on are PFLD `0.08/49.51` versus TLP-RACK
`149.42/132.49`. PFLD maximum FCT is about 3.7% higher, so the result is
correctly described as an RTO advantage rather than an FCT advantage.

The probe label has been corrected from `tail probes` to `section/tail probes`:
the profile also emits a section-end marker when RSS closes an old entropy
section. In the balanced 8,000,000-byte tornado workload, periodic frequency
remains statistically flat without RTX probes. A separate, explicitly
disclosed 32-to-1, 4,000,000-byte incast stress case exposes the complementary
regime: mean RTOs fall from `2,597.36` with section/tail probes only to
`522.94` at X=4, `48.10` at X=1, and `132.10` with once/RTT probing.
Maximum FCT remains bottleneck-dominated and does not improve.

## Matched-seed validation

All 69 compiled tests pass. Fresh raw outputs are under
`/iopsstor/scratch/cscs/btommaso/orderedchaos-host-only-20260807`.

| Workload | Matched sample | Exact final | Host-only | Observation |
|---|---:|---:|---:|---|
| Figure 7A, trim on, no corruption | 10 seeds | median max FCT 216.983 us | 219.034 us | +0.95% |
| Figure 7A, trim on, corruption 5e-5 | 10 seeds | median max FCT 217.423 us | 221.777 us | +2.00% |
| Figure 8 PFLD, both trim modes | 10 seeds / mode | mean 4070.511 us | 4070.511 us | exactly identical |
| Figure 10, trim on, corruption 5e-5 | 12 seeds | median 787.465 us | 759.617 us | -3.54% (host-only faster) |

The important limitation is recovery-path fidelity in Figure 10. With trim on
and corruption 5e-5, the host-only runs average 9.58 RTO events per full run,
versus 0.17 for exact final. Completion time does not regress in this sample,
but the switch-free variant cannot guarantee that every promoted probe remains
usable as ordered loss evidence. The exact-final reserved low FIFO removes that
ambiguity and remains the stronger choice when eliminating RTOs is the primary
claim.
