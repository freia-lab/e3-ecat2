#!/usr/bin/env bash
set -euo pipefail

JSON=${1:-./config.json}
ETHERCAT=${2:-/opt/etherlab/bin/ethercat}

echo ">>> Building ..."
make

echo ">>> Running diagnostic (needs sudo to request/activate master) ..."
sudo ./ecat_diag "$JSON" --with-cli "$ETHERCAT"

echo ">>> Validation complete."
