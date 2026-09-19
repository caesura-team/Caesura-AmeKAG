#!/usr/bin/env python3
"""Freeze the tracked required scope and exact source inputs before CI fanout.

The template's exact hosted names remain candidate expectations until a real
run verifies them. Preparing a policy is not evidence of successful jobs.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import stat

from ci_package_lane import _outputs
from package_verification import _sha256_file
from run_validation import _source_identity
from verify_execution_bundle import _no_links, _snapshot

PLATFORMS=("windows","linux","macos")
EXECUTION_ROLES={f"{platform}-{configuration}-execution" for platform in PLATFORMS for configuration in ("debug","release")}
PACKAGE_ROLES={platform+"-package" for platform in (*PLATFORMS,"web")}
JOB_ROLES={role.removesuffix("-execution") for role in EXECUTION_ROLES}|{"web-package","ios-compile","android-static"}
TEMPLATE_KEYS={"schema_version","required_jobs","artifact_roles","output_prefixes","execution_inputs","package_inputs"}


def _need(condition,message):
    if not condition:raise ValueError(message)


def _read(path):
    path=_no_links(path)
    digest=_sha256_file(path)
    _,value=_snapshot(path,digest,"policy source")
    return value,digest


def _output_destination(repo,policy_path,github_output):
    if github_output is None:return None
    path=Path(github_output).absolute()
    parent=_no_links(path.parent)
    _need(not path.is_symlink() and not path.is_junction(),"GitHub output must not be linked")
    _need(not any((item/".git").exists() for item in (parent,*parent.parents))
          and not path.is_relative_to(_no_links(repo)),"GitHub output must be outside source checkouts")
    frozen=Path(policy_path).absolute()
    _need(os.path.normcase(str(path.resolve()))!=os.path.normcase(str(frozen.resolve())),
          "GitHub output must differ from the frozen policy")
    if path.exists():
        info=path.stat()
        _need(stat.S_ISREG(info.st_mode) and info.st_nlink==1,"GitHub output must be a single plain file")
        _no_links(path)
    return path


def prepare_policy(*,repo,output,source_sha,release_tag=None):
    repo=_no_links(repo)
    _need((repo/".git").exists(),"A source checkout is required")
    _need(isinstance(source_sha,str) and re.fullmatch("[0-9a-f]{40}",source_sha),"Expected full source SHA required")
    before=dict(_source_identity(repo))
    _need(before.get("source_sha")==source_sha,"Wrong source checkout")
    _need(before.get("dirty") is False,"Policy requires a clean source checkout")
    output=Path(output).absolute()
    parent=_no_links(output.parent)
    _need(not output.is_relative_to(repo),"Frozen policy must be outside source checkout")
    _need(not any((item/".git").exists() for item in (parent,*parent.parents)),"Policy output must be outside every checkout")
    if output.exists() or output.is_symlink():raise FileExistsError(output)
    template_path=repo/"scripts/release_input_policy.json"
    template,template_sha=_read(template_path)
    _need(set(template)==TEMPLATE_KEYS and type(template["schema_version"]) is int and template["schema_version"]==1,
          "Invalid policy template schema")
    policy=copy.deepcopy(template)
    jobs=policy["required_jobs"]
    _need(isinstance(jobs,dict) and set(jobs)==JOB_ROLES,"Policy required jobs must retain the complete declared scope")
    _need(all(isinstance(name,str) and 0<len(name)<=300 and not any(c in name for c in "\r\n\x00") for name in jobs.values()),
          "Invalid exact required job name")
    _need(len(set(jobs.values()))==len(jobs),"Required job names must be distinct")
    roles=policy["artifact_roles"]
    _need(isinstance(roles,dict) and set(roles)==EXECUTION_ROLES|PACKAGE_ROLES,"Policy required artifact roles differ")
    _need(all(isinstance(job,str) and job in jobs for job in roles.values()),"Unknown required producer role")
    prefixes=policy["output_prefixes"]
    _need(isinstance(prefixes,dict) and set(prefixes)==set(roles),"Missing output prefix role")
    _need(all(isinstance(value,str) and re.fullmatch("[a-z][a-z0-9_]{0,99}",value) for value in prefixes.values())
          and len(set(prefixes.values()))==len(prefixes),"Output prefixes must be distinct and valid")
    profile_path=repo/"scripts/validation_profiles.json"
    profiles,profile_sha=_read(profile_path)
    executions=policy["execution_inputs"]
    _need(isinstance(executions,dict) and set(executions)==EXECUTION_ROLES,"Required execution roles differ")
    for role,spec in executions.items():
        _need(isinstance(spec,dict) and set(spec)=={"profile_name","platform","configuration"},"Invalid execution specification")
        name=role.removesuffix("-execution")
        _need(spec["profile_name"]==name and roles[role]==name,"Execution role and profile mapping differ")
        _need(isinstance(spec["platform"],str) and spec["platform"] in PLATFORMS
              and spec["configuration"] in ("Debug","Release")
              and name==spec["platform"]+"-"+spec["configuration"].lower(),"Execution profile context differs")
        profile=profiles.get("profiles",{}).get(name)
        _need(isinstance(profile,dict) and all(profile.get(key)==spec[key] for key in ("platform","configuration")),
              "Selected validation profile does not match lane context")
    cmake_path=_no_links(repo/"CMakeLists.txt")
    cmake_sha=_sha256_file(cmake_path)
    cmake=cmake_path.read_text(encoding="utf-8-sig")
    versions=re.findall(r"(?im)^\s*project\(\s*CaesuraAmeKAG\s+VERSION\s+(\d+\.\d+\.\d+(?:\.\d+)?)\s+LANGUAGES\b",cmake)
    _need(len(versions)==1,"One explicit engine version is required")
    version=versions[0]
    _need(release_tag is None or release_tag=="v"+version,"Release tag differs from engine version")
    packages=policy["package_inputs"]
    _need(isinstance(packages,dict) and set(packages)==PACKAGE_ROLES,"Required package roles differ")
    names=set()
    for role,spec in packages.items():
        _need(isinstance(spec,dict) and set(spec)=={"platform","configuration","job_key","required_files"},"Invalid package specification")
        platform=role.removesuffix("-package")
        _need(spec["platform"]==platform and spec["configuration"]=="Release"
              and roles[role]==("web-package" if platform=="web" else platform+"-release"),"Package context/producer role differs")
        _need(isinstance(spec["job_key"],str) and re.fullmatch("[a-z][a-z0-9_-]{0,99}",spec["job_key"]),"Invalid producer job key")
        files=spec["required_files"]
        _need(isinstance(files,dict) and 1<=len(files)<=4,"Explicit final files required")
        selected={}
        for template_name,kind in files.items():
            _need(isinstance(template_name,str) and kind=="file","Final package must be an explicit file")
            name=template_name.replace("{version}",version)
            _need(0<len(name)<=255 and name not in (".","..") and not any(c in name for c in "/\\{}:\r\n\x00"),"Invalid final package filename")
            _need(name.casefold() not in names,"Duplicate final package filename")
            names.add(name.casefold());selected[name]=kind
        spec["required_files"]=selected
    for path,digest in ((template_path,template_sha),(profile_path,profile_sha),(cmake_path,cmake_sha)):
        _need(_sha256_file(path)==digest,"Source input changed during policy preparation")
    _need(_source_identity(repo)==before,"Source checkout changed during policy preparation")
    policy.update(source_sha=source_sha,version=version,template_sha256=template_sha,
                  source_files={"CMakeLists.txt":cmake_sha,"scripts/validation_profiles.json":profile_sha})
    text=json.dumps(policy,ensure_ascii=False,separators=(",",":"))
    raw=(text+"\n").encode("utf-8")
    with output.open("xb") as stream:stream.write(raw)
    return {"status":"POLICY_FROZEN","release_ready":False,"policy_path":str(output),
            "policy_sha256":hashlib.sha256(raw).hexdigest(),"profile_sha256":profile_sha,"version":version,"policy_json":text}


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo",type=Path,default=Path.cwd())
    parser.add_argument("--source-sha",default=os.environ.get("SOURCE_SHA"))
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--release-tag")
    parser.add_argument("--github-output",type=Path)
    args=parser.parse_args(argv)
    try:
        destination=_output_destination(args.repo,args.output,args.github_output)
        result=prepare_policy(repo=args.repo,output=args.output,source_sha=args.source_sha,release_tag=args.release_tag)
        _outputs(destination,{key:result[key] for key in ("policy_sha256","profile_sha256","version","policy_json")})
        print(json.dumps({key:value for key,value in result.items() if key!="policy_json"},ensure_ascii=False))
        return 0
    except (OSError,ValueError,RuntimeError,KeyError,TypeError) as error:
        print(json.dumps({"status":"FAIL","release_ready":False,"error":str(error)},ensure_ascii=False))
        return 1


if __name__=="__main__":raise SystemExit(main())
