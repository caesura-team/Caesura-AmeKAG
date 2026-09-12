-- =============================================================================
--  Caesura (AmeKAG) ?? scheduler.lua
--  Token stream executor. Iterates tokens, dispatches to kag[cmd](ctx, params),
--  handles flow-control inline (if/jump/call/return/label/end/macro/eval/wait).
--  Coroutine-based: yields on blocking ops, resumes next frame from token_index.
-- =============================================================================

-- [R4-FIX] Note: The [button]/[endbutton] choice/branch system is implemented
-- purely in Lua (kag/commands/text.lua). Flow control here handles [switch]/[case]
-- for data-driven branching. See text.lua for interactive user choices.

local scheduler = {}

-- Error values may have a __tostring metamethod which also throws. Keep
-- diagnostics usable without letting formatting escape the error boundary.
function scheduler.error_text(value)
    local ok, message = pcall(tostring, value)
    if ok then return message end
    return "non-string Lua error (" .. type(value) .. "; tostring failed)"
end

-- Hot path: hoisted schema reference (isMigrated/coerce run per token);
-- require() itself is cached but the per-token lookup still costs.
local schemaModule = require("kag.schema")
local capabilityRuntime = require("capability_runtime")
local invoke_capable = capabilityRuntime.invoke_command
-- Compile-time front-end (Phase A): hoisted module reference -- the
-- scheduler compiles every token stream once (idempotent per stream).
local compiler = require("kag.compiler")

-- Rollback snapshots share the token stream and macro definitions. Retire
-- checkpoints before mutating either reference; static AOT calls do not pass
-- through the runtime expansion branch and retain their normal history.
local function clear_macro_history(ctx)
    ctx._undoStack = {}
end

-- ──── Flow-control command set (handled inline, never dispatched to kag table) ────

local flow_commands = {
    ["if"] = true, ["else"] = true, ["endif"] = true,
    ["switch"] = true, ["case"] = true, ["endswitch"] = true,
    ["jump"] = true, ["goto"] = true, ["call"] = true, ["return"] = true,
    ["link"] = true, ["end"] = true,
    ["label"] = true,
    ["macro"] = true, ["endmacro"] = true, ["erasemacro"] = true,
    ["eval"] = true, ["emb"] = true,
    ["stop"] = true,
}

-- ???? Internal helpers ????????????????????????????????????????????????????????????????????????????????????????????????????????????????

