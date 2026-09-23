"""Synthetic traces test rejection rules; they are not native execution proof."""
import copy
import math
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from engine_soak_trace import check_trace
from test_engine_soak_contract import measured


def fixture(cycles=40, cycle_seconds=6.1):
    events = [{"event":"initialized", "cycle":0, "owner_frame":0,
               "seconds":0.0, "detail":measured()}]
    frame = 0
    request = 0
    for cycle in range(cycles):
        names = [("transient_resources", {"texture":100+cycle, "texture_valid":True,
                   "target":200+cycle, "logical_texture_bytes":4100}),
                 ("cycle_begin", {"texture":100+cycle, "target":200+cycle})]
        for page in ("a", "b", "restored"):
            request += 1
            names += [("capture_admitted", {"page":page,"request_id":request,"generation":7}),
                      ("capture_consumed", {"page":page,"request_id":request,
                        "file":f"cycle-{cycle+1}-{page}.png", "png_bytes":100,
                        "frame_id":request,"pixel":[130,35,50] if page=="b" else [20,50,90]})]
            if page == "a": names.append(("save", {"bytes":900}))
            if page == "b": names.append(("load", {}))
        names += [("activities_admitted", {"async_id":cycle+1,"voice_handles":[3*cycle+1,3*cycle+2,3*cycle+3]}),
                  ("activities_finished", {"completed":1,"natural":1,"cancelled_admissions":8}),
                  ("rollback", {})]
        for offset,(name,detail) in enumerate(names):
            frame += 1
            seconds=cycle*cycle_seconds+offset*.01
            if name=="activities_finished":seconds=(cycle+1)*cycle_seconds-.08
            if name=="rollback":seconds=(cycle+1)*cycle_seconds-.07
            events.append(dict(event=name,cycle=cycle,owner_frame=frame,
                               seconds=seconds,detail=detail))
        for index in range(3):
            frame += 1
            sample=measured();sample["host"]["completedOwnerFrames"]=frame
            sample["render"]["captureSubmissionFrame"]=request
            events.append(dict(event="settling",cycle=cycle,owner_frame=frame,
                               seconds=(cycle+1)*cycle_seconds-.02+index*.005,detail=sample))
        events.append(dict(event="quiet",cycle=cycle+1,owner_frame=frame,
                           seconds=(cycle+1)*cycle_seconds,detail=copy.deepcopy(sample)))
        if cycle == 19:
            events.append(dict(event="measurement_begin",cycle=20,owner_frame=frame,
                               seconds=(cycle+1)*cycle_seconds,detail={}))
    total=cycles*cycle_seconds
    result=dict(status="PROBE_COMPLETED",pid=4321,process_created="134343123456789012",
                warm_cycles=20,completed_cycles=cycles,measured_cycles=cycles-20,
                process_seconds=total+.1,measured_seconds=(cycles-20)*cycle_seconds+.1,
                completed_owner_frames=frame+1,shutdown_host={"initialized":False,"running":False},
                device_backend={"id":4,"name":"WinMM","sample_rate":44100,"reported_buffer_size":8192},
                context_restarts=0,cold_restart="NOT_RUN")
    process={"pid":4321,"created":"134343123456789012"}
    return result,events,process,total+1,"short"


