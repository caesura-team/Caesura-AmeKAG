// Web supplies trusted scenes and input/output; kag_runner owns every session.
export async function installRunnerBridge(lua) {
  await lua.doString(`
    local runner = require('kag_runner')
    runner.set_resume_adapter({
      is_paused = function() return _CAESURA_DEBUG_PAUSED == true end,
      resume = function(_, co, value) return __resume_web_scene(co, value) end,
    })

    local function provider(sources, bundle)
      sources = __copy_web_scenes(sources)
      local function find(name)
        return __find_web_scene(bundle, name), __find_web_scene(sources, name)
      end
      return function(name)
        local serialized, source = find(name)
        if serialized ~= nil then return require('kag.compiler').deserialize(serialized) end
        if type(source) == 'string' and #source > 0 then return require('tokenizer').parse(source) end
      end, function(name)
        local serialized, source = find(name)
        return serialized ~= nil or source ~= nil
      end
    end

    local function publishText(ctx)
      __SCENE_DRAWS_TABLE = {}
      if not ctx then return end
      local function text(text,x,y,r,g,b,a,scale,bold,italic,strike)
        if type(text)=='string' and #text>0 then
          __SCENE_DRAWS_TABLE[#__SCENE_DRAWS_TABLE+1] = {
            t=text,x=x or 0,y=y or 0,r=r or 255,g=g or 255,b=b or 255,a=a or 255,
            s=scale or 1,bd=bold and 1 or 0,it=italic and 1 or 0,st=strike and 1 or 0,
          }
        end
      end
      require('kag.text_scene').render(ctx,{
        render_text=text,
        render_ruby=function(base,ruby,x,y,r,g,b,a)
          text(base,x,y,r,g,b,a)
          if #__SCENE_DRAWS_TABLE>0 then __SCENE_DRAWS_TABLE[#__SCENE_DRAWS_TABLE].ruby=ruby end
        end,
      })
    end
    __PUBLISH_WEB_TEXT = function() publishText(runner.get_ctx()) end

    local function publish()
      local ctx = runner.get_ctx()
      __CTXREF, __LAST_CTX, __CO = ctx, ctx, ctx and ctx.co
      publishText(ctx)
      __SCENE_ENDINGS = {}
      if not ctx then return end
      for id, info in pairs(ctx.seen_endings or {}) do
        __SCENE_ENDINGS[#__SCENE_ENDINGS+1] = {
          id=tostring(id), name=type(info)=='table' and tostring(info.name or '') or '',
        }
      end
      if ctx._web_restore_history then
        __RESTORED_WEB_BACKLOG = require('kag.save_state').copy(ctx.backlog or {})
        __SCENE_BACKLOG = nil
      end
    end
    __PUBLISH_WEB_RUNNER = publish

    local function page(ctx)
      local draws = {}
      for _, d in ipairs(ctx.text_state and ctx.text_state.draws or {}) do
        if d.text and #d.text > 0 then
          draws[#draws+1] = {t=tostring(d.text), x=d.x or 0, y=d.y or 0}
        end
      end
      return draws
    end

    local function aim_choice(ctx, opts)
      if ctx._choiceMode then
        local buttons = ctx._choiceButtonsActive or ctx._choiceButtons or {}
        local button = buttons[tonumber(opts.choiceIndex) or 1]
        _GAME_MOUSE_X = tonumber(opts.choiceX) or 100
        _GAME_MOUSE_Y = tonumber(opts.choiceY)
          or (button and ((tonumber(button.y) or 450)+(tonumber(button.h) or 24)/2)) or 460
        if type(_KAG_onClick)=='function' then _KAG_onClick() end
      end
    end

    local function remember_page(draws)
      __SCENE_PAGE = draws
      __SCENE_BACKLOG = __SCENE_BACKLOG or {}
      __SCENE_BACKLOG[#__SCENE_BACKLOG+1] = draws
    end

    local function click(ctx, opts)
      aim_choice(ctx, opts)
      local pending_page = ctx.waiting_input and page(ctx)
      local ok, reason = runner.on_click()
      if ok and reason ~= 'revealed' and pending_page then
        remember_page(pending_page)
      end
      return ok, reason
    end

    local function pump(opts, advance)
      local frames, clicks = 0, 0
      local limit = tonumber(opts.maxFrames) or 200000
      local clicked = advance or __CLICK == true
      __CLICK = false
      while frames < limit do
        local ctx = runner.get_ctx()
        if not ctx then return 'ERR:no-context' end
        if not ctx.co and not ctx._pendingRestore and not ctx._scene_changed and not ctx._pendingJump then
          return 'DONE:'..tostring(ctx.token_index)..':'..clicks
        end
        frames = frames+1
        if __PERF_TRACE == true then __FRAME_COUNT=frames end
        local ok, reason
        -- A manual advance releases a restored checkpoint even when the host
        -- retains skip mode. Automatic input still stops at the checkpoint.
        local resume_rollback = ctx._rollback_waiting and clicked
        if ctx.waiting_input and not ctx._pendingRestore and (not ctx.skip_mode or resume_rollback) then
          if clicked or (opts.autoClick and clicks < 10000) then
            ok, reason = click(ctx, opts)
            clicked = false
            clicks = clicks+1
          else
            -- This synchronous host returns a settled page. The runner still
            -- advances reveal time through its normal 16-ms update path.
            local st = ctx.text_state
            if ctx.reveal and st and (st.reveal_chars or 0) < ctx.reveal.total then
              ok, reason = runner.update(0.016)
            else
              return 'WAIT:'..tostring(ctx.token_index)
            end
          end
        else
          local skipped_page = ctx.waiting_input and ctx.skip_mode and page(ctx)
          if skipped_page and ctx.skip_mode==true then aim_choice(ctx,opts) end
          ok, reason = runner.update(0.016)
          if ok and skipped_page then remember_page(skipped_page) end
        end
        local current = runner.get_ctx()
        if current ~= ctx and current then current._web_restore_history=true end
        if current and current._rollback_waiting then
          -- Rollback retains its context identity, but replaces that context's
          -- historical page and backlog. Use the existing restore publisher
          -- and hand this click point back to the host before any auto-click.
          current._web_restore_history=true
          return 'WAIT:'..tostring(current.token_index)
        end
        if not ok and reason=='waiting-input' and ctx.skip_mode=='seen' then
          return 'WAIT:'..tostring(ctx.token_index)
        end
        if not ok and reason ~= 'waiting-input' and reason ~= 'ended' and reason ~= 'dead' then
          return 'ERR:'..tostring(reason)
        end
      end
      local ctx = runner.get_ctx()
      return 'ERR:frame-limit@'..tostring(ctx and ctx.token_index or 0)
    end

    function __DRIVE_WEB_RUNNER(mode, name, opts, sources, bundle)
      local owner = runner.get_ctx()
      if owner and owner ~= __CTXREF and owner.tf and owner.tf.load_result=='ok' then
        owner._web_restore_history=true
      end
      -- A host may call the public runner rollback between drives. Capture
      -- its history replacement before an explicit advance clears the wait.
      if owner and owner._rollback_waiting then owner._web_restore_history=true end
      local advance = opts.advance == true and opts.advanceScene == name
        and owner ~= nil
      if mode == 'load' or not advance then
        local old_loader, old_has = __PREPARE_SCENE_TOKENS, __HAS_RESTORE_SCENE
        if sources ~= nil or bundle ~= nil then
          __PREPARE_SCENE_TOKENS, __HAS_RESTORE_SCENE = provider(sources, bundle)
        end
        local ok, reason
        if mode == 'load' then
          local prepared
          prepared, reason = require('kag.commands.save').prepare_load({slot=opts.slot})
          if prepared then
            ok, reason = runner.restore_candidate(owner, prepared)
            if not ok then require('kag.presentation').discard(prepared._presentation) end
          end
        else
          ok, reason = runner.start(name, {replace=true})
        end
        if not ok then
          __PREPARE_SCENE_TOKENS, __HAS_RESTORE_SCENE = old_loader, old_has
          if owner and runner.get_ctx()==owner then
            owner.tf=owner.tf or {}
            owner.tf.load_result, owner.tf.load_error='error', tostring(reason)
          end
          publish()
          return 'ERR:'..tostring(reason)
        end
        if mode~='load' then __WEB_NEW_SESSION=true end
        if mode=='load' then runner.get_ctx()._web_restore_history=true end
      end
      local ctx = runner.get_ctx()
      local cps = tonumber(opts.textSpeed)
      if cps and cps > 0 then ctx.cps=cps; ctx.text_speed=math.max(1,math.floor(1000/cps)) end
      if mode~='load' or opts.skip~=nil then ctx.skip_mode = opts.skip == true end
      local ok, result = pcall(pump, opts, advance)
      publish()
      return ok and result or 'ERR:'..tostring(result)
    end
  `)
}
