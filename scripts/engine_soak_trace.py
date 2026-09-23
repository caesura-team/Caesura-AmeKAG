"""Single-context workload trace checks; no process, image or recovery execution.

An empty error list is only this trace's acceptance, not a complete soak gate.
The controller must separately authenticate inputs/binaries/modules, inspect PNG
bytes and prove the required context/cold restart and real negative controls.
"""

import copy
import math

from engine_soak_contract import CACHES, EXACT, GROWTH_BUDGETS, UINT64_MAX, check_quiet

LIMITS = {"diagnostic": (0.0, 2), "short": (120.0, 20), "long": (3600.0, 600)}


class TraceError(ValueError):
    pass


def _need(ok, reason):
    if not ok:
        raise TraceError(reason)


def _uint(value, label, minimum=0):
    _need(type(value) is int and minimum <= value <= UINT64_MAX, label + ": invalid uint64")
    return value


def _number(value, label):
    _need(type(value) in (int, float) and math.isfinite(value) and value >= 0,
          label + ": invalid nonnegative finite number")
    return value


def _get(document, path):
    for part in path.split("."):
        _need(isinstance(document, dict) and part in document, path + ": missing field")
        document = document[part]
    return document


def _set(document, path, value):
    group, name = path.split(".")
    document[group][name] = value


