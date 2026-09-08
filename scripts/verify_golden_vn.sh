#!/usr/bin/env bash
# =============================================================================
#  Caesura (AmeKAG) — verify_golden_vn.sh
#
#  End-to-end verification for the Golden Project (tests/projects/golden_vn/),
#  the long-term release regression fixture (task book §14 / release-gate.md).
#
#  Steps
#   1. ks_check         — static contract check of story.ks (zero warnings)
#   2. headless full run — kag_runner drives the whole script to [end] (DONE)
#   3. branch reachability — both choice routes funnel into *common_mid -> [end]
#   4. feature surface   — the story references every commanded feature
#   5. web smoke        — informational manual step (not executed here)
#
#  Usage (from repo root):  bash scripts/verify_golden_vn.sh
#  Exit: 0 = all checks passed, 1 = any check failed.
# =============================================================================
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
cd "$ROOT" || { echo "[verify-golden] cannot cd repo root"; exit 1; }

# CTest supplies the exact lua_cli product; never replace an explicit path
# with another configuration, a packaged relic, or an interpreter on PATH.
if [ "${CAESURA_LUA+x}" = x ]; then
    LUA="$CAESURA_LUA"
    if command -v cygpath >/dev/null 2>&1; then
        LUA="$(cygpath -u "$LUA")" || {
            echo "[verify-golden] FATAL: cannot convert CAESURA_LUA path"; exit 1;
        }
    fi
    if [ -z "$CAESURA_LUA" ] || [ ! -f "$LUA" ]; then
        echo "[verify-golden] FATAL: CAESURA_LUA does not point at a Lua interpreter: $CAESURA_LUA"
        exit 1
    fi
else
    # Unconfigured manual invocation retains the developer convenience probe.
    LUA=""
    for _luacand in \
        external/lua/lua.exe external/lua/lua \
        build/lua/Release/lua.exe build/lua/Release/lua \
        build/lua/Debug/lua.exe build/lua/Debug/lua \
        build/lua/RelWithDebInfo/lua.exe build/lua/RelWithDebInfo/lua \
        build/lua/MinSizeRel/lua.exe build/lua/MinSizeRel/lua \
        build/lua/lua.exe build/lua/lua
    do
        if [ -f "$_luacand" ]; then LUA="$_luacand"; break; fi
    done
    if [ -z "$LUA" ]; then
        LUA="$(command -v lua5.4 2>/dev/null || true)"
        [ -n "$LUA" ] || LUA="$(command -v lua 2>/dev/null || true)"
    fi
    if [ -z "$LUA" ] || [ ! -e "$LUA" ]; then
        echo "[verify-golden] FATAL: no Lua interpreter (probed: external/lua/lua[.exe], build/lua/{Release,Debug,RelWithDebInfo,MinSizeRel}/lua[.exe], build/lua/lua[.exe], PATH lua5.4/lua)"; exit 1
    fi
fi

STORY="${GOLDEN_STORY:-tests/projects/golden_vn/story.ks}"
BRANCHES="${GOLDEN_BRANCHES:-route_forest route_city}"
DRIVER="tests/scripts/sample_game_headless.lua"
FRAME_BUDGET="${GOLDEN_FRAMES:-200000}"

PASS=0; FAIL=0
note() { echo "[verify-golden] $*"; }

check() { # check <name> <exitcode> [detail]
    if [ "$2" -eq 0 ]; then PASS=$((PASS + 1)); note "PASS  $1"
    else FAIL=$((FAIL + 1)); note "FAIL  $1  (exit=$2 ${3:-})"; fi
}

echo ""
echo "================================================================"
echo "  Golden Project verification — $STORY"
echo "================================================================"

# ---- 1. Static contract check (goal: zero warnings) ----
note "Step 1: ks_check ($STORY)"
KSC_OUT="$("$LUA" scripts/ks_check.lua "$STORY" 2>&1)"
KSC_RC=$?
echo "$KSC_OUT" | sed "s/^/  /"
WARN_COUNT="$(printf "%s" "$KSC_OUT" | grep -c "^[WARN]" || true)"
if [ "$KSC_RC" -ne 0 ]; then
    check "ks_check: clean contract" "$KSC_RC" "($WARN_COUNT warnings)"
