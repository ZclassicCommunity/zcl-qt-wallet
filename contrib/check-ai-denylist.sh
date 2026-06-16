#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ai_dir="$repo_root/src/ai"

if [[ ! -d "$ai_dir" ]]; then
  echo "AI denylist: no src/ai directory yet; nothing to check"
  exit 0
fi

pattern='(z_sendmany|sendrawtransaction|signrawtransaction|walletpassphrase|dumpprivkey|z_exportkey|z_exportviewingkey|z_exportwallet|importprivkey|z_importkey|RPC::executeTransaction|Connection::doRPC)'

if grep -RInE "$pattern" "$ai_dir"; then
  echo "AI denylist: forbidden raw wallet/RPC primitive referenced in src/ai" >&2
  exit 1
fi

echo "AI denylist: pass"
