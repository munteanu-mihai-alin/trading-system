#!/usr/bin/env bash
# Deploy a CI-built engine binary from GitHub Packages to an instance.
#
# Closes the gap that made stale binaries possible: CI has been
# publishing ghcr images for weeks and nothing ever consumed one, so
# the box ran locally-built binaries whose provenance nobody could
# check. bin/hft_app was once an ad-hoc build matching no staged
# version at all.
#
#   scripts/deploy_from_ghcr.sh --instance paper [--tag <ref>] [--force]
#
# --tag defaults to the instance's current branch tag. Accepts either a
# branch tag (chronos2-mr-pred-exit) or a pinned one
# (chronos2-mr-pred-exit-42c15b2); the pinned form is what you want for
# a repeatable deploy.
#
# The binary is the authority on what it is. We name the staged
# directory from what `hft_app --branch --commit` reports, NOT from the
# tag we asked for, so a mislabelled image cannot produce a directory
# that lies about its contents.
#
# Rollback is the previous symlink target, captured before the swap and
# restored on any failure after it. Nothing is ever overwritten in
# place: `cp` onto bin/hft_app would follow the symlink and silently
# rewrite the staged version it points at.

set -euo pipefail

ROOT="/mnt/HC_Volume_105581071/trading-live"
SERVICES="${ROOT}/services"
IMAGE_BASE="ghcr.io/munteanu-mihai-alin/trading-system/hft_app"

INSTANCE=""
TAG=""
FORCE="false"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --instance) INSTANCE="$2"; shift 2 ;;
    --tag)      TAG="$2"; shift 2 ;;
    --force)    FORCE="true"; shift ;;
    -h|--help)  sed -n '2,26p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[[ -n "${INSTANCE}" ]] || { echo "ERROR: --instance is required" >&2; exit 2; }
INST_DIR="${ROOT}/${INSTANCE}"
[[ -d "${INST_DIR}" ]] || { echo "ERROR: no such instance: ${INST_DIR}" >&2; exit 2; }

# ---- interlock 1: never swap under a running engine ----------------
# Not overridable. Repointing the symlink while the engine holds open
# positions means the next restart silently runs different code than
# the one that opened them.
UNIT="hft_app@${INSTANCE}.service"
if systemctl is-active --quiet "${UNIT}"; then
  echo "REFUSING: ${UNIT} is active. Stop the engine before deploying." >&2
  exit 1
fi

# ---- interlock 2: not during (or just before) the session ----------
# Even with the engine down, the RTH start timer can fire mid-swap and
# launch whatever the symlink happens to point at that instant. The
# window starts at 09:00 rather than 09:30 to cover the 09:25 start.
NY_DOW="$(TZ=America/New_York date +%u)"
NY_HHMM="$(TZ=America/New_York date +%H%M)"
if [[ "${NY_DOW}" -le 5 && "${NY_HHMM}" > "0900" && "${NY_HHMM}" < "1600" ]]; then
  if [[ "${FORCE}" != "true" ]]; then
    echo "REFUSING: $(TZ=America/New_York date '+%a %H:%M %Z') is inside the" >&2
    echo "  trading window and the RTH timer may start the engine mid-swap." >&2
    echo "  Re-run outside 09:00-16:00 ET, or pass --force." >&2
    exit 1
  fi
  echo "WARNING: deploying inside the trading window (--force)."
fi

# ---- resolve the tag ------------------------------------------------
if [[ -z "${TAG}" ]]; then
  CURRENT_LINK="$(readlink "${INST_DIR}/bin/hft_app" 2>/dev/null || true)"
  if [[ -n "${CURRENT_LINK}" ]]; then
    # versions/<branch>-<sha>/hft_app -> <branch>
    CUR_VER="$(basename "$(dirname "${CURRENT_LINK}")")"
    TAG="${CUR_VER%-*}"
  fi
  [[ -n "${TAG}" ]] || { echo "ERROR: could not infer --tag" >&2; exit 2; }
  echo "No --tag given; using the instance's current branch: ${TAG}"