elif [ "$WARN_COUNT" -gt 0 ]; then
    note "  (informational: $WARN_COUNT lint warning(s), not a CI gate)"
    check "ks_check: clean contract" 0
else
    check "ks_check: clean contract, zero warnings" 0
fi

# ---- 1b. ks_check: v2 roundtrip scene (lives under tests/scripts/ -- the
#      _safeScenePath allowlist location that makes save->load resume legal) ----
note "Step 1b: ks_check tests/scripts/golden_rt.ks"
RTKS_OUT="$("$LUA" scripts/ks_check.lua tests/scripts/golden_rt.ks 2>&1)"
RTKS_RC=$?
echo "$RTKS_OUT" | sed "s/^/  /"
if [ "$RTKS_RC" -eq 0 ]; then
    check "ks_check: roundtrip scene clean contract" 0
else
    check "ks_check: roundtrip scene clean contract" "$RTKS_RC"
fi

# ---- 2. Headless full run to [end] ----
note "Step 2: headless full run to [end] (frame budget=$FRAME_BUDGET)"
MAIN_OUT="$(SAMPLE_STORY="$STORY" SAMPLE_FRAMES="$FRAME_BUDGET" "$LUA" "$DRIVER" 2>&1)"
MAIN_RC=$?
printf "%s\n" "$MAIN_OUT" | grep -E "RESULT|ENDING|FATAL" | sed "s/^/  /" || true
if [ "$MAIN_RC" -eq 0 ] && printf "%s\n" "$MAIN_OUT" | grep -q "RESULT DONE"; then
    check "headless: golden project runs to DONE" 0
else
    check "headless: golden project runs to DONE" "$MAIN_RC"
fi

# ---- 3. Choice-route reachability ----
note "Step 3: choice-route reachability (branches -> [end])"
for b in $BRANCHES; do
    case "$b" in
        route_forest) EXPECT_ROUTE=forest; EXPECT_TEXT='You take the quiet forest path.' ;;
        route_city) EXPECT_ROUTE=city; EXPECT_TEXT='You take the bright city path.' ;;
        *) check "$b branch reachable -> DONE" 1 "unknown Golden branch contract"; continue ;;
    esac
    RL="$(SAMPLE_STORY="$STORY" SAMPLE_ENDING="$b" SAMPLE_FRAMES="$FRAME_BUDGET" \
          SAMPLE_BRANCH_ROUTE="$EXPECT_ROUTE" SAMPLE_BRANCH_TEXT="$EXPECT_TEXT" "$LUA" "$DRIVER" 2>&1)"
    RL_RC=$?
    printf "%s\n" "$RL" | grep -E "RESULT|ENDING|BRANCH_PROGRESS" | sed "s/^/  /" || true
    # A reveal is now final, so repeated clicks during typewriter animation no
    # longer inflate the count. Require the route variable AND a fully revealed
    # branch line, with real completion and the original terminal-token guard.
    # A staged-but-unexecuted jump, wrong branch, or nonzero driver exit fails.
    if [ "$RL_RC" -eq 0 ] && printf "%s\n" "$RL" | grep -q "RESULT DONE"; then
        TOK="$(printf "%s\n" "$RL" | grep -o 'token=[0-9]*' | grep -o '[0-9]*' | head -1)"
        if [ "${TOK:-0}" -ge 100 ] && printf "%s\n" "$RL" | grep -Fxq "BRANCH_PROGRESS route=$EXPECT_ROUTE text=true"; then
            check "$b branch reachable -> DONE" 0
        else
            check "$b branch reachable -> DONE" 1 "missing branch progress: token=${TOK:-?}"
        fi
    elif printf "%s\n" "$RL" | grep -q "ENDING_NOT_FOUND"; then
        check "$b branch reachable -> DONE" 2
    else
        check "$b branch reachable -> DONE" 1
    fi
done