def check_trace(result, events, expected_process, observed_seconds, mode):
    """Check one context's ordered trace; never infer execution from status alone.

    expected_process and observed_seconds must come from the controlled owner,
    outside the probe. File names/PNG metadata here are a structural contract;
    this function does not read or authenticate the corresponding image bytes.
    Context recovery, cold restart and deliberate faults are separate controls.
    """
    try:
        _need(mode in LIMITS, "Unknown trace mode")
        minimum_seconds, minimum_cycles = LIMITS[mode]
        _need(isinstance(result, dict) and isinstance(expected_process, dict), "Missing result/owner")
        _need(result.get("status") == "PROBE_COMPLETED", "Probe did not complete")
        pid = _uint(result.get("pid"), "result.pid", 1)
        _need(pid == _uint(expected_process.get("pid"), "owner.pid", 1), "Wrong owned PID")
        created = result.get("process_created")
        _need(type(created) is str and created.isascii() and created.isdecimal()
              and 0 < len(created) <= 30 and int(created) > 0
              and created == expected_process.get("created"), "Wrong owned creation identity")
        _need(result.get("shutdown_host") == {"initialized": False, "running": False}
              and all(type(value) is bool for value in result["shutdown_host"].values()),
              "Shutdown state missing or not retired")
        _need(_uint(result.get("warm_cycles"), "warm_cycles") == 20, "Warm count changed")
        count = _uint(result.get("completed_cycles"), "completed_cycles", 21)
        _need(count <= 100000, "Cycle count exceeds bounded trace contract")
        measured_count = _uint(result.get("measured_cycles"), "measured_cycles")
        _need(measured_count == count - 20 and measured_count >= minimum_cycles,
              "Incomplete measured cycle count")
        process_seconds = _number(result.get("process_seconds"), "process_seconds")
        measured_seconds = _number(result.get("measured_seconds"), "measured_seconds")
        owned_seconds = _number(observed_seconds, "owner elapsed time")
        _need(0 < process_seconds <= owned_seconds, "Probe elapsed exceeds observed owner interval")
        _need(isinstance(events, list) and 0 < len(events) <= count * 150 + 2, "Missing/oversized events")
        previous_time, previous_frame = 0.0, 0
        for index, event in enumerate(events):
            _need(isinstance(event, dict) and isinstance(event.get("detail"), dict), f"event {index}: missing object")
            seconds = _number(event.get("seconds"), f"event {index}.seconds")
            frame = _uint(event.get("owner_frame"), f"event {index}.owner_frame")
            _uint(event.get("cycle"), f"event {index}.cycle")
            _need(seconds >= previous_time and frame >= previous_frame, "Event clock/frame moved backwards")
            _need(seconds - previous_time < 10, "Progress stopped between events")
            _need(seconds <= process_seconds, "Event extends beyond process interval")
            previous_time, previous_frame = seconds, frame
        _need(_uint(result.get("completed_owner_frames"), "completed_owner_frames") >= previous_frame,
              "Result omits completed owner frames")

        cursor = 0

        def take(name, cycle):
            nonlocal cursor
            _need(cursor < len(events), "Truncated trace at " + name)
            event = events[cursor]
            _need(event.get("event") == name and event["cycle"] == cycle,
                  f"Expected {name} for cycle {cycle} at event {cursor}")
            cursor += 1
            return event

        initialized = take("initialized", 0)
        generation = _uint(_get(initialized["detail"], "render.contextGeneration"), "context generation", 1)
        screenshot_generation = None
        seen_requests = set()
        warm = []
        baseline = None
        measurement_start = None
        final_time = None
        for cycle in range(count):
            allocated = take("transient_resources", cycle)
            allocation = allocated["detail"]
            texture = _uint(allocation.get("texture"), "admitted texture", 1)
            target = _uint(allocation.get("target"), "admitted target", 1)
            _need(allocation.get("texture_valid") is True, "Transient texture was not valid")
            _uint(allocation.get("logical_texture_bytes"), "allocated texture bytes", 1)
            begin = take("cycle_begin", cycle)
            _uint(begin["detail"].get("texture"), "begin texture", 1)
            _uint(begin["detail"].get("target"), "begin target", 1)
            _need(begin["detail"] == {"texture": texture, "target": target}, "Allocation/begin identities differ")
            for page in ("a", "b", "restored"):
                admitted = take("capture_admitted", cycle)["detail"]
                _need(admitted.get("page") == page, "Wrong admitted screenshot page")
                request = _uint(admitted.get("request_id"), "capture request", 1)
                ticket_generation = _uint(admitted.get("generation"), "capture generation", 1)
                # Screenshot ticket and graphics-context generations are distinct
                # allocators. Each is stable here; they need not have equal values.
                if screenshot_generation is None:
                    screenshot_generation = ticket_generation
                _need(ticket_generation == screenshot_generation and request not in seen_requests,
                      "Reused ticket or substituted screenshot generation")
                seen_requests.add(request)
                consumed = take("capture_consumed", cycle)["detail"]
                _need(consumed.get("page") == page and _uint(consumed.get("request_id"), "consumed request", 1) == request,
                      "Consumed screenshot does not match admission")
                _need(consumed.get("file") == f"cycle-{cycle + 1}-{page}.png", "Wrong/unsafe screenshot filename")
                _uint(consumed.get("png_bytes"), "PNG byte count", 9)
                _uint(consumed.get("frame_id"), "screenshot frame", 1)
                pixels = consumed.get("pixel")
                color = (130, 35, 50) if page == "b" else (20, 50, 90)
                _need(isinstance(pixels, list) and len(pixels) == 3
                      and all(type(value) is int and 0 <= value <= 255 and abs(value - expected) <= 2
                              for value, expected in zip(pixels, color)), "Wrong reported screenshot pixel")
                if page == "a":
                    _uint(take("save", cycle)["detail"].get("bytes"), "encrypted save size", 33)
                if page == "b":
                    take("load", cycle)
            activities = take("activities_admitted", cycle)["detail"]
            _uint(activities.get("async_id"), "async admission", 1)
            voices = activities.get("voice_handles")
            _need(isinstance(voices, list) and len(voices) == 3, "Missing voice admissions")
            _need(len({_uint(handle, "voice admission", 1) for handle in voices}) == 3, "Voice handles are not distinct")
            finished = take("activities_finished", cycle)["detail"]
            for key, expected in (("completed", 1), ("natural", 1), ("cancelled_admissions", 8)):
                _need(_uint(finished.get(key), key) == expected, "Incomplete activity " + key)
            rollback = take("rollback", cycle)
            settling = []
            while cursor < len(events) and events[cursor].get("event") == "settling":
                settling.append(take("settling", cycle))
            _need(3 <= len(settling) <= 120, "Need three bounded consecutive quiet observations")
            quiet = take("quiet", cycle + 1)
            _need(quiet["seconds"] - begin["seconds"] < 10, "Cycle watchdog exceeded")
            _need(quiet["seconds"] - rollback["seconds"] < 4
                  and quiet["owner_frame"] - rollback["owner_frame"] <= 120, "Quiet settling budget exceeded")
            tail = settling[-3:]
            for prior, current in zip(tail, tail[1:]):
                _need(current["owner_frame"] == prior["owner_frame"] + 1, "Quiet observations are not consecutive frames")
            for item in tail:
                sample = item["detail"]
                errors = check_quiet(sample, sample)
                _need(not errors, "Ineligible quiet observation: " + "; ".join(errors))
                _need(_get(sample, "host.completedOwnerFrames") == item["owner_frame"], "Snapshot owner frame differs")
                _need(_get(sample, "render.contextGeneration") == generation, "Context changed inside trace")
                for path in EXACT:
                    _need(_get(sample, path) == _get(tail[-1]["detail"], path), "Quiet resources have not settled: " + path)
            _need(quiet["detail"] == tail[-1]["detail"] and quiet["owner_frame"] == tail[-1]["owner_frame"],
                  "Quiet checkpoint differs from last settled observation")
            if cycle < 20:
                warm.append(quiet["detail"])
                if cycle == 19:
                    baseline = copy.deepcopy(warm[-1])
                    for path in (*CACHES, *GROWTH_BUDGETS):
                        _set(baseline, path, max(_get(sample, path) for sample in warm))
                    for sample in warm[-3:]:
                        errors = check_quiet(sample, baseline)
                        _need(not errors, "Warm resources still grow: " + "; ".join(errors))
                    start = take("measurement_begin", 20)
                    # Clock reads and the flushed quiet record precede this
                    # event. Global ordering disallows an earlier clock, and a
                    # later start only shortens the accepted measured span.
                    _need(start["owner_frame"] == quiet["owner_frame"],
                          "Measurement start is not the warm checkpoint")
                    measurement_start = start["seconds"]
            else:
                errors = check_quiet(quiet["detail"], baseline)
                _need(not errors, "Measured quiet boundary: " + "; ".join(errors))
            final_time = quiet["seconds"]
        _need(cursor == len(events), "Unexpected events after final cycle")
        span = final_time - measurement_start
        _need(span >= minimum_seconds, "Actual measured trace is too short")
        _need(span <= measured_seconds <= span + 1.0 and measured_seconds <= process_seconds,
              "Reported measurement time differs from trace")
        return []
    except (TraceError, KeyError, TypeError, OverflowError) as error:
        return [str(error) or type(error).__name__]