fi

IMAGE="${IMAGE_BASE}:${TAG}"
echo "Pulling ${IMAGE} ..."
docker pull --quiet "${IMAGE}" >/dev/null

# ---- extract --------------------------------------------------------
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"; docker rm -f "${CID:-}" >/dev/null 2>&1 || true' EXIT
CID="$(docker create "${IMAGE}")"
docker cp "${CID}:/opt/hft/hft_app" "${TMP}/hft_app"
chmod +x "${TMP}/hft_app"

# ---- ask the binary what it is --------------------------------------
# Also proves it links and runs on this host before it goes anywhere
# near the symlink.
export LD_LIBRARY_PATH="${SERVICES}/dependencies/linux/install/lib"
if ! PROV="$("${TMP}/hft_app" --branch --commit --version 2>&1)"; then
  echo "ERROR: extracted binary failed to run:" >&2
  echo "${PROV}" >&2
  exit 1
fi
BRANCH="$(sed -n 's/^branch=//p' <<<"${PROV}")"
COMMIT="$(sed -n 's/^commit=//p' <<<"${PROV}")"
BUILD_VERSION="$(sed -n 's/^version=//p' <<<"${PROV}")"
[[ -n "${BRANCH}" && -n "${COMMIT}" ]] || {
  echo "ERROR: binary did not report provenance; refusing to deploy" >&2
  exit 1
}
echo "Binary reports: branch=${BRANCH} commit=${COMMIT} version=${BUILD_VERSION}"

# A build with no CI run number in its version did not come from CI.
if [[ "${BUILD_VERSION}" != *.*.*.* && "${FORCE}" != "true" ]]; then
  echo "REFUSING: version '${BUILD_VERSION}' has no CI run number, so this" >&2
  echo "  image was not built by CI. Pass --force to deploy it anyway." >&2
  exit 1
fi

# ---- stage ----------------------------------------------------------
VERSION="${BRANCH}-${COMMIT}"
DEST="${INST_DIR}/bin/versions/${VERSION}"
mkdir -p "${DEST}"
install -m 0755 "${TMP}/hft_app" "${DEST}/hft_app"
cat > "${DEST}/binary.json" <<EOF
{
  "version": "${VERSION}",
  "branch": "${BRANCH}",
  "commit": "${COMMIT}",
  "build_version": "${BUILD_VERSION}",
  "source": "${IMAGE}",
  "deployed_at": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "description": "${BRANCH} @ ${COMMIT} (ghcr)"
}
EOF
echo "Staged ${DEST}/hft_app"

# ---- activate, with rollback ----------------------------------------
LINK="${INST_DIR}/bin/hft_app"
PREVIOUS="$(readlink "${LINK}" 2>/dev/null || true)"
ln -sfn "versions/${VERSION}/hft_app" "${LINK}"

VERIFY="$("${LINK}" --commit 2>/dev/null | sed -n 's/^commit=//p;s/^\([0-9a-f]\{7,\}\)$/\1/p' | head -1 || true)"
if [[ "${VERIFY}" != "${COMMIT}" ]]; then
  echo "ERROR: post-swap check failed (expected ${COMMIT}, got '${VERIFY}')" >&2
  if [[ -n "${PREVIOUS}" ]]; then
    ln -sfn "${PREVIOUS}" "${LINK}"
    echo "Rolled back to ${PREVIOUS}" >&2
  fi
  exit 1
fi

echo
echo "Deployed to ${INSTANCE}:"
echo "  ${LINK} -> versions/${VERSION}/hft_app"
echo "  commit=${COMMIT} version=${BUILD_VERSION}"
[[ -n "${PREVIOUS}" ]] && echo "  rollback: ln -sfn ${PREVIOUS} ${LINK}"
echo
echo "The engine starts on the next RTH timer firing, or:"
echo "  systemctl start ${UNIT}"