-- Neo-Genesis: label index. One pass per scene build (at load) turns
-- [jump *label] from O(n) scan into O(1) lookup. The index is rebuilt
-- whenever a macro splice mutates the stream (see invalidate below).
-- Exported for tests and tooling (ks_check could reuse it); the
-- scheduler itself calls the local.
-- Scene-path allowlist for cross-scene jumps (security: jump/call/link
-- concatenate target onto assets/script/ and tokenizer.parse_file does
-- io.open -- a crafted target like ../evil.ks would traverse out of the
-- game dir and EXECUTE an arbitrary .ks as a scene. Same predicate
-- family as SaveCommands._safeScenePath; kept local to avoid a
-- dependency cycle (scheduler is the bottom of the stack).
local function is_safe_scene_path(path)
    if type(path) ~= "string" or #path == 0 then return false end
    if path:find("..", 1, true) then return false end
    if path:find("^assets/script/") ~= 1 then return false end
    return path:find("%.ks$") ~= nil
end

-- [round 98] Cross-scene switch budget (dead-loop / overflow guard). A script
-- that ping-pongs scenes (A<->B via [jump]/[call]/[link]) or nests cross-scene
-- [call] chains would otherwise run forever under the external frame loop's
-- re-spawn (each cross-scene [jump]/[link] dies the coroutine and update()
-- re-spawns it) or grow ctx.call_stack without bound within one run ([call]
-- swaps tokens inline and never returns). Session-scoped counter: reset ONLY
-- on a fresh kag_runner.start (a per-run reset would clear it on every scene
-- swap and defeat the guard). Each allowed cross-scene switch increments it;
-- returning false means the budget is exhausted and the caller must WARN +
-- fall through (do not switch) -- the same cut-and-WARN shape as the round-84
-- _backJumps backward-jump guard. Purposely generous so no authored game trips
-- it, tight enough to stop pathological churn quickly.
local SCENE_SWITCH_MAX = 4096
local function budget_scene_switch(ctx)
    if type(ctx) ~= "table" then return false end
    local n = tonumber(ctx._sceneSwitches) or 0
    n = n + 1
    ctx._sceneSwitches = n
    return n <= SCENE_SWITCH_MAX
end

local function build_label_index(tokens)
    local idx = {}
    for i, tok in ipairs(tokens) do
        if tok[1] == "label" and tok[2] and type(tok[2].name) == "string" then
            idx[tok[2].name] = idx[tok[2].name] or i  -- first wins (KAG3)
        end
    end
    return idx
end

local function find_label(tokens, name, label_index)
    if label_index and label_index[name] then
        return label_index[name]
    end
    for i, tok in ipairs(tokens) do
        if tok[1] == "label" and tok[2] and tok[2].name == name then
            return i
        end
    end
    return nil
end

-- Shared [if]/[elseif]/[while]/[for] expression evaluation.
-- Neo-Genesis: delegates to kag/expr.lua, which translates TJS-style
-- operators (&& || ! != ?:) into Lua and reports compile/runtime errors
-- visibly (scene:line) instead of silently taking the else branch.
local exprLang = require("kag.expr")
local function eval_expr(ctx, expr)
    return exprLang.evaluate(ctx, expr)
end
-- Compiled streams carry the pre-translated (TJS->Lua) source: evaluate
-- directly without re-translating (the compiler did it once at load).
-- Optional `dump` (Battle 1c): precompiled bytecode from _compiled.exprDumps
-- for this token — evaluateTranslated loads it with mode "b" (no parse).
local function eval_expr_translated(ctx, translated, original, dump)
    return exprLang.evaluateTranslated(ctx, translated, original, dump)
end
local function skip_to(tokens, start_idx, targets, closers)
    -- closers: which target commands CLOSE the enclosing chain (only
    -- endif does; elseif/else are branch markers and must not decrement
    -- the depth -- otherwise an outer-false [if] lands on an inner
    -- chain's endif and double-executes the outer body).
    closers = closers or targets
    local depth = 1
    for i = start_idx + 1, #tokens do
        local cmd = tokens[i][1]
        if targets[cmd] then
            if depth == 1 then return i end
            if closers[cmd] then depth = depth - 1 end
        elseif targets.opens and targets.opens[cmd] then
            depth = depth + 1
        end
    end
    return #tokens
end

-- ???? Main execution loop ??????????????????????????????????????????????????????????????????????????????????????????????????????????

function scheduler.run(ctx, tokens, start_index)
    if not tokens or #tokens == 0 then return end
    -- Compile-time front-end (Phase A): normalizes token format + params,
    -- builds the label index and the flow jump table ONCE. Hand-built
    -- token arrays (tests, macro splices) compile lazily on first run;
    -- a compiled stream keeps the side table for subsequent resumes.
    compiler.compile(tokens)
    local compiled = tokens._compiled
    -- Hoist the side-table fields to locals: the hot loop touches
    -- handlers/flow/exprs per token -- `compiled.handlers[i]` is a
    -- two-level hash lookup; the local is a single-level one (measured
    -- ~4x faster in the 2000-token dispatch benchmark).
    local compiled_handlers = compiled.handlers
    local compiled_flow = compiled.flow
    local compiled_exprs = compiled.exprs
    local compiled_exprDumps = compiled.exprDumps

    -- Lazy label index AFTER normalization: build_label_index expects the
    -- array format ({"label", {name=...}}), not the tokenizer's raw
    -- records -- building too early produced a sticky EMPTY index.
    -- (compiler.compile builds it eagerly; this only fills the legacy
    -- ctx field for callers that read ctx.label_index directly.)
    if ctx and ctx.label_index == nil then
        ctx.label_index = compiled.labels
    end
    -- if/elseif chain state: each entry tracks whether a branch was TAKEN
    -- so later elseif/else in the SAME chain are skipped (nested chains
    -- are independent via the stack).
    -- Session-local control-flow stacks hoisted to ctx (round 75): the
    -- [save] snapshot serializes them (capture_state) and a [load]
    -- resume restores them into ctx._resumeLoopStacks. They are run
    -- locals otherwise, and a [load] re-spawns scheduler.run -- the
    -- restored run would start with empty stacks, so the first [endfor]/
    -- [endwhile] became a no-op and any enclosing loop silently ended
    -- (round-74 defect report). [endif]/[endswitch] would likewise lose
    -- their chain/branch state. kag_runner.start resets all four for a
    -- fresh session; every other run entry either restores the marker
    -- (post-load) or inherits the live tables (cross-scene [jump]/[call]
    -- swap tokens mid-run -- the stacks must survive those).
    local resume_st = ctx._resumeLoopStacks
    if resume_st then
        ctx._resumeLoopStacks = nil
        if type(resume_st.if_) == "table" then ctx._ifStack = resume_st.if_ end
        if type(resume_st.switch) == "table" then ctx._switchStack = resume_st.switch end
        if type(resume_st.while_) == "table" then ctx._whileStack = resume_st.while_ end
        if type(resume_st.for_) == "table" then ctx._forStack = resume_st.for_ end
    end
    local if_stack = ctx._ifStack or {}
    ctx._ifStack = if_stack
    -- KAG3 variable frames: ctx.lf is the local (call-frame) variable
    -- table -- [call] pushes a fresh frame, [return] pops it. ctx.mp is
    -- the message-parameter table (KAG3 [call ... mp="x"]); ensure both
    -- exist for expression envs and interpolation.
    if ctx.lf == nil then ctx.lf = {} end
    if ctx.mp == nil then ctx.mp = {} end
    -- switch/case state: taken case bodies must not fall through into
    -- later cases (KAG3 semantics -- a matched case runs alone).
    local switch_stack = ctx._switchStack or {}
    ctx._switchStack = switch_stack
    -- while state. Bounded -- a runaway loop in a script (a variable
    -- that never changes) must not hang the runner. KAG3 had no bound;
    -- Neo-Genesis caps iterations and errors loudly.
    local while_stack = ctx._whileStack or {}
    ctx._whileStack = while_stack
    -- for-loop state (numeric counter loops, same bounded regime).
    local for_stack = ctx._forStack or {}
    ctx._forStack = for_stack
    local WHILE_MAX_ITERS = 65536

    local function control_frame()
        return {
            if_ = if_stack, switch = switch_stack,
            while_ = while_stack, for_ = for_stack,
            for_marks = ctx._forStackMarks or {},
            for_rewound = ctx._forRewound or {},
        }
    end

    local function use_control_frame(frame)
        frame = frame or {}
        if_stack, switch_stack = frame.if_ or {}, frame.switch or {}
        while_stack, for_stack = frame.while_ or {}, frame.for_ or {}
        ctx._ifStack, ctx._switchStack = if_stack, switch_stack
        ctx._whileStack, ctx._forStack = while_stack, for_stack
        ctx._forStackMarks = frame.for_marks or {}
        ctx._forRewound = frame.for_rewound or {}
    end

    -- [round 84 C2] Per-run backward-jump cycle memory (macro re-expansion
    -- guard -- keyed by "origin>target" for BACKWARD intra-scene jumps). A
    -- fresh scene pass starts clean; [jump]/[call] across scenes re-enter
    -- via scheduler.run and get their own table.
    ctx._backJumps = {}

    local kag = require("kag")
    local operation = require("kag.operation")
    local kagDebug = require("kag_debug")
    start_index = start_index or 1

    -- Cross-scene [call]/[return]/[jump]/[link] swap the token stream
    -- mid-loop; the compiled side table follows the stream, so re-read it
    -- after every swap (compile is idempotent: a fresh stream compiles
    -- once, a compiled stream is a no-op).
    local function refresh_compiled()
        compiler.compile(tokens)
        compiled = tokens._compiled
        compiled_handlers = compiled.handlers
        compiled_flow = compiled.flow
        compiled_exprs = compiled.exprs
        compiled_exprDumps = compiled.exprDumps
    end

    -- t49: after the [return] restore (explicit or implicit) the CALLER's
    -- labelMap is current again. A deferred [select]/[button] jump created
    -- inside the scene being LEFT carries a SCENE-LOCAL label: if that label
    -- does not exist in the restored scene, resolving it later (runner dead
    -- branch) printed "Choice label not found" and stalled not-running
    -- forever (t46 boundary finding on the t43 change). Drop it loudly here
    -- -- the t38 drop semantics, now scoped to the actually dangling case;
    -- a label that DOES exist (choice made in the caller, call returns) is
    -- kept and its legitimate replay is untouched (t43 Case C).
    local function prune_dangling_pending_jump()
        local pj = ctx._pendingJump
        if type(pj) ~= "string" or pj:sub(1, 1) ~= "*" then return end
        local lbl = pj:gsub("^*", "")
        local idx = compiled.labels[lbl] or find_label(tokens, lbl, ctx.label_index)
        if not idx then
            print("[KAG Runner] Dropping deferred choice jump: label " .. pj
                  .. " not in restored scene "
                  .. tostring(ctx.current_scene or ctx.currentScene or "?")
                  .. " (choice labels are scene-local)")
            ctx._pendingJump = nil
        end
    end

    local i = start_index
    -- A save taken by the callee's final command resumes just after its last
    -- token. Perform the pending implicit return before entering the loop.
    while i > #tokens and ctx.call_stack and #ctx.call_stack > 0 do
        local frame = table.remove(ctx.call_stack)
        ctx.label_index = frame.label_index
        ctx.current_scene, ctx.currentScene = frame.scene, frame.scene
        if frame.lf ~= nil then ctx.lf = frame.lf end
        if frame.mp ~= nil then ctx.mp = frame.mp end
        if frame.control then use_control_frame(frame.control) end
        tokens, ctx.tokens = frame.tokens, frame.tokens
        refresh_compiled()
        ctx.labelMap = ctx.label_index or compiled.labels
        i = frame.index or 1
        ctx.token_index, ctx._resume_index = math.max(1,i-1), i
        ctx._scene_changed = false
        prune_dangling_pending_jump()
    end
    while i <= #tokens do
        ctx._executing_index = i
        ctx._executing_command = tokens[i][1] or tokens[i].cmd
        local tok = tokens[i]
        local cmd = tok[1]
        -- KAG3 command-name aliases (elsif -> elseif): tokens produced by
        -- tokenizer.parse are already normalized, but macro bodies and
        -- hand-built token streams may not be -- normalize defensively.
        if cmd == "elsif" then cmd = "elseif" end
        local params = tok[2] or {}

        -- KAG scene debugger (Neo-Genesis): breakpoint/step check BEFORE
        -- dispatch. A hit yields "__kag_pause" and waits for the runner
        -- (or RPC) to resume with "continue" or "step"); the token is then
        -- executed as usual. Zero cost when no breakpoints are armed.
        if kagDebug.check(ctx, cmd, i) == "pause" then
            kagDebug.set_paused(true)
            local resume_act = coroutine.yield("__kag_pause")
            kagDebug.set_paused(false)
            if resume_act == "step" then kagDebug.step() end
            if ctx.stop_flag then return end
        end

        -- Check stop flag
        if ctx.stop_flag then return end

        -- Check for Lua-initiated flow control (from [iscript] or external Lua)
        if ctx._next_index then
            i = ctx._next_index
            ctx._next_index = nil
        end

        -- Flow control: [jump]/[goto] (goto = KAG3 alias of jump)
        if cmd == "jump" or cmd == "goto" then
            -- bare [jump next.ks] -> params[1] (KAG3 syntax); STRING-only:
            -- with named params (even a typo), params[1] is the raw pair
            -- table and target:sub would throw (review should-fix)
            local target = params.target or params.label or params.storage
            if type(params[1]) == "string" then target = target or params[1] end
            if not target then
                print("[WARN] [jump] missing target/label/storage parameter")
            elseif target:sub(1,1) ~= "*" then
                -- Cross-scene jump: load new scene file (bare [jump x.ks]
                -- and named target= both land here -- audit fix)
                local path = "assets/script/" .. target
                if not is_safe_scene_path(path) then
                    print("[WARN] [jump] blocked scene path: " .. path)
                elseif not budget_scene_switch(ctx) then
                    -- [round 98] cross-scene switch budget: an A<->B [jump]
                    -- ping-pong would otherwise spin forever under the external
                    -- frame loop (each jump ends the run, but the runner
                    -- re-spawns it). WARN + fall through -- do NOT switch.
                    print("[WARN] [jump] cross-scene switch budget exceeded at "
                        .. tostring(ctx.current_scene or "?")
                        .. " (possible A<->B ping-pong); switch cut")
                else
                    local new_tokens = ctx.load_tokens and ctx.load_tokens(path)
                    if new_tokens then
                        ctx.tokens = new_tokens
                        ctx.token_index = 1
                        ctx.current_scene = path
                        ctx.call_stack = {}
                        ctx.lf = {}  -- jump starts a fresh local frame
                        ctx.layers = {}
                        ctx.backlog = {}
                        ctx.label_index = nil  -- raw tokens: entry lazy-builds
                        -- [round 98 C] A cross-scene jump is an arbitrary
                        -- control-flow transfer exactly like an intra-scene
                        -- jump: any loop/summary stacks still live at the
                        -- jump site describe a loop body we are now leaving
                        -- for an unrelated scene. Reset them symmetrically
                        -- with the intra-scene branch (round 83 A4/A5) so
                        -- (a) no stale entries leak into later loops and
                        -- (b) a LATER same-name [for] in the new scene
                        -- re-initializes its counter from its declared
                        -- start instead of reusing the stale marker from
                        -- the jumped-away scene (observed: 11 iterations
                        -- instead of 2 across a scene boundary).
                        use_control_frame()
                        operation.cancel_all(ctx)
                        return
                    else
                        print("[WARN] [jump] failed to load scene: " .. path)
                    end
                end
            else
                -- Intra-scene jump: find label (target may be "label" or "*label")
                -- Compiled streams carry the O(1) label index; fall back to
                -- the scan for hand-built streams (macro splices etc).
                local label = target:gsub("^*", "")  -- strip leading * if present
                local idx = compiled.labels[label]
                if not idx then
                    idx = find_label(tokens, label, ctx.label_index)
                end
                if idx then
                    -- [round 84 C2] macro re-expansion loop guard.
                    -- A static-safe macro (defined at top level, no erase /
                    -- redefinition) is INLINED at compile time, so the macro
                    -- body's [goto] runs with an EMPTY runtime _macroStack and
                    -- the round-75 depth guard never fires -- the C2 script
                    -- bounced forever re-expanding the same body. The hazard
                    -- is specifically a BACKWARD jump (target label at-or-
                    -- before the current position) whose re-traversal re-enters
                    -- the inlined body and re-takes the SAME backward-edge,
                    -- so the goto and its siblings loop. A dynamic macro that
                    -- splices at runtime hits the identical edge through its
                    -- own body tokens, so this guard covers both morphologies.
                    -- Forward jumps (idx > i) and the FIRST take of a backward
                    -- edge still land normally; only REPEATING the same
                    -- backward edge (same origin token -> same target label)
                    -- is cut, with a WARN instead of a hang. (Round 83 A4/A5
                    -- stack resets below apply only on a TAKEN jump.)
                    ctx._backJumps = ctx._backJumps or {}
                    if idx <= i then
                        local key = i .. ">" .. idx
                        if ctx._backJumps[key] then
                            print("[WARN] [jump] backward goto into macro body -- "
                                .. "macro re-expansion loop prevented (target *"
                                .. label .. ", from token " .. i .. ")")
                        else
                            ctx._backJumps[key] = true
                            i = idx
                        end
                    else
                        i = idx
                    end
                    if i == idx then
                        -- [round 83 A4/A5] An intra-scene jump is an arbitrary
                        -- control-flow transfer: any loop/summary stacks still
                        -- live at the jump site describe a loop body we are now
                        -- leaving. Reset them so (a) no stale entries leak into
                        -- later loops and (b) a LATER same-name [for] re-initializes
                        -- its counter from its declared start instead of reusing
                        -- the stale marker (observed: 11 iterations instead of 2).
                        ctx._forStack = {}
                        ctx._whileStack = {}
                        ctx._ifStack = {}
                        ctx._switchStack = {}
                        ctx._forStackMarks = {}
                    end
                else
                    print("[WARN] [jump] label not found: " .. label)
                end
            end

        -- Flow control: [call]
        elseif cmd == "call" then
            -- bare [call next.ks] -> params[1] (string-only guard)
            local target = params.target or params.storage
            if type(params[1]) == "string" then target = target or params[1] end
            if not target then
                print("[WARN] [call] missing target/storage parameter")
            elseif target:sub(1, 1) == "*" then
                -- Intra-scene call (KAG3): push the current position and
                -- jump to the label; [return] pops back. lf gets a fresh
                -- frame exactly like cross-scene calls.
                local label = target:gsub("^*", "")
                local idx = compiled.labels[label]
                if not idx then
                    idx = find_label(tokens, label, ctx.label_index)
                end
                if idx then
                    ctx.call_stack = ctx.call_stack or {}
                    table.insert(ctx.call_stack, {
                        tokens = tokens, index = i + 1,
                        label_index = ctx.label_index,
                        scene = ctx.current_scene,
                        lf = ctx.lf,
                        mp = ctx.mp,
                        control = control_frame(),
                    })
                    ctx.lf = {}
                    use_control_frame()
                    i = idx
                else
                    print("[WARN] [call] label not found: " .. label)
                end
            else
            local path = "assets/script/" .. target
            local new_tokens = nil
            if not is_safe_scene_path(path) then
                print("[WARN] [call] blocked scene path: " .. path)
            else
                new_tokens = ctx.load_tokens and ctx.load_tokens(path)
            end
            if new_tokens and budget_scene_switch(ctx) then
                ctx.call_stack = ctx.call_stack or {}
                table.insert(ctx.call_stack, {
                    tokens = tokens, index = i + 1,
                    label_index = ctx.label_index,
                    scene = ctx.current_scene,
                    lf = ctx.lf,          -- restore on [return] (KAG3 lf frame)
                    mp = ctx.mp,
                    control = control_frame(),
                })
                ctx.lf = {}               -- fresh local frame for the callee
                use_control_frame()
                tokens = new_tokens
                ctx.tokens = tokens
                ctx.current_scene = path
                ctx.label_index = nil  -- raw tokens: run() entry rebuilds
                refresh_compiled()
                i = 0
            elseif new_tokens then
                -- [round 98] cross-scene switch budget: an A<->B [call] chain
                -- would grow ctx.call_stack without bound (cross-scene calls
                -- swap tokens inline and never return) and spin. WARN + fall
                -- through -- do NOT push a frame or switch.
                print("[WARN] [call] cross-scene switch budget exceeded ("
                    .. tostring(ctx.current_scene or "?") .. " -> " .. path
                    .. "); call cut")
            end
            end

        -- Flow control: [return]
        elseif cmd == "return" then
            ctx.call_stack = ctx.call_stack or {}  -- mirror the [call] guard
            local frame = table.remove(ctx.call_stack)
            if frame then
                ctx.label_index = frame.label_index  -- caller scene again
                ctx.current_scene = frame.scene       -- caller scene NAME again
                if frame.lf ~= nil then ctx.lf = frame.lf end  -- pop lf frame
                if frame.mp ~= nil then ctx.mp = frame.mp end
                if frame.control then use_control_frame(frame.control) end
            end
            if frame then
                tokens = frame.tokens
                ctx.tokens = tokens
                refresh_compiled()
                i = frame.index - 1
                -- t43: a cross-scene [call] set _scene_changed via load_tokens
                -- (to ask update() to re-spawn after a run-ending switch), but
                -- the coroutine never died -- the call ran inline and we are
                -- now back in the CALLER's scene. The switch signal is moot;
                -- leaving it set made the runner's dead-coroutine branch drop
                -- a still-valid deferred [select] jump (t40 regression).
                ctx._scene_changed = false
                -- t49: a deferred choice jump left DANGLING by a callee-internal
                -- [select] (never consumed before [return]) targets a callee
                -- label that cannot exist in the restored caller scene -- drop
                -- it loudly here instead of stalling at the runner's dead
                -- branch ("Choice label not found: <label>", FRAME_LIMIT).
                prune_dangling_pending_jump()
            else
                return  -- No call stack, end execution
            end

        -- Flow control: [link]
        elseif cmd == "link" then
            -- (index rebuilt below with the swapped stream)
            -- bare [link next.ks] -> params[1] (string-only guard)
            local target = params.target or params.storage
            if type(params[1]) == "string" then target = target or params[1] end
            if not target then
                -- review should-fix: a typo'd [link] must NOT wipe
                -- layers/backlog/call_stack -- WARN and keep scene state
                print("[WARN] [link] missing target/storage parameter")
            else
            local path = "assets/script/" .. target
            local new_tokens = nil
            if not is_safe_scene_path(path) then
                print("[WARN] [link] blocked scene path: " .. path)
            elseif not budget_scene_switch(ctx) then
                -- [round 98] cross-scene switch budget: WARN + fall through
                -- and, unlike the normal path, do NOT wipe layers/backlog/
                -- call_stack -- there is no scene to link into.
                print("[WARN] [link] cross-scene switch budget exceeded ("
                    .. tostring(ctx.current_scene or "?") .. " -> " .. path
                    .. "); link cut")
            else
                -- security info: clear ONLY after the allowlist accepts
                -- (a blocked traversal link must not wipe scene state)
                ctx.layers = {}
                ctx.backlog = {}
                operation.cancel_all(ctx)
                ctx.call_stack = {}
                ctx.lf = {}  -- new scene, new local frame
                new_tokens = ctx.load_tokens and ctx.load_tokens(path)
            end
            if new_tokens then
                tokens = new_tokens
                ctx.tokens = tokens
                ctx.token_index = 1
                ctx.current_scene = path
                ctx.label_index = nil  -- raw tokens: run() entry rebuilds
                refresh_compiled()
                i = 0
            end
            end

        -- Flow control: [end]
        elseif cmd == "end" then
            return

        -- Flow control: [stop] -- immediate stop (audit: flow_commands
        -- declared stop but no branch handled it; it fell through to the
        -- unknown-command path and rendered as dialogue text). Identical
        -- to [end] (both terminate the coroutine); kept separate so
        -- scripts can use the KAG3 spelling.
        elseif cmd == "stop" then
            return

        -- Flow control: [label] ?? no-op, used by jump/call
        elseif cmd == "label" then
            -- pass

        -- Flow control: [if]/[else]/[endif]
        elseif cmd == "if" then
            -- Compiled streams carry the pre-translated (TJS->Lua) source;
            -- runtime evaluate re-translates (idempotent) and caches the
            -- compiled chunk keyed by env identity, so the hot path pays
            -- only the cache lookup.
            local src = compiled_exprs[i]
            local ok, result
            if src then
                ok, result = eval_expr_translated(ctx, src, params.exp,
                    compiled_exprDumps and compiled_exprDumps[i])
            else
                ok, result = eval_expr(ctx, params.exp or "false")
            end
            local taken = ok and not not result or false
            if_stack[#if_stack + 1] = taken
            if not taken then
                -- Skip to the next elseif/else/endif; the target is NOT
                -- consumed (i-1) so an elseif evaluates its own expression.
                -- Compiled streams jump O(1); hand-built fall back to scan.
                local f = compiled_flow[i]
                if f then
                    i = f.jump_false - 1
                else
                    i = skip_to(tokens, i, {
                        ["elseif"] = true, ["elsif"] = true, ["else"] = true, ["endif"] = true,
                        opens = {["if"] = true}
                    }, {["endif"] = true}) - 1
                end
            end

        elseif cmd == "elseif" then
            local taken = if_stack[#if_stack]
            if taken then
                -- A previous branch was taken: skip the rest of the chain.
                local f = compiled_flow[i]
                if f then
                    i = f.jump_chain_end - 1
                else
                    i = skip_to(tokens, i, {
                        ["elseif"] = true, ["elsif"] = true, ["else"] = true, ["endif"] = true,
                        opens = {["if"] = true}
                    }, {["endif"] = true}) - 1
                end
            else
                local src = compiled_exprs[i]
                local ok, result
                if src then
                    ok, result = eval_expr_translated(ctx, src, params.exp,
                        compiled_exprDumps and compiled_exprDumps[i])
                else
                    ok, result = eval_expr(ctx, params.exp or "false")
                end
                taken = ok and not not result or false
                if_stack[#if_stack] = taken
                if not taken then
                    local f = compiled_flow[i]
                    if f then
                        i = f.jump_chain_end - 1
                    else
                        i = skip_to(tokens, i, {
                            ["elseif"] = true, ["elsif"] = true, ["else"] = true, ["endif"] = true,
                            opens = {["if"] = true}
                        }, {["endif"] = true}) - 1
                    end
                end
            end

        elseif cmd == "else" then
            local taken = if_stack[#if_stack]
            if taken then
                local f = compiled_flow[i]
                if f then
                    i = f.jump_chain_end - 1  -- do not consume: endif pops
                else
                    i = skip_to(tokens, i, {
                        ["endif"] = true,
                        opens = {["if"] = true}
                    }) - 1  -- do not consume: endif must pop the chain
                end
            end
            -- (not taken: execute the else body -- fall through)

        elseif cmd == "endif" then
            if_stack[#if_stack] = nil  -- pop the chain

        
        -- Flow control: [while]/[endwhile] -- bounded data-driven loops.
        elseif cmd == "while" then
            -- Total-iteration guard, PER SCENE: entries are popped each
            -- iteration, so the bound must live outside the stack; and a
            -- session-global counter would permanently disable [while]
            -- after 65k total executions across scenes (security LOW).
            -- Keying by scene lets each scene budget its own 65k.
            local wscene = ctx.current_scene or "?"
            ctx._whileIterByScene = ctx._whileIterByScene or {}
            ctx._whileIterByScene[wscene] =
                (ctx._whileIterByScene[wscene] or 0) + 1
            if ctx._whileIterByScene[wscene] > WHILE_MAX_ITERS then
                error("[while] exceeded " .. WHILE_MAX_ITERS
                    .. " total iterations in scene '" .. wscene
                    .. "' (bounded loop guard)", 0)
            end
            local src = compiled_exprs[i]
            local ok, result
            if src then
                ok, result = eval_expr_translated(ctx, src, params.exp,
                    compiled_exprDumps and compiled_exprDumps[i])
            else
                ok, result = eval_expr(ctx, params.exp or "false")
            end
            -- ALWAYS push (ended flag): the matching endwhile must know
            -- whether the loop is still live (rewind) or was skipped
            -- because the condition turned false (pop and continue).
            while_stack[#while_stack + 1] = {
                pos = i,  -- loop head: endwhile rewinds to i-1
                ended = not (ok and result),
            }
            if not (ok and result) then
                -- Skip the body: depth-aware (nested while has its own
                -- endwhile). i stops on the endwhile so it pops.
                local f = compiled_flow[i]
                if f then
                    i = f.skip_body_to - 1
                else
                    i = skip_to(tokens, i, {
                        ["endwhile"] = true,
                        opens = {["while"] = true}
                    }, {["endwhile"] = true}) - 1
                end
            end

        elseif cmd == "endwhile" then
            local w = while_stack[#while_stack]
            if w then
                if w.ended then
                    while_stack[#while_stack] = nil  -- loop over: pop
                else
                    -- POP before rewinding: the next [while] re-pushes a
                    -- fresh entry. Without this, a live loop's entry stays
                    -- on the stack and the ENDWHILE of an OUTER loop grabs
                    -- the stale inner entry (pos) and rewinds into the
                    -- inner body -- an infinite inner re-run.
                    -- (The per-entry iters guard was removed: ctx's total
                    -- iteration guard always fires first -- dead code.)
                    while_stack[#while_stack] = nil
                    i = w.pos - 1  -- loop head: re-evaluate next iteration
                end
            end

        -- Flow control: [for var="i" start="0" end="3" step="1"]...[endfor]
        -- numeric loops (Neo-Genesis; KAG3 had no counter loop). Shares
        -- the while per-scene guard: a step=0 loop cannot hang the runner.
        elseif cmd == "for" then
            -- Shared total-iteration guard (per scene, same as [while]).
            local wscene = ctx.current_scene or "?"
            ctx._whileIterByScene = ctx._whileIterByScene or {}
            ctx._whileIterByScene[wscene] =
                (ctx._whileIterByScene[wscene] or 0) + 1
            if ctx._whileIterByScene[wscene] > WHILE_MAX_ITERS then
                error("[for] exceeded " .. WHILE_MAX_ITERS
                    .. " total iterations in scene '" .. wscene
                    .. "' (bounded loop guard)", 0)
            end
            local vname = params.var or "i"
            -- compiler translates start/end/step to Lua at compile time;
            -- evaluateTranslated skips the idempotent re-translation.
            -- AOT dumps (Battle 1c) skip the per-env source load; [for]
            -- stores per-expression dumps (start/end/step sub-table).
            local forDumps = compiled_exprDumps and compiled_exprDumps[i]
            local ok0, sval = eval_expr_translated(ctx,
                tostring(params.start or "0"), tostring(params.start or "0"),
                forDumps and forDumps.start)
            local ok1, eval = eval_expr_translated(ctx,
                tostring(params["end"] or "0"), tostring(params["end"] or "0"),
                forDumps and forDumps["end"])
            local ok2, sstep = eval_expr_translated(ctx,
                tostring(params.step or "1"), tostring(params.step or "1"),
                forDumps and forDumps.step)
            if ok0 and ok1 and ok2 then
                local sv = tonumber(sval) or 0
                local ev = tonumber(eval) or 0
                local sp = tonumber(sstep) or 1
                if sp == 0 then sp = 1 end  -- degenerate step: treat as 1
                ctx.f = ctx.f or {}
                -- FIRST entry sets the counter; a re-entry (endfor
                -- rewound here) must KEEP the incremented value or the
                -- loop restarts from start forever. Same-named nested
                -- loops are a script anti-pattern (documented).
                ctx._forStackMarks = ctx._forStackMarks or {}
                ctx._forRewound = ctx._forRewound or {}
                -- Stack-counted marks (round 63): a same-name NESTED [for]
                -- must not clear the OUTER loop's mark when it ends —
                -- previously the inner endfor set the name to nil, so the
                -- outer loop re-initialized its counter on re-entry and
                -- never terminated. The counter initializes only when no
                -- live loop owns the name; a rewind re-entry (flagged by
                -- endfor) neither re-initializes nor adds a level.
                local m0 = ctx._forStackMarks[vname] or 0
                if ctx._forRewound[vname] then
                    ctx._forRewound[vname] = nil
                else
                    if m0 == 0 then
                        ctx.f[vname] = sv
                        ctx._forStackMarks[vname] = 1
                    else
                        ctx._forStackMarks[vname] = m0 + 1
                    end
                end
                for_stack[#for_stack + 1] = {
                    var = vname, endv = ev, step = sp, pos = i,
                    ended = not ((sp > 0 and sv <= ev) or (sp < 0 and sv >= ev)),
                }
                if for_stack[#for_stack].ended then
                    -- start already past the end: skip the body
                    local f = compiled_flow[i]
                    if f then
                        i = f.skip_body_to - 1
                    else
                        i = skip_to(tokens, i, {
                            ["endfor"] = true,
                            opens = {["for"] = true}
                        }, {["endfor"] = true}) - 1
                    end
                end
            else
                -- bad numbers: skip the body (evaluated loudly already)
                local f = compiled_flow[i]
                if f then
                    i = f.skip_body_to - 1
                else
                    i = skip_to(tokens, i, {
                        ["endfor"] = true,
                        opens = {["for"] = true}
                    }, {["endfor"] = true}) - 1
                end
            end

        elseif cmd == "endfor" then
            local w = for_stack[#for_stack]
            if w then
                if w.ended then
                    for_stack[#for_stack] = nil  -- loop over: pop
                    -- An empty/skipped loop set the mark too: clear it or
                    -- a later same-name [for] reuses the stale counter
                    -- (round 63: count-based so a nested same-name loop
                    -- only decrements its own level).
                    if ctx._forStackMarks then
                        local mc = (ctx._forStackMarks[w.var] or 1) - 1
                        if mc <= 0 then
                            ctx._forStackMarks[w.var] = nil
                        else
                            ctx._forStackMarks[w.var] = mc
                        end
                    end
                else
                    local cur = tonumber(ctx.f and ctx.f[w.var]) or 0
                    cur = cur + w.step
                    ctx.f[w.var] = cur
                    local over = (w.step > 0 and cur > w.endv)
                        or (w.step < 0 and cur < w.endv)
                    if over then
                        for_stack[#for_stack] = nil  -- done: pop
                        if ctx._forStackMarks then
                            local mc = (ctx._forStackMarks[w.var] or 1) - 1
                            if mc <= 0 then
                                ctx._forStackMarks[w.var] = nil
                            else
                                ctx._forStackMarks[w.var] = mc
                            end
                        end
                    else
                        for_stack[#for_stack] = nil  -- pop; next [for] re-pushes
                        ctx._forRewound = ctx._forRewound or {}
                        ctx._forRewound[w.var] = true  -- re-entry keeps the mark
                        i = w.pos - 1  -- loop head
                    end
                end
            end

        -- Flow control: [until exp="..." timeout=ms] — declarative
        -- conditional wait (Neo-Genesis; Ren'Py needs python loops for
        -- this). Waits until the expression is truthy or timeout elapses;
        -- per-frame yield so input/rendering keep running. The exp is
        -- AOT-translated at compile time (same path as [if]/[while]).
        elseif cmd == "until" then
            local uexp = params.exp
            local timeout = tonumber(params.timeout) or 60000
            if timeout < 0 then timeout = 0 end
            if type(uexp) == "string" and uexp ~= "" then
                -- Per-scene frame guard (round 59): [until] yields every
                -- frame so it cannot hang the main thread, but a huge
                -- timeout with a never-true exp would poll for a very
                -- long time. Bound the total frames waited per scene the
                -- same way [while]/[for] bound iterations
                -- (WHILE_MAX_ITERS).
                local wscene = ctx.current_scene or "?"
                ctx._untilFramesByScene = ctx._untilFramesByScene or {}
                local op <close> = operation.start(ctx)
                local ct = op.token
                local elapsed = 0
                local frameTime = 16
                -- Honor the main loop's flow controls: a scene abort
                -- (stop_flag), a Lua-initiated jump (_next_index) or an
                -- operation cancel ends the wait immediately instead of
                -- blocking until timeout (review should-fix).
                while not ct.cancelled and not ctx.stop_flag
                      and not ctx._next_index and elapsed < timeout do
                    local src = compiled_exprs[i]
                    local ok, v
                    if src then
                        ok, v = eval_expr_translated(ctx, src, uexp,
                            compiled_exprDumps and compiled_exprDumps[i])
                    else
                        ok, v = eval_expr(ctx, uexp)
                    end
                    -- a broken expression cannot become true: stop
                    -- polling (the diagnostic already printed once)
                    if not ok then break end
                    if v then break end
                    local dt = tonumber(coroutine.yield() or frameTime)
                        or frameTime
                    ctx._untilFramesByScene[wscene] =
                        (ctx._untilFramesByScene[wscene] or 0) + 1
                    if ctx._untilFramesByScene[wscene] > WHILE_MAX_ITERS then
                        error("[until] exceeded " .. WHILE_MAX_ITERS
                            .. " total frames in scene '" .. wscene
                            .. "' (bounded wait guard)", 0)
                    end
                    elapsed = elapsed + dt
                end
                if not ct.cancelled then op:complete() end
            end

        -- Flow control: [break]/[continue] inside while/for loops
        -- (Neo-Genesis; KAG3 had neither). break pops the innermost loop
        -- and jumps past its end token; continue skips the rest of the
        -- body so the end token runs (endfor increments / endwhile
        -- re-evaluates).
        elseif cmd == "break" or cmd == "continue" then
            local wl = while_stack[#while_stack]
            local fl = for_stack[#for_stack]
            local wpos = wl and wl.pos or -1
            local fpos = fl and fl.pos or -1
            if wpos < 0 and fpos < 0 then
                error("[" .. cmd .. "] outside a loop", 0)
            end
            -- the innermost loop is the one whose head token is LAST
            local inWhile = wpos > fpos
            local tgt = {
                ["endwhile"] = true, ["endfor"] = true,
                opens = {["while"] = true, ["for"] = true}
            }
            local cls = {["endwhile"] = true, ["endfor"] = true}
            if cmd == "break" then
                -- pop the loop we are leaving; its end token must NOT run
                -- (it would pop again / rewind into the body)
                if inWhile then
                    while_stack[#while_stack] = nil
                else
                    if ctx._forStackMarks then
                        local mc = (ctx._forStackMarks[fl.var] or 1) - 1
                        if mc <= 0 then
                            ctx._forStackMarks[fl.var] = nil
                        else
                            ctx._forStackMarks[fl.var] = mc
                        end
                        if ctx._forRewound then
                            ctx._forRewound[fl.var] = nil
                        end
                    end
                    for_stack[#for_stack] = nil
                end
                local f = compiled_flow[i]
                if f then
                    i = f.loop_end  -- compiled: index of the end token
                else
                    i = skip_to(tokens, i, tgt, cls)  -- +1 skips the end token
                end
            else  -- continue
                -- skip to the end token; -1 lets the loop process it
                local f = compiled_flow[i]
                if f then
                    i = f.loop_end - 1
                else
                    i = skip_to(tokens, i, tgt, cls) - 1
                end
            end

        -- Flow control: [switch]/[case]/[default]/[endswitch] (spec [1.4])
        elseif cmd == "switch" then
            -- switch/case dispatch (Alpha: flat only, single-level)
            -- Selector (round 55): named exp= is a TJS expression compiled
            -- at load (same machinery as [if]); otherwise the KAG3
            -- positional form is a bare variable name. Comparison stays
            -- tostring equality for both forms ([case 100] matches
            -- exp="f.gold" yielding "100").
            local src = compiled_exprs[i]
            local switchVal
            if src then
                local ok, result = eval_expr_translated(ctx, src, params.exp,
                    compiled_exprDumps and compiled_exprDumps[i])
                if ok and result ~= nil then
                    switchVal = tostring(result)
                else
                    -- expression failed or evaluated to nil: use the raw
                    -- text so the no-case/default path decides (never a
                    -- nil key)
                    switchVal = tostring(params.exp or "")
                end
            else
                -- Script variables live in ctx.f (the eval env, shared
                -- with [if]/[while]); ctx.variables is the legacy
                -- binding-layer table. f wins, variables is the fallback.
                local switchExpr = params[1] or ""
                if ctx.f and ctx.f[switchExpr] ~= nil then
                    switchVal = tostring(ctx.f[switchExpr])
                elseif ctx.variables and ctx.variables[switchExpr] ~= nil then
                    switchVal = tostring(ctx.variables[switchExpr])
                else
                    switchVal = switchExpr
                end
            end

            -- Find matching case. Compiled streams carry the depth-aware
            -- case table (case value -> body start, O(1) lookup; nested
            -- switches' cases never enter it); hand-built streams fall
            -- back to the depth-aware runtime scan.
            local caseStart = nil
            local f = compiled_flow[i]
            if f and f.cases then
                local target = f.cases[switchVal]
                if target then
                    caseStart = target
                elseif f.default then
                    caseStart = f.default
                end
            else
                local scanIdx = i + 1  -- start AFTER our own token
                local scanDepth = 1
                while scanIdx <= #tokens do
                    local stok = tokens[scanIdx]
                    local scmd = stok[1]
                    if scmd == "switch" then
                        scanDepth = scanDepth + 1
                    elseif scmd == "endswitch" then
                        if scanDepth == 1 then break end  -- OUR endswitch
                        scanDepth = scanDepth - 1
                    elseif scanDepth == 1 and scmd == "case" then
                        local caseParams = stok[2] or {}
                        local caseVal = caseParams[1] or ""
                        if tostring(caseVal) == switchVal then
                            caseStart = scanIdx + 1
                            break
                        end
                    elseif scanDepth == 1 and scmd == "default" then
                        caseStart = scanIdx + 1
                        break
                    end
                    scanIdx = scanIdx + 1
                end
            end

            -- ALWAYS push (taken flag): the matching endswitch pops OUR
            -- entry -- a no-match switch that skipped its body must not
            -- let its endswitch pop the OUTER switch's entry.
            if caseStart then
                switch_stack[#switch_stack + 1] = true
                i = caseStart - 1  -- loop's i+1 lands ON the first body token
            else
                switch_stack[#switch_stack + 1] = false  -- no case taken
                -- Skip to OUR endswitch, depth-aware (a nested switch in
                -- the skipped body has its own endswitch).
                if f then
                    i = f.endswitch - 1
                else
                    i = skip_to(tokens, i, {
                        ["endswitch"] = true,
                        opens = {["switch"] = true}
                    }, {["endswitch"] = true}) - 1
                end
            end

        elseif cmd == "case" or cmd == "default" then
            if switch_stack[#switch_stack] then
                -- A case was already taken: this case body must NOT run
                -- (no fall-through). Skip to OUR endswitch (depth-aware --
                -- a nested switch inside the skipped case body has its
                -- own endswitch; stopping at the nearest one would pop
                -- the wrong entry and resume the skipped body), leaving
                -- i ON the endswitch so the loop processes it and POPS.
                local f = compiled_flow[i]
                if f then
                    i = f.skip_to_end - 1
                else
                    i = skip_to(tokens, i, {
                        ["endswitch"] = true,
                        opens = {["switch"] = true}
                    }, {["endswitch"] = true}) - 1
                end
            end
            -- (not taken: this case belongs to a switch whose case body
            -- is currently executing -- fall through and run its body)

        elseif cmd == "endswitch" then
            switch_stack[#switch_stack] = nil  -- pop the switch

        -- Flow control: [iscript] — inline Lua code block
        elseif cmd == "iscript" then
            local code = params.body or ""
            if #code > 0 then
                local sandbox = {
                    ctx       = ctx,
                    f         = ctx.f or {},
                    sf        = ctx.sf or {},
                    tf        = ctx.tf or {},
                    kag       = require("kag"),
                    math      = math,
                    string    = string,
                    table     = table,
                    os        = { clock = os.clock, date = os.date, time = os.time },
                    tostring  = tostring,
                    tonumber  = tonumber,
                    type      = type,
                    pairs     = pairs,
                    ipairs    = ipairs,
                    next      = next,
                    print     = print,
                    pcall     = pcall,
                    select    = select,
                    unpack    = unpack or table.unpack,
                    error     = error,
                    coroutine  = coroutine,
                }
                local fn, compileErr = load(code, "=iscript", "t", sandbox)
                if fn then
                    local ok, runtimeErr = pcall(fn)
                    if not ok then
                        print("[iscript] Runtime error: " .. tostring(runtimeErr))
                    end
                else
                    print("[iscript] Compile error: " .. tostring(compileErr))
                end
            end

        -- Flow control: [eval] — unified scope (ctx + f + sf + tf)
        elseif cmd == "eval" then
            local code = params.exp or params.code or ""
            -- Round 61/68: TJS operator parity with [if]/[switch] —
            -- translate && || ! != before load; translateAssignment splits
            -- at the first top-level '=' and runs the FULL pipeline on the
            -- RHS, so ternary assignments (x = cond ? a : b) work too.
            if #code > 0 and code:find("[&|!?]") then
                code = exprLang.translateAssignment(code)
            end
            if #code == 0 then
                -- KAG3 requires [eval exp="..."]; a bare [eval lf.x = 1]
                -- parses as positional pairs and silently did nothing.
                -- Surface it so scripts stay honest.
                print(string.format(
                    "[WARN] [eval] without exp= at %s:%s (KAG3 form: "
                    .. "[eval exp=\"...\"])",
                    ctx.current_scene or ctx.currentScene or "?",
                    tostring(ctx.token_index or ctx.tokenIndex or "?")))
            end
            -- Whitelist env (audit): the old `__index = _G` exposed the
            -- ENTIRE global table -- os.execute and friends reachable from
            -- a [eval] tag -- while the strict-sandbox path (SystemCommands
            -- via sandbox.execute) never ran because this branch intercepts
            -- first. Same whitelist as [iscript]: os restricted to
            -- clock/date/time, no require/debug/io/load.
            local env = {
                ctx       = ctx,
                f         = ctx.f or {},
                sf        = ctx.sf or {},
                tf        = ctx.tf or {},
                mp        = ctx.mp or {},
                lf        = ctx.lf or {},
                math      = math,
                string    = string,
                table     = table,
                os        = { clock = os.clock, date = os.date, time = os.time },
                tostring  = tostring,
                tonumber  = tonumber,
                type      = type,
                pairs     = pairs,
                ipairs    = ipairs,
                next      = next,
                print     = print,
                pcall     = pcall,
                select    = select,
                unpack    = unpack or table.unpack,
                error     = error,
            }
            local fn, compileErr = load(code, "=eval", "t", env)
            if not fn then
                -- Round 71 (doc contract): a BARE VALUE expression
                -- ([eval] f.hp * 2, [eval] f.a ? 1 : 2) is not a valid Lua
                -- statement; wrap it in "return" through the FULL pipeline
                -- (ternary included) so the doc contract holds:
                -- "a bare expression stores its result in tf.eval_result".
                -- Assignments/calls compile as statements and never reach
                -- this path; when the wrap also fails, the original
                -- compile error is reported.
                local wrapped, werr = load(
                    "return " .. exprLang.translate(code),
                    "=eval", "t", env)
                if wrapped then
                    fn = wrapped
                else
                    compileErr = compileErr or werr
                end
            end
            if fn then
                local ok, result = pcall(fn)
                if ok and result ~= nil then
                    ctx.tf = ctx.tf or {}
                    rawset(ctx.tf, "eval_result", result)  -- no-trap invariant
                end
            else
                print(string.format("[eval] Compile error @ %s:%d: %s",
                    ctx.current_scene or "?", ctx.token_index or 0,
                    tostring(compileErr)))
            end

        -- Flow control: [macro] / [endmacro]
        elseif cmd == "macro" then
            -- bare [macro m args=...] -> params[1] (KAG3 syntax: the
            -- macro name is a positional arg -- audit fix); string-only
            -- guard (named-only params leave a pair table at params[1])
            local name = params.name
            if type(name) ~= "string" and type(params[1]) == "string" then
                name = params[1]
            end
            -- Collect macro body until [endmacro]
            local body = {}
            i = i + 1
            -- Depth-aware body collection (round 75): a nested
            -- [macro inner]...[endmacro] pair belongs to the OUTER body;
            -- the body ends at the [endmacro] matching the opening
            -- [macro] (depth back to 0). The naive scan stopped at the
            -- first [endmacro], corrupting the stream (round-74 report).
            local bdepth = 1
            while i <= #tokens do
                local bt = tokens[i]
                if bt[1] == "macro" then
                    bdepth = bdepth + 1
                elseif bt[1] == "endmacro" then
                    bdepth = bdepth - 1
                    if bdepth == 0 then break end
                end
                table.insert(body, {bt[1], bt[2]})
                i = i + 1
            end
            if name then
                clear_macro_history(ctx)
                ctx.macros = ctx.macros or {}
                ctx.macros[name] = body
                -- Neo-Genesis: parameterized macros (KAG3 args feature).
                -- args="who,what" declares the params; invocations fill
                -- %who% / %what% placeholders in the body.
                ctx.macro_args = ctx.macro_args or {}
                local args = {}
                if type(params.args) == "string" then
                    for raw_a in params.args:gmatch("[^,]+") do
                        local a = raw_a:match("^%s*(.-)%s*$")  -- trim
                        if #a > 0 then args[#args + 1] = a end
                    end
                end
                ctx.macro_args[name] = args
            end

        elseif cmd == "erasemacro" then
            -- bare [erasemacro m] -> params[1] (string-only guard)
            local name = params.name
            if type(name) ~= "string" and type(params[1]) == "string" then
                name = params[1]
            end
            if name and ctx.macros then
                if ctx.macros[name] ~= nil or (ctx.macro_args and ctx.macro_args[name] ~= nil) then
                    clear_macro_history(ctx)
                end
                ctx.macros[name] = nil
                if ctx.macro_args then ctx.macro_args[name] = nil end
            end
        elseif cmd == "endmacro" then
            -- pass (handled by macro recording above)

        -- Regular command: dispatch to kag table
        else
            -- Check if it's a macro invocation
            local macro_body = ctx.macros and ctx.macros[cmd]
            if macro_body then
                clear_macro_history(ctx)
                -- Expansion depth guard (round 75): the old per-ctx
                -- counter grew without bound, so a legit scene with >1000
                -- macro calls (e.g. a [for] loop calling a macro once per
                -- iteration) errored with a misleading recursion message.
                -- Track NESTING depth instead: each splice pushes the
                -- index of its last body token; the loop pops it once
                -- execution moves past that index (bottom of the loop).
                -- A self-recursive macro grows the stack without bound;
                -- a completed body always pops (loop rewinds re-splice
                -- with an empty stack, so iterations stay flat).
                ctx._macroStack = ctx._macroStack or {}
                -- endIdx = index of the LAST spliced body token; the loop
                -- pops the entry once i moves past it (bottom of loop).
                -- A completed body therefore never accumulates entries --
                -- a rewind re-splices with an empty (or fully popped)
                -- stack. Self-recursion re-splices at the SAME position
                -- while the previous entry is still live (i has not yet
                -- passed endIdx) -- the stack grows one per level and
                -- trips the depth cap. No dedupe: deduping by endIdx
                -- would swallow exactly the recursion signal.
                -- A nested splice also shifts the unexecuted outer tail.
                -- Keep its boundary live until that tail has actually run.
                local expansion_grow = #macro_body - 1
                for depth, end_index in ipairs(ctx._macroStack) do
                    if end_index >= i then
                        ctx._macroStack[depth] = end_index + expansion_grow
                    end
                end
                ctx._macroStack[#ctx._macroStack + 1] = i - 1 + #macro_body
                if #ctx._macroStack > 100 then
                    error("[macro] expansion depth exceeded (possible "
                          .. "self-recursive macro at " .. cmd .. ")")
                end
                -- Parameter substitution: build a per-invocation copy with
                -- %arg% placeholders filled from the call's params. The
                -- shared body is never mutated (multiple calls reuse it).
                local argNames = ctx.macro_args and ctx.macro_args[cmd]
                -- Deep-copy helper (splice scope: no-arg macros use it).
                local function deepCopy(t)
                    if type(t) ~= "table" then return t end
                    local out = {}
                    for k, v in pairs(t) do out[k] = deepCopy(v) end
                    return out
                end
                if argNames and #argNames > 0 then
                    -- Substitute %arg% placeholders inside the params
                    -- table (token t[2] is ALWAYS the params table -- the
                    -- string content lives in params values).
                    local function fill(v)
                        if type(v) == "string" then
                            return (v:gsub("%%([%w_]+)%%", function(an)
                                -- numeric placeholders (%1%) map to the
                                -- bare positional key params[1] (audit:
                                -- scheduler normalizes "1" to a NUMBER
                                -- key, so params["1"] was always nil)
                                local key = tonumber(an) or an
                                -- string-only: a mismatched arg usage
                                -- leaves the raw pair table at params[1]
                                -- and gsub would raise on a table value
                                -- (review nit -- keep the literal instead)
                                local val = params[key]
                                if type(val) == "string" then
                                    return val
                                end
                                return ("%" .. an .. "%")
                            end))
                        elseif type(v) == "table" then
                            -- Deep COPY: the body params table is shared by
                            -- every invocation -- in-place substitution
                            -- would poison later calls (review caught this).
                            local out = {}
                            for k, vv in pairs(v) do out[k] = fill(vv) end
                            return out
                        end
                        return v
                    end
                    local sub = {}
                    for n = 1, #macro_body do
                        local t = macro_body[n]
                        sub[n] = { t[1], fill(t[2]) }
                    end
                    macro_body = sub
                end
                -- Splice macro body into token stream, replacing the invocation.
                -- Copy the body (shallow, per-token) and shift the tail in place
                -- to avoid rebuilding the whole array with 3 inserts per call.
                -- NOTE: do NOT grow via table.insert(tokens, nil) -- nil
                -- elements do not extend the array length (#tokens stays
                -- put), so table.move's source range (#tokens - grow) was
                -- wrong and the tail tokens (labels/end after the call
                -- site) were OVERWRITTEN and lost. Shift the tail element
                -- by element from the END backwards (no overwrite), then
                -- clear the vacated slots (review: macro splice tail loss).
                tokens._runtime_rewritten = true
                local tailStart = i + 1
                local old_len = #tokens
                local bodyCount = #macro_body
                -- Grow the array by bodyCount - 1 (the invocation is replaced
                -- by bodyCount tokens).
                local grow = bodyCount - 1
                if grow > 0 then
                    for n = old_len, tailStart, -1 do
                        tokens[n + grow] = tokens[n]
                    end
                    for n = tailStart, tailStart + grow - 1 do
                        tokens[n] = nil
                    end
                elseif grow < 0 then
                    for n = tailStart, old_len do
                        tokens[n + grow] = tokens[n]
                    end
                    for n = old_len + grow + 1, old_len do
                        tokens[n] = nil
                    end
                end
                for n = 1, bodyCount do
                    -- Deep-copy every token: no-arg macros share the body's
                    -- params tables by reference today (security info item)
                    -- and a handler mutating them would poison later calls.
                    tokens[i - 1 + n] = {macro_body[n][1], deepCopy(macro_body[n][2])}
                end
                ctx.tokens = tokens
                ctx.label_index = nil  -- splice changed the stream: next jumps re-scan
                -- The compiled side table is now stale AND the local
                -- `compiled` variable still points at it. Invalidate FIRST
                -- (refresh_compiled's compiler.compile is a no-op while
                -- _compiled exists -- review: splice misdispatch used the
                -- stale handler binding), then recompile in place so the
                -- local table follows the spliced stream.
                compiler.invalidate(tokens)
                refresh_compiled()
                i = i - 1  -- will point to first body token after i = i + 1
            else
                -- text chunks become [ch] commands
                -- Neo-Genesis rules: coerce typed params BEFORE dispatch so
                -- handlers get numbers/booleans and bad input is reported
                -- with location instead of silently swallowed.
                if schemaModule.isMigrated(cmd) then
                    params = schemaModule.coerce(cmd, params, ctx)
                end
                -- Compiled streams bind the handler at compile time; the
                -- fallback lookup keeps hand-built streams working.
                local handler = compiled_handlers[i] or kag[cmd]
                local actual_cmd = cmd
                if not handler and type(cmd) == "string" and #cmd > 0 then
                    -- Unrecognized tag: KAG3 semantics render it as text, but
                    -- a typo'd command name (e.g. [elsif] before aliasing,
                    -- [wait] vs [wait]) would otherwise be invisible to the
                    -- author -- warn ONCE per scene+command so scripts stay
                    -- honest without flooding logs in [while] loops.
                    ctx._warned_cmds = ctx._warned_cmds or {}
                    local wkey = (ctx.current_scene or ctx.currentScene or "?")
                        .. "|" .. cmd
                    if not ctx._warned_cmds[wkey] then
                        ctx._warned_cmds[wkey] = true
                        print(string.format(
                            "[WARN] unknown KAG command '%s' treated as text "
                            .. "(typo?) at %s:%s",
                            cmd,
                            ctx.current_scene or ctx.currentScene or "?",
                            tostring(ctx.token_index or ctx.tokenIndex or "?")))
                    end
                    -- Unrecognized text ?? treat as [ch] -- through the ch
                    -- contract so interpolation ($f.name) and type coercion
                    -- apply to plain dialogue lines too (the main use case).
                    handler = kag["ch"]
                    if handler then
                        params = {text = cmd}
                        actual_cmd = "ch"
                        if schemaModule.isMigrated("ch") then
                            params = schemaModule.coerce("ch", params, ctx)
                        end
                    end
                end
                if handler then
                    local status, err = pcall(invoke_capable, handler, ctx, params, actual_cmd)
                    if not status then
                        local error_message = scheduler.error_text(err)
                        -- Lua-side error reporting (with scene:line)
                        print(string.format(
                            "[ERROR] KAG command '%s' failed at %s:%s: %s",
                            actual_cmd,
                            ctx.current_scene or ctx.currentScene or "?",
                            tostring(i),
                            error_message))
                        -- t212 G2: the ErrorUI chain needs scene + line
                        -- (previously only the print carried them); stash the
                        -- same fields on ctx so Engine::handleFatalError can
                        -- read them for the DiagnosticInfo.
                        ctx.error_command = actual_cmd
                        ctx.error_token_line = i
                        if ctx.handle_error then
                            local handled, handler_error = pcall(ctx.handle_error, actual_cmd, error_message,
                                  ctx.current_scene or ctx.currentScene or "",
                                  i)
                            if not handled then
                                ctx.stop_flag, ctx._command_error = true, true
                                ctx.error_handler_error = scheduler.error_text(handler_error)
                            end
                        else
                            ctx.stop_flag, ctx._command_error = true, true
                        end
                        if ctx._command_error then return end
                    end
                end
            end
        end

        ctx.token_index = i
        ctx._executing_index, ctx._executing_command = nil, nil
        i = i + 1
        -- Macro expansion depth: pop splices whose body execution has
        -- moved past the last body token (nested splices pop inner-first
        -- because their end index is smaller).
        while ctx._macroStack and #ctx._macroStack > 0
            and i > ctx._macroStack[#ctx._macroStack] do
            ctx._macroStack[#ctx._macroStack] = nil
        end
        -- Implicit return: a [call] whose subroutine falls off the END of
        -- the token stream (no [return] tag before the last token) must not
        -- leave a stale call frame -- pop it and resume at the saved return
        -- index (KAG3 semantics; also keeps ctx.lf consistent across jumps).
        while i > #tokens and ctx.call_stack and #ctx.call_stack > 0 do
            local frame = table.remove(ctx.call_stack)
            ctx.label_index = frame.label_index
            ctx.current_scene = frame.scene
            if frame.lf ~= nil then ctx.lf = frame.lf end
            if frame.mp ~= nil then ctx.mp = frame.mp end
            if frame.control then use_control_frame(frame.control) end
            tokens = frame.tokens
            ctx.tokens = tokens
            refresh_compiled()
            i = (frame.index or 1)
            -- t43 (implicit-return mirror of the explicit [return]): caller
            -- scene restored, the cross-scene switch signal is moot.
            ctx._scene_changed = false
            -- t49: mirror of the explicit-return dangling-label guard.
            prune_dangling_pending_jump()
        end
        ctx._resume_index = i
        coroutine.yield()
    end
end

-- ???? Resume from saved state ??????????????????????????????????????????????????????????????????????????????????????????????????

-- [R7-FIX] Read Skip: if skip_mode is "seen", only advance past already-seen tokens
-- Usage: [skip mode=seen] to skip only previously-read text
-- NOTE: Full implementation requires hooking into the token advance loop.
-- Current implementation: context variable is set so Lua scripts can check it.

function scheduler.resume(ctx)
    if not ctx.tokens or not ctx.token_index then return end
    scheduler.run(ctx, ctx.tokens, ctx.token_index)
end

scheduler.build_label_index = build_label_index
scheduler.is_safe_scene_path = is_safe_scene_path
scheduler.find_label = find_label
return scheduler
