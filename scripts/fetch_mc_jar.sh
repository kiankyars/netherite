#!/usr/bin/env bash
# fetch_mc_jar.sh - put YOUR minecraft-1.11.2.jar in the ForgeGradle cache
# layout, for boxes with no launcher install and no decompile workspace.
#
# You must own Minecraft (https://www.minecraft.net). This fetches the client
# jar from Mojang's official distribution endpoints, resolved through the
# version manifest and checked against the manifest sha1 - the same artifact
# and the same endpoints ForgeGradle's setupDecompWorkspace pulls.
#
# Why the gradle cache path and not just $MC_JAR: assets/mc_jar.py honours
# MC_JAR, but c/magma/tests/check_jar_models.py (make verify-harsh) has a fixed
# candidate list that starts at ~/.gradle/caches. Landing the jar there
# satisfies both.
#
# Usage: bash scripts/fetch_mc_jar.sh [DEST_JAR]
set -euo pipefail

VER=1.11.2
MANIFEST=https://piston-meta.mojang.com/mc/game/version_manifest_v2.json
DEST="${1:-$HOME/.gradle/caches/minecraft/net/minecraft/minecraft/$VER/minecraft-$VER.jar}"

command -v curl >/dev/null || { echo "ERROR: curl not found" >&2; exit 2; }
command -v uv   >/dev/null || { echo "ERROR: uv not found" >&2; exit 2; }

if [ -f "$DEST" ]; then
    echo "already present: $DEST"
    exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo "== resolving $VER from the Mojang version manifest =="
curl -fsS --max-time 60 "$MANIFEST" -o "$tmp/manifest.json"
read -r URL SHA1 < <(
    MC_MANIFEST="$tmp/manifest.json" MC_VER="$VER" \
    uv run --no-project python - <<'PY'
import json, os, sys
m = json.load(open(os.environ["MC_MANIFEST"]))
v = [x for x in m["versions"] if x["id"] == os.environ["MC_VER"]]
if not v:
    sys.exit("version %s not in manifest" % os.environ["MC_VER"])
print(v[0]["url"])
PY
)
curl -fsS --max-time 60 "$URL" -o "$tmp/version.json"
read -r URL SHA1 < <(
    MC_VERSION_JSON="$tmp/version.json" uv run --no-project python - <<'PY'
import json, os
c = json.load(open(os.environ["MC_VERSION_JSON"]))["downloads"]["client"]
print(c["url"], c["sha1"])
PY
)

echo "== downloading client jar =="
curl -fsS --max-time 600 "$URL" -o "$tmp/client.jar"
got=$(sha1sum "$tmp/client.jar" | cut -d' ' -f1)
[ "$got" = "$SHA1" ] || {
    echo "ERROR: sha1 mismatch (got $got, manifest says $SHA1)" >&2
    exit 1
}

mkdir -p "$(dirname "$DEST")"
mv "$tmp/client.jar" "$DEST"
echo "minecraft-$VER.jar ready: $DEST (sha1 $got)"
