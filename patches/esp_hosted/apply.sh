#!/bin/bash
# Apply the local esp_hosted patches to every example that pulls the component.
#
# The IDF component manager rewrites managed_components/ when it re-resolves
# dependencies, so these patches must be re-applied after that. The script is
# idempotent: a patch that is already applied is skipped.
#
# Upstream each patch to esp_hosted and delete it here once it is merged.

set -e

REPO_ROOT="$(git rev-parse --show-toplevel)"
PATCH_DIR="$REPO_ROOT/patches/esp_hosted"
applied=0
skipped=0

for comp in "$REPO_ROOT"/examples/*/managed_components/espressif__esp_hosted; do
    [ -d "$comp" ] || continue
    for p in "$PATCH_DIR"/[0-9]*.patch; do
        [ -f "$p" ] || continue
        if patch -p1 -d "$comp" -R --dry-run -s -f < "$p" >/dev/null 2>&1; then
            skipped=$((skipped + 1))
            continue
        fi
        if ! patch -p1 -d "$comp" -s -f < "$p"; then
            echo "error: failed to apply $(basename "$p") to $comp"
            exit 1
        fi
        applied=$((applied + 1))
    done
done

echo "esp_hosted patches: $applied applied, $skipped already present"