# Exercise the actual driver with deliberately wrong semantic expectations.
# These must finish the scene but reject its claimed branch proof, so a driver
# that emits an unconditional success marker cannot satisfy this gate.
note "Step 3b: branch proof negative controls"
for mismatch in route text; do
    EXPECT_ROUTE=forest
    EXPECT_TEXT='You take the quiet forest path.'
    if [ "$mismatch" = route ]; then EXPECT_ROUTE=city
    else EXPECT_TEXT='__golden_nonexistent_branch_text__'; fi
    NEG_OUT="$(SAMPLE_STORY="$STORY" SAMPLE_ENDING=route_forest SAMPLE_FRAMES="$FRAME_BUDGET" \
        SAMPLE_BRANCH_ROUTE="$EXPECT_ROUTE" SAMPLE_BRANCH_TEXT="$EXPECT_TEXT" "$LUA" "$DRIVER" 2>&1)"
    NEG_RC=$?
    if [ "$NEG_RC" -eq 1 ] && printf "%s\n" "$NEG_OUT" | grep -q "RESULT DONE" \
        && printf "%s\n" "$NEG_OUT" | grep -Fxq "BRANCH_PROGRESS route=forest text=false"; then
        check "branch proof rejects mismatched $mismatch after a complete run" 0
    else
        check "branch proof rejects mismatched $mismatch after a complete run" 1 "rc=$NEG_RC"
    fi
done

# ---- 4. Feature surface coverage (source greps, no run needed) ----
note "Step 4: feature surface coverage in story.ks"
SRC="$(cat "$STORY")"
FEATURES="playbgm playse playvoice save load select nvl tween layout layout_slot i18n history replay trans eval macro jump set end"
for feat in $FEATURES; do
    if printf "%s" "$SRC" | grep -q "\[$feat"; then
        check "feature [$feat] present" 0
    else
        check "feature [$feat] present" 4 "missing from story.ks"
    fi
done

# ---- 4b. v1 headless flags (golden_vn_headless.lua: eval/save-load/macro/xscene) ----
note "Step 4b: v1 feature flags via golden_vn_headless.lua (routes + cross-scene)"
if [ ! -f "tests/scripts/golden_vn_headless.lua" ]; then
    check "golden_vn_headless.lua present" 4 "driver file missing"
else
    V1_OK=1
    for route in 1 2; do
        V1_OUT="$(GOLDEN_ROUTE="$route" SAMPLE_STORY="$STORY" SAMPLE_FRAMES="$FRAME_BUDGET" \
                  "$LUA" tests/scripts/golden_vn_headless.lua 2>&1)"
        V1_RC=$?
        printf "  [route=%s] %s\n" "$route" "$(printf '%s\n' "$V1_OUT" | grep -E "RESULT|ROUTE|EVAL_OK|LOAD_MISS_OK|MACRO_OK" | tr '\n' ' ')"
        ROUTE_EXPECT=$([ "$route" = "1" ] && echo forest || echo city)
        if [ "$V1_RC" -eq 0 ] \
           && printf '%s\n' "$V1_OUT" | grep -q "RESULT DONE" \
           && printf '%s\n' "$V1_OUT" | grep -q "ROUTE $ROUTE_EXPECT" \
           && printf '%s\n' "$V1_OUT" | grep -q "EVAL_OK" \
           && printf '%s\n' "$V1_OUT" | grep -q "LOAD_MISS_OK" \
           && printf '%s\n' "$V1_OUT" | grep -q "MACRO_OK"; then
            check "v1 flags route=$route (eval/save-load/macro, route=$ROUTE_EXPECT)" 0
        else
            V1_OK=0
            check "v1 flags route=$route (eval/save-load/macro, route=$ROUTE_EXPECT)" 1 "rc=$V1_RC"
        fi
    done
    XOUT="$(GOLDEN_CROSS=1 SAMPLE_STORY="tests/projects/golden_vn/golden_cross.ks" \
             SAMPLE_FRAMES="$FRAME_BUDGET" "$LUA" tests/scripts/golden_vn_headless.lua 2>&1)"
    XRC=$?
    printf "  [cross] %s\n" "$(printf '%s\n' "$XOUT" | grep -E "RESULT|XSCENE_OK|XSCENE_REMAP" | tr '\n' ' ')"
    if [ "$XRC" -eq 0 ] \
       && printf '%s\n' "$XOUT" | grep -q "RESULT DONE" \
       && printf '%s\n' "$XOUT" | grep -q "XSCENE_OK"; then
        check "v1 cross-scene jump (scene_b executed, XSCENE_OK)" 0
    else
        check "v1 cross-scene jump (scene_b executed, XSCENE_OK)" 1 "rc=$XRC"
    fi
