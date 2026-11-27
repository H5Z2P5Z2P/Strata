#!/usr/bin/env bash
set -euo pipefail
exec bash "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/start-model.sh" unsloth-ud-iq3_xxs-dflash2 "$@"
