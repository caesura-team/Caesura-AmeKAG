-- Bounded nonrecursive asset directory listing. Runtime code uses the current
-- asset reader; it never acquires shell access or probes directories by writing.
local FileUtil = {}
local MAX_ENTRIES, MAX_NAME_BYTES = 4096, 1024 * 1024
local function safe_directory(dir)
    if type(dir)~="string" or #dir==0 or #dir>4096 or dir:sub(1,1)=="/"
        or dir:find("..",1,true) or dir:find("\\",1,true) or dir:find(":",1,true)
        or dir:find("//",1,true) or dir:find("%c") then return false end
    for part in dir:gmatch("[^/]+") do if part=="." then return false end end
    return true
end
local function validate_names(names)
    if type(names)~="table" then error("Asset directory listing returned no complete list",0) end
    local bytes,seen=0,{}
    for i,name in ipairs(names) do
        if i>MAX_ENTRIES or type(name)~="string" or name=="" or name:find("/",1,true)
            or not safe_directory(name) or (utf8 and not utf8.len(name)) then
            error("Invalid asset directory leaf name or entry limit",0)
        end
        bytes=bytes+#name
        if bytes>MAX_NAME_BYTES or seen[name] then error("Asset directory byte limit or duplicate name",0) end
        seen[name]=true
    end
    return names
end
local function standalone_lfs(dir)
    -- Development CLI compatibility only. Strict runtime without the native
    -- asset capability reports unsupported instead of falling back to host FS.
    if _SANDBOX_MODE=="strict" then error("Asset directory listing unsupported: no safe runtime binding",0) end
    -- The maintained standalone interpreter explicitly preloads this module.
    -- Do not search disk/HTTP for it, or grant it to an engine Lua state.
    local cli_loaded=package.loaded and package.loaded["caesura_cli_fs"]
    local cli_preload=package.preload and package.preload["caesura_cli_fs"]
    if cli_loaded~=nil or type(cli_preload)=="function" then
        local cli=require("caesura_cli_fs")
        if type(cli)~="table" or type(cli.list_dir)~="function" then error("Invalid CLI asset listing capability",0) end
        local names,reason=cli.list_dir(dir,MAX_ENTRIES,MAX_NAME_BYTES)
        if names==nil or reason~=nil then error("CLI asset directory listing failed: "..tostring(reason),0) end
        return names
    end
    local loaded,lfs=pcall(require,"lfs")
    if not loaded or type(lfs)~="table" or type(lfs.dir)~="function"
        or type(lfs.symlinkattributes)~="function" then
        error("Asset directory listing unsupported: no safe runtime binding or CLI LuaFileSystem",0)
    end
    local opened,iter,dir_obj=pcall(lfs.dir,dir)
    if not opened or type(iter)~="function" or dir_obj==nil then error("Asset directory listing failed in CLI LuaFileSystem",0) end
    local names,visited,bytes={},0,0
    local ok,reason=pcall(function()
        -- lfs.dir returns BOTH iterator and directory userdata. Dropping the
        -- userdata makes the real iterator fail even though mocks may work.
        for name in iter,dir_obj do
            if name~="." and name~=".." then
                visited=visited+1;if visited>MAX_ENTRIES then error("Asset directory entry limit",0) end
                local mode,why=lfs.symlinkattributes(dir.."/"..name,"mode")
                if not mode then error("Asset directory metadata failed: "..tostring(why),0) end
                if mode=="file" then
                    bytes=bytes+#name;if bytes>MAX_NAME_BYTES then error("Asset directory byte limit",0) end
                    names[#names+1]=name
                end
            end
        end
    end)
    if not ok then pcall(function() dir_obj:close() end);error(reason,0) end
    return names
end
function FileUtil.scan_dir(dir,pattern)
    if not safe_directory(dir) then error("Invalid relative asset directory",0) end
    local names
    if type(KAG)=="table" and type(KAG.list_assets)=="function" then
        local reason
        names,reason=KAG.list_assets(dir,MAX_ENTRIES,MAX_NAME_BYTES)
        if names==nil or reason~=nil then error("Asset directory listing failed: "..tostring(reason),0) end
    else names=standalone_lfs(dir) end
    names=validate_names(names)
    pattern=pattern or ".*"
    local files={}
    for _,name in ipairs(names) do if name:match(pattern) then files[#files+1]=name end end
    table.sort(files)
    return files
end
function FileUtil.scan_dirs(dirs,pattern)
    for _,dir in ipairs(dirs) do
        local files=FileUtil.scan_dir(dir,pattern)
        if #files>0 then return files end
    end
    return {}
end
return FileUtil