fi

# ---- 4c. v2 semantic flags (rollback / history+backlog / save->load roundtrip) ----
note "Step 4c: v2 semantic flags via golden_vn_headless.lua"
RBOUT="$(GOLDEN_RB=1 SAMPLE_FRAMES="$FRAME_BUDGET" "$LUA" tests/scripts/golden_vn_headless.lua 2>&1)"
RBRC=$?
printf "  [rollback] %s\n" "$(printf '%s\n' "$RBOUT" | grep -E "RB_|RESULT" | tr '\n' ' ')"
if [ "$RBRC" -eq 0 ] \
   && printf '%s\n' "$RBOUT" | grep -q "RESULT DONE" \
   && printf '%s\n' "$RBOUT" | grep -q "RB_FORWARD rb=2 observedB=true" \
   && printf '%s\n' "$RBOUT" | grep -q "RB_POP1 ok=true rb=2 rewind=true" \
   && printf '%s\n' "$RBOUT" | grep -q "RB_POP2 ok=true rb=1" \
   && printf '%s\n' "$RBOUT" | grep -q "RB_POP2_A" \
   && printf '%s\n' "$RBOUT" | grep -q "RB_REPLAY_END"; then
    check "v2 rollback semantic (pop1 rewinds token + f.rb=2, pop2 restores f.rb=1)" 0
else
    check "v2 rollback semantic (pop1 rewinds token + f.rb=2, pop2 restores f.rb=1)" 1 "rc=$RBRC"
fi
HIOUT="$(GOLDEN_HISTORY=1 SAMPLE_FRAMES="$FRAME_BUDGET" "$LUA" tests/scripts/golden_vn_headless.lua 2>&1)"
HIRC=$?
printf "  [history] %s\n" "$(printf '%s\n' "$HIOUT" | grep -E "HISTORY|BACKLOG|RESULT" | tr '\n' ' ')"
if [ "$HIRC" -eq 0 ] \
   && printf '%s\n' "$HIOUT" | grep -q "RESULT DONE" \
   && printf '%s\n' "$HIOUT" | grep -q "HISTORY_OPEN backlog=2" \
   && printf '%s\n' "$HIOUT" | grep -q "BACKLOG_OK" \
   && printf '%s\n' "$HIOUT" | grep -q "HISTORY_OK"; then
    check "v2 history/backlog semantic (overlay opens, 2 verifiable entries, Esc close, story continues)" 0
else
    check "v2 history/backlog semantic (overlay opens, 2 verifiable entries, Esc close, story continues)" 1 "rc=$HIRC"
fi
RTOUT="$(GOLDEN_ROUNDTRIP=1 SAMPLE_FRAMES="$FRAME_BUDGET" "$LUA" tests/scripts/golden_vn_headless.lua 2>&1)"
RTRC=$?
printf "  [roundtrip] %s\n" "$(printf '%s\n' "$RTOUT" | grep -E "RT_|ROUNDTRIP|RESULT" | tr '\n' ' ')"
# Require a newly active coroutine, restored values, and a completed
# continuation. The consumed save command must not write the slot a second time.
RTSAVES=$(printf '%s\n' "$RTOUT" | grep -c "\[SaveCmd\] Saved to slot 9")
if [ "$RTRC" -eq 0 ] \
   && printf '%s\n' "$RTOUT" | grep -q "RESULT DONE" \
   && printf '%s\n' "$RTOUT" | grep -q "RT_FORWARD marker=POST_SAVE_MUTATED counter=2" \
   && printf '%s\n' "$RTOUT" | grep -q "ROUNDTRIP_OK" \
   && printf '%s\n' "$RTOUT" | grep -q "RT_RESUME_ARMED" \
   && [ "$RTSAVES" -eq 1 ] \
   && printf '%s\n' "$RTOUT" | grep -q "RT_CONTINUATION_OK" \
   && printf '%s\n' "$RTOUT" | grep -q "RT_REPLAY_END"; then
    check "v2 save->load roundtrip (restored state + active continuation, no duplicate save)" 0