class SoakTraceTests(unittest.TestCase):
    def setUp(self): self.args=list(fixture())

    def reject(self):
        errors=check_trace(*self.args)
        self.assertTrue(errors)
        self.assertTrue(all(isinstance(e,str) and e for e in errors))

    def event(self,name,cycle=20):
        return next(e for e in self.args[1] if e["event"]==name and e["cycle"]==cycle)

    def test_complete_trace_passes_without_modifying_observations(self):
        before=copy.deepcopy(self.args)
        self.assertEqual(check_trace(*self.args),[])
        self.assertEqual(self.args,before)

    def test_diagnostic_is_not_a_short_or_long_duration(self):
        for mode in ["short","long"]:
            with self.subTest(mode=mode):
                self.args=list(fixture(22,.6));self.args[4]=mode;self.reject()
        self.args[4]="diagnostic"
        self.assertEqual(check_trace(*self.args),[])

    def test_ticket_and_renderer_generation_counters_are_independent(self):
        for event in self.args[1]:
            if event["event"]=="capture_admitted":event["detail"]["generation"]=19
        self.assertEqual(check_trace(*self.args),[])

    def test_idle_gap_between_cycles_cannot_supply_measured_time(self):
        boundary=self.args[1].index(self.event("transient_resources",21))
        for event in self.args[1][boundary:]:event["seconds"]+=100
        self.args[0]["process_seconds"]+=100
        self.args[0]["measured_seconds"]+=100
        self.args[3]+=100
        self.reject()

    def test_measurement_clock_can_start_after_checkpoint_on_same_owner_frame(self):
        # The native probe flushes quiet, sets its steady clock, then emits
        # measurement_begin. Those three times cannot be bit-identical.
        start=self.event("measurement_begin",20)
        start["seconds"]+=.00001
        # The next cycle starts after that newly observed timestamp too.
        self.event("transient_resources",20)["seconds"]+=.00001
        self.assertEqual(check_trace(*self.args),[])

    def test_self_reported_counts_cannot_replace_completed_cycles(self):
        for field in ["warm_cycles","completed_cycles","measured_cycles"]:
            for value in [True,-1,math.nan,40.0,0]:
                with self.subTest(field=field,value=value):
                    self.args=list(fixture());self.args[0][field]=value;self.reject()

    def test_wrong_process_identity_or_success_state_is_rejected(self):
        for field,value in [("pid",4322),("pid",True),("process_created","old"),("status","FAIL"),
                            ("shutdown_host",{"initialized":True,"running":False})]:
            with self.subTest(field=field):
                self.args=list(fixture());self.args[0][field]=value;self.reject()

    def test_missing_duplicate_or_reordered_activity_is_rejected(self):
        for name in ["save","load","rollback","activities_admitted","activities_finished","cycle_begin"]:
            for change in ["missing","duplicate"]:
                with self.subTest(name=name,change=change):
                    self.args=list(fixture());target=self.event(name);index=self.args[1].index(target)
                    if change=="missing":self.args[1].pop(index)
                    else:self.args[1].insert(index,copy.deepcopy(target))
                    self.reject()
        self.args=list(fixture());a=self.event("save");b=self.event("load")
        a["event"],b["event"]="load","save";self.reject()

    def test_unadmitted_callbacks_wrong_counts_and_reused_handles_are_rejected(self):
        for event,key,value in [("activities_admitted","async_id",0),
                                ("activities_admitted","voice_handles",[4,4,5]),
                                ("activities_finished","completed",0),
                                ("activities_finished","natural",True),
                                ("activities_finished","cancelled_admissions",0),
                                ("transient_resources","texture_valid",False),
                                ("cycle_begin","texture",99999),("save","bytes",0)]:
            with self.subTest(event=event,key=key):
                self.args=list(fixture());self.event(event)["detail"][key]=value;self.reject()

    def test_screenshot_identity_pair_and_safe_output_name_are_required(self):
        for key,value in [("request_id",999999),("page","restored"),("png_bytes",0),
                          ("file","../elsewhere.png"),("frame_id",True)]:
            with self.subTest(key=key):
                self.args=list(fixture());self.event("capture_consumed")["detail"][key]=value;self.reject()
        self.args=list(fixture());self.event("capture_admitted")["detail"]["generation"]=99;self.reject()

    def test_finite_monotonic_clock_and_owner_frames_are_required(self):
        for key,value in [("seconds",math.nan),("seconds",math.inf),("seconds",True),("seconds",-1),
                          ("owner_frame",True),("owner_frame",-1),("owner_frame",0)]:
            with self.subTest(key=key,value=value):
                self.args=list(fixture());self.event("load")[key]=value;self.reject()

    def test_reported_time_cannot_inflate_real_trace_or_owner_time(self):
        for field,value in [("measured_seconds",99999),("process_seconds",99999),
                            ("measured_seconds",True),("process_seconds",math.nan)]:
            with self.subTest(field=field):
                self.args=list(fixture());self.args[0][field]=value;self.reject()
        self.args=list(fixture());self.args[3]=1.0;self.reject()
        self.args=list(fixture());self.event("measurement_begin",20)["seconds"]-=60;self.reject()

    def test_three_real_consecutive_quiet_observations_are_required(self):
        self.args[1].remove(self.event("settling"));self.reject()
        self.args=list(fixture());self.event("settling")["detail"]["jobs"]["workerPending"]=1;self.reject()
        self.args=list(fixture());self.event("settling")["detail"]["host"]["completedOwnerFrames"]+=1;self.reject()

    def test_frozen_warm_baseline_rejects_growth_and_context_substitution(self):
        for group,key,value in [("memory","textureBytes",8192),("render","contextGeneration",8),
                                ("memory","luaBytes",3000000)]:
            with self.subTest(group=group,key=key):
                self.args=list(fixture());self.event("quiet",21)["detail"][group][key]=value;self.reject()
        self.args=list(fixture());self.event("quiet",21)["detail"]["render"]["resources"]["textures"]+=1;self.reject()

    def test_missing_sections_unknown_events_and_truncated_tail_fail_closed(self):
        for result,events in [({},[]),(self.args[0],[]),(None,None)]:
            with self.subTest(result=result is None):
                self.assertTrue(check_trace(result,events,self.args[2],self.args[3],"short"))
        self.args=list(fixture());self.event("load")["event"]="claimed_load";self.reject()
        self.args=list(fixture());self.args[1].pop();self.reject()

    def test_begin_resource_ids_require_integers_even_when_numeric_value_matches(self):
        for field in ["texture", "target"]:
            with self.subTest(field=field):
                self.args=list(fixture())
                detail=self.event("cycle_begin")["detail"]
                detail[field]=float(detail[field])
                self.reject()

    def test_consistent_settled_growth_is_rejected_against_frozen_warm_baseline(self):
        for path,delta in [(('render','resources','textures'),1),
                           (('memory','textureBytes'),1),
                           (('memory','luaBytes'),1024**2+1),
                           (('memory','privateBytes'),64*1024**2+1),
                           (('async','cacheEntries'),1)]:
            with self.subTest(path=path):
                self.args=list(fixture())
                for event in self.args[1]:
                    if ((event['event']=='settling' and event['cycle']==20)
                            or (event['event']=='quiet' and event['cycle']==21)):
                        node=event['detail']
                        for part in path[:-1]:node=node[part]
                        node[path[-1]]+=delta
                errors=check_trace(*self.args)
                self.assertTrue(errors)
                self.assertTrue(any('Measured quiet boundary' in error for error in errors),errors)

    def test_long_mode_accepts_a_complete_continuous_single_context_trace(self):
        self.args=list(fixture(620,6.1));self.args[4]='long'
        self.assertEqual(check_trace(*self.args),[])


if __name__=="__main__":unittest.main(verbosity=2)
