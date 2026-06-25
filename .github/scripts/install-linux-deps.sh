#!/usr/bin/env bash
# Installs CI Linux packages after hardening runner apt sources.

set -euo pipefail

if [ "$#" -eq 0 ]; then
  echo "usage: install-linux-deps.sh <apt-package>..." >&2
  exit 2
fi

# Disables preinstalled third-party runner sources that can break apt update.
disable_third_party_sources() {
  local source_dir="/etc/apt/sources.list.d"
  if [ ! -d "${source_dir}" ]; then
    return
  fi

  while IFS= read -r source_file; do
    echo "Disabling apt source: ${source_file}"
    sudo mv "${source_file}" "${source_file}.disabled"
  done < <(sudo find "${source_dir}" -type f \
    \( -name "*.list" -o -name "*.sources" \) \
    ! -name "ubuntu.sources" \
    ! -name "*.disabled" \
    -print)
}

# Retries apt update with third-party sources disabled if the runner image has a stale repo.
apt_update_with_retry() {
  if sudo apt-get update -qq; then
    return
  fi

  echo "apt-get update failed; retrying without third-party runner apt sources" >&2
  disable_third_party_sources
  sudo apt-get update -qq
}

apt_update_with_retry
sudo apt-get install -y -qq "$@"
