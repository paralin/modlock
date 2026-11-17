# Tests of the Python library's handler timing, run against the generated
# module with a stand-in for the runtime's _modlock:
#
#   python3 -I python/profile_test.py

import json
import pathlib
import sys
import time
import types
import unittest
from unittest import mock

# The stand-in runtime answers every host call with an empty reply and keeps
# the library's event function.
_runtime = types.ModuleType("_modlock")
_runtime.call = lambda request: b""
_runtime.log = lambda line: None
_runtime.handle = lambda handler: setattr(_runtime, "handler", handler)
sys.modules["_modlock"] = _runtime
sys.path.insert(0, str(pathlib.Path(__file__).parent))

import modlock  # noqa: E402
from modlock import wire  # noqa: E402

# The mod's handlers, each defined on its own line of this file.
counts = {"frames": 0, "ticks": 0, "commands": 0}
spend = {"ms": 0.0}
fake = {"now": 0.0}


@modlock.on_frame
def count_frame(event: modlock.FrameEvent) -> None:
    counts["frames"] += 1


modlock.every(0.01, lambda: counts.__setitem__("ticks", counts["ticks"] + 1))


@modlock.command("go")
def go(player: modlock.Player, args: str) -> None:
    counts["commands"] += 1


@modlock.on_world
def slow_world(name: str) -> None:
    fake["now"] += spend["ms"] / 1000


def deliver(method: str, kind: str, request: object) -> dict:
    """deliver sends the mod one host event and returns its decoded reply."""
    schema = modlock._SCHEMA
    call = wire.encode(schema, "Call", {"method": method, "request": wire.encode(schema, kind, request)})
    return wire.decode(schema, "Reply", _runtime.handler(call))


def frame(seconds: float) -> None:
    """frame delivers a frame at seconds on the game clock."""
    reply = deliver("Frame", "FrameEvent", modlock.FrameEvent(time_seconds=seconds))
    assert not reply["error"], reply["error"]


def profile(method: str) -> dict | None:
    """profile calls the profiler's service with method and decodes its answer."""
    reply = deliver("Serve", "ServiceCall", modlock.ServiceCall(service="modlock.profile", method=method))
    assert not reply["error"], reply["error"]
    payload = wire.decode(modlock._SCHEMA, "ServiceReply", reply["response"]).payload
    return json.loads(payload) if payload else None


def line_of(code: str) -> int:
    """line_of returns the line of this file that starts with code."""
    lines = pathlib.Path(__file__).read_text().splitlines()
    return next(i for i, text in enumerate(lines, 1) if text.startswith(code))


class ProfileTest(unittest.TestCase):
    def test_1_without_the_profiler_no_clock_is_read(self) -> None:
        with mock.patch.object(time, "perf_counter", side_effect=AssertionError("read the clock")) as clock:
            for i in range(1, 6):
                deliver("Command", "CommandEvent", modlock.CommandEvent(player=0, line="go"))
                frame(i)
        self.assertEqual(clock.call_count, 0)
        self.assertEqual(counts["frames"], 5)
        self.assertEqual(counts["commands"], 5)
        self.assertGreater(counts["ticks"], 0)

    def test_2_the_profiler_times_each_handler_by_its_function(self) -> None:
        # The first read turns timing on and reports nothing yet.
        self.assertEqual(profile("read")["sites"], [])
        for i in range(10, 14):
            deliver("Command", "CommandEvent", modlock.CommandEvent(player=0, line="go"))
            frame(i)

        # The next read has each handler's calls, located at its function.
        read = profile("read")
        self.assertEqual(read["frames"]["calls"], 4)
        sites = {row["kind"]: row for row in read["sites"]}
        self.assertEqual(sites["on_frame"]["calls"], 4)
        self.assertEqual(sites["every"]["calls"], 4)
        self.assertEqual(sites["command /go"]["calls"], 4)
        self.assertTrue(sites["on_frame"]["at"].endswith(f"profile_test.py:{line_of('@modlock.on_frame')}"))
        self.assertTrue(sites["every"]["at"].endswith(f"profile_test.py:{line_of('modlock.every(')}"))
        self.assertTrue(sites["command /go"]["at"].endswith(f"profile_test.py:{line_of('@modlock.command')}"))

        # Stop turns timing off again.
        self.assertIsNone(profile("stop"))
        with mock.patch.object(time, "perf_counter") as clock:
            frame(14)
        self.assertEqual(clock.call_count, 0)

    def test_3_the_profiler_lists_each_event_over_4_ms(self) -> None:
        profile("read")
        with mock.patch.object(time, "perf_counter", side_effect=lambda: fake["now"]):
            # Twenty-two slow world events, the first 6 ms and the rest 5 ms.
            for i in range(22):
                spend["ms"] = 6.0 if i == 0 else 5.0
                deliver("World", "WorldEvent", modlock.WorldEvent(map="dl_midtown"))
            read = profile("read")

        # The first sixteen are listed with their busiest handler, and the
        # rest counted.
        self.assertEqual(len(read["slow"]), 16)
        self.assertEqual(read["slowMore"], 6)
        first = read["slow"][0]
        self.assertEqual((first["event"], first["kind"]), ("World", "on_world"))
        self.assertAlmostEqual(first["ms"], 6.0)
        self.assertAlmostEqual(first["siteMs"], 6.0)
        self.assertTrue(first["at"].endswith(f"profile_test.py:{line_of('@modlock.on_world')}"))
        self.assertEqual(profile("read")["slow"], [])
        profile("stop")


if __name__ == "__main__":
    unittest.main()
