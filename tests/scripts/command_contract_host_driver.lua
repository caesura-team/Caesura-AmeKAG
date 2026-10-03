-- Actual engine entry driver. It never substitutes a command handler, schema,
-- renderer, audio implementation, or native binding. The parent owns materialized
-- scene bytes, private user-data/config, binary identity and process deadline.
local M = {}

function M.start(request)
    assert(type(request) == "table" and type(request.case) == "table")
    local case = request.case
    -- Native probe may seed this before sandbox lockdown. Never create an
    -- extra global or use its existence as evidence that execution completed.
    local publication = rawget(_G, "_CAESURA_COMMAND_CONTRACT_RESULT")
    if type(publication) ~= "table" then publication = nil end
    if publication then publication.status = "RUNNING"; publication.complete = false end
    assert(case.status == "READY_FOR_NATIVE_EXECUTION", "case has unmet authoring/device prerequisites")
    assert(type(case.scene_path) == "string" and case.scene_path:match("^assets/script/contracts/[a-z0-9_-]+%.ks$"))
    assert(type(request.manifest_sha256) == "string" and #request.manifest_sha256 == 64)
    local getinfo = assert(debug and debug.getinfo, "native binding identity inspection unavailable")
    for _, item in ipairs({{DevCore, "quit"}, {DevCore, "set_text_input_rect"},
        {Render, "load_texture"}, {Render, "submit_transition"}, {KAG, "play_bgm"}}) do
        local fn = item[1] and item[1][item[2]]
        assert(type(fn) == "function" and getinfo(fn, "S").what == "C", "real native binding required: " .. item[2])
    end
    require("kag.init")
    local runner = require("kag_runner")
    local layers = require("layers")
    local debugger = require("kag_debug")
    local json = require("capability_json")
    if type(case.expected_refusal) == "table" then
        local refusal = case.expected_refusal
        local runtime = require("capability_runtime")
        local actual = runtime.query(refusal.feature)
        assert(actual.status == "unsupported" and actual.reason == refusal.reason,
            "actual capability does not match this declared refusal case")
        assert(runtime.configure_project_json(assert(json.encode({capabilities={
            required=json.array({}),optional=json.array({refusal.feature}),accept_approximate=json.array({})}}))))
    end
    if case.require_ai_unavailable then
        assert(not require("backend").ai_available(), "AI service must be disabled before this offline fallback case")
    end
    local backend = require("backend")
    local sma_module, sma_seen, sma_rendered = nil, false, 0
    local sma_native, sma_texture, sma_proof = nil, nil, {}
    local owner_before, owner_after, rollback_before, rollback_after = nil, nil, nil, nil
    local choice_proof, overlay_proof = {}, {}
    local function draw_texts(ctx)
        local texts = {}
        for _, draw in ipairs(ctx.text_state and ctx.text_state.draws or {}) do
            if type(draw.text) == "string" then texts[#texts+1] = draw.text end
        end
        return table.concat(texts, "|")
    end
    local function owner(ctx)
        local active = {}
        for i, token in ipairs(ctx.active_operations or {}) do active[i] = token end
        return {focus=ctx.input_focus, native_focus=backend.get_input_focus(),
            click=rawget(_G,"_KAG_onClick"), key=rawget(_G,"_KAG_onKeyDown"),
            text=rawget(_G,"_KAG_onTextInput"), operations=active}
    end
    local function same_owner(before, after)
        if before.focus ~= after.focus or before.native_focus ~= after.native_focus
            or before.click ~= after.click or before.key ~= after.key or before.text ~= after.text
            or #before.operations ~= #after.operations then return false end
        for i, token in ipairs(before.operations) do if token ~= after.operations[i] then return false end end
        return true
    end
    local function compare_owner_fields(before, after)
        local checks={focus=before.focus==after.focus,native_focus=before.native_focus==after.native_focus,
            click=before.click==after.click,key=before.key==after.key,text=before.text==after.text,
            operation_count=#before.operations==#after.operations,operation_identity=true}
        local slots={}
        for i=1,math.max(#before.operations,#after.operations) do
            local equal=before.operations[i]==after.operations[i]
            slots[i]={index=i,equal=equal}
            if not equal then checks.operation_identity=false end
        end
        return {checks=checks,before_focus_nil=before.focus==nil,after_focus_nil=after.focus==nil,
            before_focus_type=type(before.focus),after_focus_type=type(after.focus),
            before_click_type=type(before.click),after_click_type=type(after.click),
            before_key_type=type(before.key),after_key_type=type(after.key),
            before_text_type=type(before.text),after_text_type=type(after.text),
            operations_before=#before.operations,operations_after=#after.operations,slots=json.array(slots)}
    end
    local old_check = debugger.check
    local trace, seen, frames, finished = {}, {}, 0, false
    local focus_frames, actions, choice_frame = {}, {}, nil
    local frame_limit = assert(tonumber(case.max_frames))
    assert(frame_limit >= 1 and frame_limit <= 1800)
    debugger.check = function(ctx, cmd, index)
        if #trace >= 8192 then error("command trace budget exhausted") end
        trace[#trace + 1] = {command=cmd,index=index,scene=ctx.current_scene,frame=frames}
        seen[cmd] = (seen[cmd] or 0) + 1
        if case.establish_explicit_focus and cmd==case.observe_overlay_command and not owner_before then
            overlay_proof.fixture_baseline={prior_lua_focus=ctx.input_focus,prior_lua_nil=ctx.input_focus==nil,
                prior_native_focus=backend.get_input_focus(),requested_lua="kag",requested_native="KAG"}
            assert(ctx.input_focus==nil or ctx.input_focus=="kag", "unexpected pre-fixture Lua focus")
            assert(overlay_proof.fixture_baseline.prior_native_focus=="KAG", "unexpected pre-fixture native focus")
            ctx.input_focus="kag"
            backend.set_input_focus("KAG")
            assert(backend.get_input_focus()=="KAG", "explicit native baseline not applied")
        end
        if not owner_before and ((case.choice_index and (cmd == "endbutton" or cmd == "endselect"))
            or cmd == case.observe_overlay_command) then owner_before = owner(ctx) end
        -- Observe release when the next token is entered, BEFORE [end] and
        -- runner.finish_session can perform blanket operation cleanup.
        if owner_before and not owner_after and (choice_proof.action_count or overlay_proof.action_count)
            and cmd ~= case.observe_overlay_command and cmd ~= "endbutton" and cmd ~= "endselect" then
            owner_after=owner(ctx)
            local restored=same_owner(owner_before,owner_after)
            local comparison=compare_owner_fields(owner_before,owner_after)
            if case.choice_index then choice_proof.owner_comparison=comparison else overlay_proof.owner_comparison=comparison end
            if case.choice_index then
                choice_proof.owner_restored_before_stop=restored
                choice_proof.released_before_terminal=not ctx._choiceMode and ctx._choiceButtonsActive==nil and ctx._selectedChoice==nil
                choice_proof.release_observed_command=cmd
            else
                overlay_proof.owner_restored_before_stop=restored
                overlay_proof.before_focus=owner_before.focus; overlay_proof.after_focus=owner_after.focus
                overlay_proof.before_native_focus=owner_before.native_focus; overlay_proof.after_native_focus=owner_after.native_focus
                overlay_proof.release_observed_command=cmd
                overlay_proof.layers_hidden=true
                for _, name in ipairs(case.require_hidden_layers or {}) do
                    local layer=layers.get_layer(name)
                    if layer and layer.visible then overlay_proof.layers_hidden=false end
                end
                overlay_proof.gallery_closed=not (ctx.galleryState and ctx.galleryState.active)
            end
        end
        if sma_module and seen.sma_stop and cmd ~= "sma_stop" and not sma_proof.stop_observed_command then
            sma_proof.stop_observed_command=cmd
            sma_proof.count_after_stop=sma_native.count()
            sma_proof.actor_absent_after_stop=not(ctx.sma_actors and ctx.sma_actors.hero)
        end
        if cmd == "rollback" and case.pre_checkpoint then
            rollback_before = {value=ctx.f.value, text=(ctx.backlog[#ctx.backlog] or {}).text,
                draw_text=draw_texts(ctx), snapshots=#(ctx._undoStack or {})}
        end
        return old_check(ctx, cmd, index)
    end
    local function finish(status, reason, ctx)
        if finished then return end
        finished = true
        debugger.check = old_check
        local stopped, result, stop_error = pcall(runner.stop)
        local cleanup = stopped and result ~= false
        if sma_texture then
            local released, release_error = pcall(function()
                backend.destroy_texture(sma_texture)
                assert(not Render.is_valid_handle(0, sma_texture), "owned texture still valid after destruction")
            end)
            sma_proof.texture_released = released
            if not released then cleanup=false; stop_error=release_error end
        end
        if not cleanup then status="FAIL"; reason="runner cleanup failed: " .. tostring(stop_error or result) end
        local receipt = {schema=1,case_id=case.id,status=status,reason=reason,
            manifest_sha256=request.manifest_sha256,frames=frames,frame_limit=frame_limit,
            scene=case.scene_path,trace=json.array(trace),observed_commands=seen,
            scripted_inputs=json.array(actions),observed_focus=focus_frames,
            sma_actor_observed=sma_seen,sma_rendered_frames=sma_rendered,sma=sma_proof,
            choice=choice_proof,overlay=overlay_proof,
            rollback_before=rollback_before,rollback_after=rollback_after,
            terminal_kind=case.terminal_kind or "natural scene end",
            expected_outcome=case.expected_refusal and "declared optional capability refusal, not application" or "applied scene contract",
            command_error=ctx and ctx._command_error == true or false,
            error_command=ctx and ctx.error_command or nil,
            label_map=ctx and ctx.labelMap or {},
            capability_diagnostics=json.array(ctx and ctx.capability_diagnostics or {}),
            sentinel=ctx and ctx.tf and ctx.tf.contract_case or nil,
            runner_cleanup=cleanup,physical_input=false,physical_audibility=false,
            process_cleanup_proven_by_parent_only=true}
        print("COMMAND_CONTRACT_NATIVE_JSON:" .. assert(json.encode(receipt)))
        if publication then
            for key, value in pairs(receipt) do publication[key] = value end
            publication.complete = true
        end
        DevCore.quit()
    end
    local function check_result(ctx)
        if not ctx or ctx._command_error or ctx.error_handler_error then
            return false, "command error: " .. tostring(ctx and ctx.error_command)
        end
        if not ctx.tf or ctx.tf.contract_case ~= case.id then return false, "success sentinel missing" end
        if case.expected_final_scene and ctx.current_scene ~= case.expected_final_scene then return false, "final scene differs" end
        if case.expected_dialogue then
            local found = false
            for _, entry in ipairs(ctx.backlog or {}) do if entry.text == case.expected_dialogue then found = true end end
            if not found then return false, "required dialogue was not produced" end
        end
        for _, label in ipairs(case.require_labels or {}) do
            local index = ctx.labelMap and ctx.labelMap[label]
            if type(index) ~= "number" or index < 1 or index % 1 ~= 0
                or not ctx.tokens or index > #ctx.tokens + 1 then
                return false, "compiled label map is missing: " .. label
            end
        end
        if next(ctx._warned_cmds or {}) then return false, "unknown command was treated as dialogue" end
        if case.expected_refusal then
            local found = false
            for _, diagnostic in ipairs(ctx.capability_diagnostics or {}) do
                if diagnostic.status ~= "unsupported" or diagnostic.feature ~= case.expected_refusal.feature
                    or diagnostic.reason ~= case.expected_refusal.reason then return false, "unexpected capability result" end
                found = true
            end
            if not found or (ctx.capability_diagnostics_dropped or 0) ~= 0 then return false, "expected exact capability refusal missing" end
        elseif #(ctx.capability_diagnostics or {}) > 0 or (ctx.capability_diagnostics_dropped or 0) > 0 then
            return false, "positive scene has unsupported/failed/approximate capability results"
        end
        for key, expected in pairs(case.expect_f or {}) do
            if not ctx.f or ctx.f[key] ~= expected then return false, "f." .. key .. " differs" end
        end
        for _, command in ipairs(case.require_runtime_trace or {}) do
            if not seen[command] then return false, "command was never entered: " .. command end
        end
        for command, maximum in pairs(case.required_max_trace or {}) do
            if (seen[command] or 0) > maximum then return false, "command repeated unexpectedly: " .. command end
        end
        if case.require_switch_routes then
            local compiled = ctx.tokens and ctx.tokens._compiled
            if not compiled or not compiled.flow then return false, "compiled switch routes missing" end
            local ordinal = 0
            for _, entered in ipairs(trace) do
                if entered.command == "switch" then
                    ordinal = ordinal + 1
                    local expected = case.require_switch_routes[ordinal]
                    local route = compiled.flow[entered.index]
                    if not expected or not route or not route.cases then return false, "switch route structure differs" end
                    local destination = expected.default and route.default or route.cases[expected.value]
                    if type(destination) ~= "number" then return false, "switch destination absent" end
                    local matched = false
                    for _, later in ipairs(trace) do
                        if later.index == destination and later.frame > entered.frame then matched = true; break end
                    end
                    if not matched then return false, "switch chosen body was not executed" end
                end
            end
            if ordinal ~= #case.require_switch_routes then return false, "switch route count differs" end
        end
        if case.expect_replay_recorded then
            local replay = require("replay")
            if replay.event_count() < 1 or replay.get_mode() ~= "off" then return false, "replay record/off state differs" end
        end
        if sma_module then
            sma_proof.final_count = sma_native.count()
            if not sma_seen or sma_rendered < 1 or (ctx.sma_actors and ctx.sma_actors.hero)
                or not sma_proof.wave or not sma_proof.ik or not sma_proof.variant_replaced
                or sma_proof.final_count ~= sma_proof.baseline
                or not sma_proof.stop_observed_command or not sma_proof.actor_absent_after_stop
                or sma_proof.count_after_stop ~= sma_proof.baseline then
                return false, "SMA owned allocation/state/replacement/teardown contract incomplete"
            end
        end
        if case.choice_index or case.escape_focus then
            if not owner_before then return false, "interactive owner not captured before command" end
            if not owner_after then return false, "owner release not observed before terminal cleanup" end
            if case.choice_index then
                if not choice_proof.owner_restored_before_stop or not choice_proof.released_before_terminal
                    or not choice_proof.wait_frame or not choice_proof.render_frame
                    or choice_proof.action_count ~= 1 or choice_proof.target ~= case.expected_choice_target then
                    return false, "choice wait/render/nondefault input/owner restoration incomplete"
                end
            else
                if not overlay_proof.owner_restored_before_stop or overlay_proof.action_count ~= 1
                    or not overlay_proof.render_frame or not overlay_proof.layers_hidden or not overlay_proof.gallery_closed then
                    return false, "overlay rendered Escape/owner restoration incomplete before terminal cleanup"
                end
            end
        end
        if case.require_focus and not focus_frames[case.require_focus] then
            return false, "required overlay never opened: " .. case.require_focus
        end
        return true
    end
    local input_done = false
    function engine_update(dt)
        if finished then return end
        frames = frames + 1
        local ctx = runner.get_ctx()
        if frames > frame_limit then finish("FAIL", "frame deadline", ctx); return end
        if ctx and type(case.escape_focus) == "string" and ctx.input_focus == case.escape_focus
            and overlay_proof.render_frame and frames > overlay_proof.render_frame
            and not overlay_proof.action_count then
            -- Inject after the host input pump and before the real overlay's
            -- update. This is a scripted polled-key input, not physical OS input.
            _GAME_KEY_ESC = true
            overlay_proof.action_count=1
            actions[#actions + 1] = {kind="polled_escape",focus=case.escape_focus,frame=frames}
        end
        local ok, continued, reason = pcall(runner.update, dt)
        ctx = runner.get_ctx() or ctx
        if ctx and type(ctx.input_focus) == "string" then focus_frames[ctx.input_focus] = focus_frames[ctx.input_focus] or frames end
        if not ok then finish("FAIL", tostring(continued), ctx); return end
        if ctx and ctx._command_error then finish("FAIL", "handler failed", ctx); return end
        if sma_module and ctx and ctx.sma_actors then
            local actor = ctx.sma_actors.hero
            if actor then
                local complete
                if actor.parts then
                    complete = #actor.parts > 0
                    for _, part in ipairs(actor.parts) do
                        complete = complete and type(part.handle) == "number" and part.handle > 0
                    end
                else complete = type(actor.handle) == "number" and actor.handle > 0 end
                if complete then
                    if not sma_native.initialized() then
                        finish("FAIL", "real SMA backend not initialized after allocation", ctx); return
                    end
                    sma_proof.initialized_after_allocation=true
                    local count = sma_native.count()
                    if count ~= sma_proof.baseline + case.prepare_sma.expected_parts then
                        finish("FAIL", "native mesh count differs from owned parts", ctx); return
                    end
                    sma_seen = true; sma_proof.allocated_count=count
                    for _, part in ipairs(actor.parts or {}) do
                        if part.texId ~= sma_texture or not Render.is_valid_handle(0, part.texId) then
                            finish("FAIL", "SMA part lacks owned logical texture", ctx); return
                        end
                        if part.id == "eyes" then
                            if part.current == "default" then sma_proof.original_eyes=part.handle end
                            if part.current == "happy" then
                                sma_proof.replacement_eyes=part.handle
                                sma_proof.variant_replaced=sma_proof.original_eyes ~= nil
                                    and part.handle ~= sma_proof.original_eyes
                            end
                        end
                    end
                    if actor.anim == "wave" and actor.loop == false then sma_proof.wave=true end
                    local ik=actor.ik
                    if ik and ik.chain[1]==5 and ik.chain[2]==6 and ik.tx==0.95 and ik.ty==0.25 and ik.l2==0.3 then sma_proof.ik=true end
                end
            end
            local updated, update_error = pcall(sma_module.update, ctx, dt)
            if not updated then finish("FAIL", tostring(update_error), ctx); return end
        end
        if type(case.expected_checkpoint) == "table" and ctx then
            local checkpoint = case.expected_checkpoint
            if seen[checkpoint.seen_command] and ctx[checkpoint.waiting_flag] then
                local last = ctx.backlog and ctx.backlog[#ctx.backlog]
                rollback_after={value=ctx.f.value,text=last and last.text,draw_text=draw_texts(ctx)}
                local matches = last and last.text == checkpoint.text and rollback_after.draw_text:find(checkpoint.draw_text,1,true) ~= nil
                local before=case.pre_checkpoint
                matches=matches and rollback_before and rollback_before.snapshots > 0
                    and rollback_before.value==before.f.value and rollback_before.text==before.text
                    and rollback_before.draw_text==before.draw_text
                    and rollback_before.value~=rollback_after.value
                    and rollback_before.draw_text~=rollback_after.draw_text
                for key, value in pairs(checkpoint.f or {}) do matches = matches and ctx.f[key] == value end
                local accepted, why = check_result(ctx)
                finish(matches and accepted and "PASS" or "FAIL", why or (not matches and "restored checkpoint differs" or nil), ctx)
                return
            end
        end
        if not continued and reason == "ended" then
            local accepted, why = check_result(ctx)
            finish(accepted and "PASS" or "FAIL", why, ctx); return
        end
        if ctx and ctx._choiceMode and type(case.choice_index) == "number" then
            choice_frame = choice_frame or frames
            choice_proof.wait_frame=choice_proof.wait_frame or frames
            if not ctx.waiting_input or #(ctx._choiceButtonsActive or {}) ~= case.expected_choice_count then
                finish("FAIL", "actual choice wait/count differs", ctx); return
            end
            if choice_proof.render_frame and frames > choice_proof.render_frame and not choice_proof.action_count then
                local selected = ctx._choiceButtonsActive and ctx._choiceButtonsActive[case.choice_index]
                if not selected then finish("FAIL", "planned choice is unavailable", ctx); return end
                _GAME_MOUSE_X, _GAME_MOUSE_Y = 16, selected.y + selected.h / 2
                choice_proof.action_count=1; choice_proof.target=selected.target
                local selected_ok, selected_error = pcall(_KAG_onClick)
                actions[#actions + 1] = {kind="choice_callback",index=case.choice_index,frame=frames}
                if not selected_ok then finish("FAIL", tostring(selected_error), ctx) end
            end
        elseif ctx and ctx._inputMode and type(case.input_text) == "string" and not input_done then
            input_done = true
            local action_ok, action_error = pcall(function()
                assert(type(_KAG_onTextInput) == "function" and type(_KAG_onKeyDown) == "function")
                _KAG_onTextInput(case.input_text)
                _KAG_onKeyDown(13, "return")
                actions[#actions + 1] = {kind="text_callbacks",frame=frames}
            end)
            if not action_ok then finish("FAIL", tostring(action_error), ctx) end
        elseif ctx and ctx.waiting_input and not ctx._inputMode and not ctx._choiceMode
            and (not case.escape_focus or ctx.input_focus ~= case.escape_focus) and case.advance_dialogue then
            local reveal = ctx.reveal
            local state = require("kag.text_scene").get_state(ctx)
            -- Let actual update advance typewriter cues; an immediate click
            -- would bypass precisely the real audio/update chain under audit.
            if reveal and (state.reveal_chars or 0) < reveal.total then return end
            local clicked, click_result, click_error = pcall(runner.on_click)
            if not clicked or click_result == false then finish("FAIL", tostring(click_error or click_result), ctx) end
        end
    end
    function engine_render()
        if finished then return end
        local ok, reason = pcall(function()
            layers.render(); runner.render()
            local ctx = runner.get_ctx()
            -- A click callback can open an overlay after update's observation.
            -- Record the actual focus at the rendered frame as well.
            if ctx and type(ctx.input_focus)=="string" then
                focus_frames[ctx.input_focus]=focus_frames[ctx.input_focus] or frames
            end
            if ctx and ctx._choiceMode and ctx.waiting_input then choice_proof.render_frame=choice_proof.render_frame or frames end
            if ctx and ctx.input_focus==case.escape_focus then overlay_proof.render_frame=overlay_proof.render_frame or frames end
            if sma_module and ctx and ctx.sma_actors and next(ctx.sma_actors) then
                sma_module.render(ctx); sma_rendered = sma_rendered + 1
            end
        end)
        if not ok then finish("FAIL", "render: " .. tostring(reason), runner.get_ctx()) end
    end
    local prepared, preparation_error=pcall(function()
        if case.prepare_sma then
            sma_module=require("kag.sma"); sma_native=assert(rawget(_G,"sma"))
            for _, name in ipairs({"initialized","count"}) do
                assert(type(sma_native[name])=="function" and getinfo(sma_native[name],"S").what=="C", "real native SMA query required")
            end
            sma_proof.baseline=sma_native.count()
            local rgba=case.prepare_sma.rgba
            sma_texture=backend.create_solid_texture(table.unpack(rgba))
            assert(type(sma_texture)=="number" and Render.is_valid_handle(0,sma_texture), "native owned texture allocation failed")
            sma_proof.texture_logical_id=sma_texture; sma_proof.rgba=rgba
            local file=assert(io.open(case.prepare_sma.asset,"rb"))
            local content=file:read("*a"); file:close()
            local asset=assert(sma_module.load(content,{validate=true}))
            assert(#asset.parts==case.prepare_sma.expected_parts, "asset part count differs")
            sma_proof.part_texture_bindings=json.array({})
            for _, part in ipairs(asset.parts) do
                sma_proof.part_texture_bindings[#sma_proof.part_texture_bindings+1]={id=part.id,original=part.tex,owned=sma_texture}
                part.tex=sma_texture
            end
            sma_module.register(case.prepare_sma.registered_name,asset)
        end
    end)
    if not prepared then finish("FAIL", tostring(preparation_error),runner.get_ctx()); return end
    local ok, started, reason = pcall(runner.start, case.scene_path)
    if not ok or not started then finish("FAIL", tostring(reason or started), runner.get_ctx()) end
end

return M
