#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
plugin_root="$(cd "${script_dir}/.." && pwd)"
source_skill="${plugin_root}/skills/ue-ai"
client="codex"
target_root=""
force=0

usage() {
    echo "Usage: install_entry_skill.sh [--client codex|claude|both] [--target-root PATH] [--force]"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --client) client="${2:-}"; shift 2 ;;
        --target-root) target_root="${2:-}"; shift 2 ;;
        --force) force=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

case "$client" in codex|claude|both) ;; *) echo "Unsupported client: $client" >&2; exit 2 ;; esac
if [[ ! -f "${source_skill}/SKILL.md" ]]; then
    echo "UE AI entry Skill is missing from ${source_skill}" >&2
    exit 3
fi
if [[ -n "$target_root" && "$client" == "both" ]]; then
    echo "--target-root can be used only with one selected client." >&2
    exit 2
fi

install_one() {
    local name="$1"
    local root
    if [[ -n "$target_root" ]]; then
        root="$target_root"
    elif [[ "$name" == "codex" ]]; then
        root="${CODEX_HOME:-${HOME}/.codex}"
    else
        root="${CLAUDE_CONFIG_DIR:-${HOME}/.claude}"
    fi
    local skills_root="${root}/skills"
    local destination="${skills_root}/ue-ai"
    mkdir -p "$skills_root"

    if [[ -d "$destination" ]] && diff -qr "$source_skill" "$destination" >/dev/null; then
        echo "[${name}] UE AI entry Skill unchanged: ${destination}"
        return
    fi
    if [[ -e "$destination" && ! -d "$destination" ]]; then
        echo "Entry Skill destination is not a directory: ${destination}" >&2
        exit 4
    fi
    if [[ -d "$destination" && "$force" -ne 1 ]]; then
        echo "Entry Skill differs at ${destination}; re-run with --force to preserve it as a backup and install this version." >&2
        exit 4
    fi

    local stage
    stage="$(mktemp -d "${skills_root}/.ue-ai-stage.XXXXXX")"
    cleanup_stage() {
        case "$stage" in "${skills_root}"/.ue-ai-stage.*) rm -rf -- "$stage" ;; esac
    }
    trap cleanup_stage RETURN
    cp -R "${source_skill}/." "$stage/"
    if ! diff -qr "$source_skill" "$stage" >/dev/null; then
        echo "Entry Skill staging verification failed." >&2
        exit 5
    fi

    local backup=""
    if [[ -d "$destination" ]]; then
        backup="${skills_root}/ue-ai.backup-$(date -u +%Y%m%dT%H%M%SZ)"
        if [[ -e "$backup" ]]; then backup="${backup}-$RANDOM"; fi
        mv -- "$destination" "$backup"
    fi
    if ! mv -- "$stage" "$destination"; then
        if [[ -n "$backup" && ! -e "$destination" && -d "$backup" ]]; then
            mv -- "$backup" "$destination"
        fi
        exit 5
    fi
    trap - RETURN
    echo "[${name}] UE AI entry Skill installed: ${destination}"
    if [[ -n "$backup" ]]; then echo "      previous copy preserved at: ${backup}"; fi
}

if [[ "$client" == "both" ]]; then
    install_one codex
    install_one claude
else
    install_one "$client"
fi
