-- Standalone CLI capability controls. The root supplies real fixture files in
-- a private CWD; this test does not use shell enumeration or fabricate listings.
local directory=assert(arg[1],"fixture relative directory required")
local source=assert(arg[2],"source scripts directory required")
package.path=source.."/?.lua;"..package.path
local fs=require("caesura_cli_fs")
assert(debug.getinfo(fs.list_dir,"S").what=="C","actual CLI C binding required")
local checks=0
local function check(value,why) checks=checks+1;assert(value,why) end
local files,reason=fs.list_dir(directory,4096,1048576)
check(type(files)=="table" and reason==nil,"complete native listing")
check(#files==3 and files[1]=="alpha.ogg" and files[2]=="zeta.wav" and files[3]=="音楽.wav","UTF8 leaf/sort/nonrecursive")
for _,path in ipairs({"../escape","/escape","C:/escape","https://example.invalid/assets","assets\\escape","assets/../escape","assets"..string.char(0).."/escape"}) do
 local value,error=fs.list_dir(path,4096,1048576)
 check(value==nil and type(error)=="string","unsafe path must refuse")
end
for _,limit in ipairs({0,-1,4097,1.5,false,"4096",0/0,math.huge}) do
 local value,error=fs.list_dir(directory,limit,1048576)
 check(value==nil and type(error)=="string","invalid bound must refuse")
end
local value,error=fs.list_dir(directory,1,1048576)
check(value==nil and type(error)=="string","entry budget refusal")
value,error=fs.list_dir(directory,4096,25)
check(value==nil and type(error)=="string","UTF8 byte budget refusal")
value,error=fs.list_dir(directory,4096,1048577)
check(value==nil and type(error)=="string","policy byte ceiling refusal")
local util=require("fileutil")
check(#util.scan_dir(directory,"%.wav$")==2,"real fileutil routes through CLI native enumeration")
local original=KAG;local runtime_calls=0
KAG={list_assets=function()runtime_calls=runtime_calls+1;return nil,"CONTROL_RUNTIME_UNSUPPORTED" end}
local ok,why=pcall(util.scan_dir,directory)
check(not ok and tostring(why):find("CONTROL_RUNTIME_UNSUPPORTED",1,true) and runtime_calls==1,"runtime rejection must not fall back")
value,error=fs.list_dir(directory,4096,1048576)
check(value==nil and tostring(error):find("authoritative",1,true),"direct CLI call must not bypass runtime capability")
KAG=original
_CAESURA_CONFIG={dev_mode=false}
assert(loadfile(source.."/sandbox.lua"))()
check(_SANDBOX_MODE=="strict","actual strict sandbox loaded")
value,error=fs.list_dir(directory,4096,1048576)
check(value==nil and tostring(error):find("strict",1,true),"captured CLI module denied after strict lockdown")
ok,why=pcall(util.scan_dir,directory)
check(not ok and tostring(why):find("unsupported",1,true),"strict fileutil cannot use CLI fallback")
print("CLI_ASSET_DIRECTORY_CONTROLS_PASS checks="..checks)