else
    check "v2 save->load roundtrip (restored state + active continuation, no duplicate save)" 1 "rc=$RTRC saves=$RTSAVES"
fi

# ---- 4d. v3 semantic flags: NVL mode (accumulate / page-turn / save / off) ----
note "Step 4d: v3 NVL semantic flags via golden_vn_headless.lua"
NVLOUT="$(GOLDEN_NVL=1 SAMPLE_FRAMES="$FRAME_BUDGET" "$LUA" tests/scripts/golden_vn_headless.lua 2>&1)"
NVLRC=$?
printf "  [nvl] %s\n" "$(printf '%s\n' "$NVLOUT" | grep -E "NVL_|RESULT" | tr '\n' ' ')"
if [ "$NVLRC" -eq 0 ] \
   && printf '%s\n' "$NVLOUT" | grep -q "RESULT DONE" \
   && printf '%s\n' "$NVLOUT" | grep -q "NVL_ACCUM_OK" \
   && printf '%s\n' "$NVLOUT" | grep -q "NVL_PAGE_OK" \
   && printf '%s\n' "$NVLOUT" | grep -q "NVL_SAVE_OK" \
   && printf '%s\n' "$NVLOUT" | grep -q "NVL_OFF_OK" \
   && printf '%s\n' "$NVLOUT" | grep -q "NVL_REPLAY_END"; then
    check "v3 NVL semantic (accum/page/save/off)" 0
else
    check "v3 NVL semantic (accum/page/save/off)" 1 "rc=$NVLRC"
fi

# ---- 4e. v3 semantic flags: voice (backlog field / save serialize / dispatch) ----
note "Step 4e: v3 voice semantic flags via golden_vn_headless.lua"
VOOUT="$(GOLDEN_VOICE=1 SAMPLE_FRAMES="$FRAME_BUDGET" "$LUA" tests/scripts/golden_vn_headless.lua 2>&1)"
VORC=$?
printf "  [voice] %s\n" "$(printf '%s\n' "$VOOUT" | grep -E "VOICE_|RESULT" | tr '\n' ' ')"
if [ "$VORC" -eq 0 ] \
   && printf '%s\n' "$VOOUT" | grep -q "RESULT DONE" \
   && printf '%s\n' "$VOOUT" | grep -q "VOICE_BL_OK" \
   && printf '%s\n' "$VOOUT" | grep -q "VOICE_SAVE_OK" \
   && printf '%s\n' "$VOOUT" | grep -q "VOICE_DISPATCH_OK" \
   && printf '%s\n' "$VOOUT" | grep -q "VOICE_REPLAY_END"; then
    check "v3 voice semantic (backlog/save/dispatch)" 0
else
    check "v3 voice semantic (backlog/save/dispatch)" 1 "rc=$VORC"
fi

# ---- 5. Web smoke (informational) ----
echo ""
note "Step 5: Web smoke (manual — not executed here)"
note "  bash scripts/package_game.sh --out dist/golden-tmp $STORY"
note "  then serve dist/golden-tmp and pick the scene from the dropdown."

echo ""
echo "================================================================"
TOTAL=$((PASS + FAIL))
if [ "$FAIL" -eq 0 ]; then
    echo "  RESULT: PASS ($PASS/$TOTAL checks)"
    echo "================================================================"
    exit 0
else
    echo "  RESULT: FAIL ($FAIL/$TOTAL checks failed)"
    echo "================================================================"
    exit 1
fi
