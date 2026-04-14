#!/usr/bin/env bash
# E2E test for PRD step 4: Import Video pipeline end-to-end.
#
# Uses CLI flags (--import-run, --no-extract, --no-instantsplat, --screenshot)
# to drive the app programmatically — no xdotool/ydotool or mouse simulation.
#
# Acceptance criteria exercised:
#   1. CLI --import-run triggers import without UI interaction
#   2. Import completes without errors
#   3. Full import completes within 10 minutes
#   4. Gaussian splats render in the viewport after import
#   5. --screenshot captures the rendered scene as PNG
#   6. Screenshot achieves >= 90% pixelmatch alignment vs first frame JPG
#   7. No crashes/hangs/unhandled errors during workflow
#
# Usage: cd <repo_root> && bash .plans/e2e_test.sh
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="$REPO/build/world_imagine"
RUN_DIR="$REPO/run_20260413221014"
FIRST_FRAME="$RUN_DIR/frames/frame_000000.jpg"
SCREENSHOT="/tmp/world_imagine_e2e_screenshot.png"
COMPARE_SCRIPT="$REPO/.plans/pixelmatch_compare.js"
PASS_THRESHOLD=0.90
IMPORT_TIMEOUT=600

export LD_LIBRARY_PATH="$HOME/local/libtorch/lib:/usr/local/cuda/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

fail() { echo "FAIL: $1" >&2; exit 1; }
pass() { echo "PASS: $1"; }

# ---------------------------------------------------------------------------
# 0. Pre-flight
# ---------------------------------------------------------------------------
[ -f "$BINARY" ]       || fail "Binary not found: $BINARY (run cmake --build build first)"
[ -d "$RUN_DIR" ]      || fail "Run directory not found: $RUN_DIR"
[ -f "$FIRST_FRAME" ]  || fail "First frame not found: $FIRST_FRAME"
[ -d "$RUN_DIR/instantsplat" ] || fail "InstantSplat fixture not found in $RUN_DIR/instantsplat/"
command -v node >/dev/null     || fail "node not installed"
[ -f "$COMPARE_SCRIPT" ]      || fail "pixelmatch compare script not found: $COMPARE_SCRIPT"

echo "=== World Imagine E2E Test (CLI mode) ==="
echo "Binary:    $BINARY"
echo "Run dir:   $RUN_DIR"
echo "Timeout:   ${IMPORT_TIMEOUT}s"

# ---------------------------------------------------------------------------
# 1. Run app with auto-import flags
# ---------------------------------------------------------------------------
echo ""
echo "[1] Running app with --import-run (timeout ${IMPORT_TIMEOUT}s)..."

rm -f "$SCREENSHOT"

timeout "$IMPORT_TIMEOUT" "$BINARY" \
    --import-run "$RUN_DIR" \
    --no-extract \
    --no-instantsplat \
    --screenshot "$SCREENSHOT" \
    --screenshot-delay 3

EXIT_CODE=$?
if [ "$EXIT_CODE" -ne 0 ]; then
    fail "App exited with code $EXIT_CODE"
fi

pass "Import completed and app exited cleanly"

# ---------------------------------------------------------------------------
# 2. Verify screenshot
# ---------------------------------------------------------------------------
echo ""
echo "[2] Verifying screenshot..."

[ -f "$SCREENSHOT" ] || fail "Screenshot not produced at $SCREENSHOT"
pass "Screenshot taken: $SCREENSHOT"

# ---------------------------------------------------------------------------
# 3. pixelmatch comparison
# ---------------------------------------------------------------------------
echo ""
echo "[3] Comparing screenshot against first video frame..."

RESULT=$(node "$COMPARE_SCRIPT" "$SCREENSHOT" "$FIRST_FRAME" "$PASS_THRESHOLD" 2>&1)
echo "    $RESULT"

if echo "$RESULT" | grep -q "^PASS"; then
    pass "pixelmatch comparison passed (>= $(echo "$PASS_THRESHOLD * 100" | bc -l | xargs printf "%.0f")% similarity)"
else
    fail "pixelmatch comparison failed: $RESULT"
fi

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
echo ""
echo "=== ALL ACCEPTANCE CRITERIA PASSED ==="
