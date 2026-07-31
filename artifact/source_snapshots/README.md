# Source snapshots

The complete reproduction uses two source states that generated the
paper-facing trace experiments. They are bundled as deterministic tar archives
so a clean checkout does not depend on unrelated Git history.

| Archive | SHA-256 |
|---|---|
| `camera_ready_source.tar.gz` | `aac94a52e7ede37905b51c03072811db814670737b8644dd9f5babfdd64bce5a` |
| `trace_source.tar.gz` | `003f988cc941bafd7d459bb43eed4f837e0fe2ffc5a0de1d7d6ad41ecb31dd0d` |

`artifact/scripts/prepare_source_snapshots.sh` verifies and extracts these
archives into scratch. Compiler compatibility changes are applied afterward
from the versioned patch files.
