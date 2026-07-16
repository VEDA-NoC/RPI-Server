#!/usr/bin/env bash
set -euo pipefail

target="${NOC_RPI_TARGET:-noc@noc}"
destination="${NOC_RPI_DEST:-/home/noc/rpi-vms}"
dry_run=false
clean=false
build=false

usage() {
    cat <<'EOF'
Usage: tools/sync-to-pi.sh [options]

Options:
  --target USER@HOST  SSH destination (default: noc@noc)
  --dest PATH         Absolute path on the Pi (default: /home/noc/rpi-vms)
  --dry-run           Show rsync changes without transferring files
  --clean             Remove stale/excluded files from the deployment directory
  --build             Run make clean && make on the Pi after synchronization
  -h, --help          Show this help

Environment:
  NOC_RPI_TARGET      Same as --target
  NOC_RPI_DEST        Same as --dest
EOF
}

while (($# > 0)); do
    case "$1" in
        --target)
            target="${2:?--target requires USER@HOST}"
            shift 2
            ;;
        --dest)
            destination="${2:?--dest requires an absolute path}"
            shift 2
            ;;
        --dry-run)
            dry_run=true
            shift
            ;;
        --clean)
            clean=true
            shift
            ;;
        --build)
            build=true
            shift
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            echo "error: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ "$destination" != /* ]]; then
    echo "error: --dest must be an absolute path" >&2
    exit 2
fi

if $clean && [[ "$destination" != "/home/noc/rpi-vms" ]]; then
    echo "error: --clean is allowed only for /home/noc/rpi-vms" >&2
    exit 2
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"

rsync_args=(
    --archive
    --compress
    --human-readable
    --itemize-changes
    --info=progress2
    --filter="P /app"
    --filter="P /.env"
    --filter="P /.env.*"
    --filter="P /certs/***"
    --filter="P /docs/current-environment-local-ko.md"
    --exclude="/.git/"
    --exclude="/.github/"
    --exclude="/build/"
    --exclude="/cmake-build-*/"
    --exclude="/docs/current-environment-local-ko.md"
    --exclude="/recordings/"
    --exclude="/index/"
    --exclude="*.db"
    --exclude="*.db-shm"
    --exclude="*.db-wal"
    --exclude="*.mp4"
    --exclude="*.log"
    --exclude="*.key"
    --exclude="*.pem"
)

if $dry_run; then
    rsync_args+=(--dry-run)
fi

if $clean; then
    rsync_args+=(--delete-delay --delete-excluded)
fi

printf 'source: %s/\n' "$repo_root"
printf 'target: %s:%s/\n' "$target" "$destination"
printf 'mode: dry_run=%s clean=%s build=%s\n' "$dry_run" "$clean" "$build"

ssh "$target" "mkdir -p -- '$destination'"
rsync "${rsync_args[@]}" "$repo_root/" "$target:$destination/"

if $build && ! $dry_run; then
    ssh "$target" "cd -- '$destination' && make clean && make"
fi
