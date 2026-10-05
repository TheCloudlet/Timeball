# Copyright 2025-2026 Yi-Ping Pan (Cloudlet)
"""Every check reads the .pftrace back with TraceProcessor and compares it to
the SQLite file it came from, so nothing here depends on looking at the UI."""

import heapq
import sqlite3
import time
from collections import defaultdict

import pytest
from perfetto.trace_processor import TraceProcessor

import sqlite_to_perfetto as s2p

# The schema EventStore writes; the converter's input contract.
SCHEMA = """
CREATE TABLE ops(op INTEGER NOT NULL, name TEXT NOT NULL, resource TEXT NOT NULL,
  initiator INTEGER NOT NULL, addr INTEGER NOT NULL, depth INTEGER NOT NULL,
  forwarded INTEGER NOT NULL, arrival INTEGER NOT NULL, start INTEGER NOT NULL,
  finish INTEGER NOT NULL, work INTEGER NOT NULL, parent INTEGER NOT NULL);
CREATE TABLE deps(op INTEGER NOT NULL, depends_on INTEGER NOT NULL);
CREATE TABLE tasks(name TEXT NOT NULL, begin_cycle INTEGER NOT NULL,
  end_cycle INTEGER NOT NULL);
"""

# (op, name, resource, initiator, addr, depth, forwarded, arrival, start,
#  finish, work, parent), in the order the engine emits them: by finish, the
# primary resource first.
OPS = [
    # A cache miss: one op, a row per hop.
    (1, "load", "Q1", 0, 0x40, 0, 1, 0, 0, 4, 0, 0),
    # Two units of a capacity-2 buffer, then a zero-cost hop at the boundary.
    (14, "fill", "buffer", 0, 0, 0, 0, 0, 0, 8, 0, 0),
    (13, "fill", "buffer", 0, 0, 0, 0, 0, 0, 10, 0, 0),
    (15, "route", "buffer", 0, 0, 0, 0, 10, 10, 10, 0, 0),
    (3, "matmul", "tensor", 0, 0, 0, 0, 0, 0, 10, 0, 0),
    # Four loads queue at a crowded resource: peak 4 waiting, a counter.
    (6, "load", "dram_q", 1, 0x100, 0, 0, 0, 10, 11, 0, 0),
    (7, "load", "dram_q", 1, 0x140, 0, 0, 1, 11, 12, 0, 0),
    (8, "load", "dram_q", 2, 0x180, 0, 0, 2, 12, 13, 0, 0),
    (9, "load", "dram_q", 2, 0x1C0, 0, 0, 3, 13, 14, 0, 0),
    (4, "sync", "tensor", 0, 0, 0, 0, 0, 10, 15, 0, 0),
    (5, "stage", "transfer", 0, 0, 0, 0, 0, 0, 20, 0, 0),
    (10, "load", "dram_q", 1, 0x200, 0, 0, 20, 20, 25, 0, 0),
    # A multi-resource operation: deps on the primary row only.
    (2, "copy", "transfer", 0, 0, 0, 0, 15, 20, 32, 0, 0),
    (2, "copy", "link", 0, 0, 0, 0, 15, 20, 32, 0, 0),
    # Two waits overlap at link: peak 2, still slices, in two lanes.
    (16, "send", "link", 0, 0, 0, 0, 16, 32, 33, 0, 0),
    # A device-register write launches device work.
    (12, "store", "dma", 0, 0x10, 0, 0, 30, 30, 31, 0, 0),
    (11, "work", "dma.work", 7, 0, 0, 0, 31, 31, 40, 1, 12),
    (1, "load", "QMem", 0, 0x40, 1, 0, 4, 4, 104, 0, 0),
    # A writeback: op 0, below the level that sent it.
    (0, "store", "QMem", 0, 0x80, 1, 0, 104, 104, 204, 0, 0),
]
# "tail" is what EventStore writes for a task left open: it ends at the last
# cycle recorded.
DEPS = [(2, 3), (2, 4), (16, 2)]
TASKS = [("warmup", 0, 20), ("steady", 20, 50), ("tail", 50, 204)]

ARG_COLUMNS = ("op", "name", "resource", "initiator", "addr", "depth",
               "forwarded", "arrival", "start", "finish", "work", "parent")


def make_db(path, ops=OPS, deps=DEPS, tasks=TASKS):
    db = sqlite3.connect(path)
    db.executescript(SCHEMA)
    db.executemany("INSERT INTO ops VALUES(?,?,?,?,?,?,?,?,?,?,?,?)", ops)
    db.executemany("INSERT INTO deps VALUES(?,?)", deps)
    db.executemany("INSERT INTO tasks VALUES(?,?,?)", tasks)
    db.commit()
    db.close()
    return path


class Trace:
    """A converted trace and the queries the checks share."""

    def __init__(self, tp):
        self.tp = tp

    def rows(self, sql):
        return [r.__dict__ for r in self.tp.query(sql)]

    def args(self, arg_set_id):
        """Debug args as a nested dict, arrays as lists."""
        out = {}
        for r in self.rows(f"SELECT key, int_value, string_value, real_value "
                           f"FROM args WHERE arg_set_id = {arg_set_id} "
                           f"AND key GLOB 'debug.*'"):
            value = next((r[k] for k in ("int_value", "string_value",
                                         "real_value") if r[k] is not None),
                         None)
            _assign(out, r["key"][len("debug."):], value)
        return out

    def slices(self, kind=None):
        """Slices with their resource (parent track) and lane (track)."""
        where = ""
        if kind == "work":
            where = "WHERE t.name GLOB 'work*'"
        elif kind == "wait":
            where = "WHERE t.name GLOB 'wait*'"
        elif kind == "tasks":
            where = "WHERE t.name = 'tasks'"
        return self.rows(
            "SELECT s.id, s.ts, s.dur, s.name, s.arg_set_id, s.track_id, "
            "t.name AS lane, p.name AS resource FROM slice s "
            "JOIN track t ON s.track_id = t.id "
            f"LEFT JOIN track p ON t.parent_id = p.id {where}")

    def counters(self, resource):
        return self.rows(
            "SELECT c.ts, c.value, c.arg_set_id FROM counter c "
            "JOIN track t ON c.track_id = t.id JOIN track p "
            f"ON t.parent_id = p.id WHERE p.name = '{resource}' ORDER BY c.ts")

    def counter_resources(self):
        return {r["name"] for r in self.rows(
            "SELECT DISTINCT p.name FROM counter c JOIN track t "
            "ON c.track_id = t.id JOIN track p ON t.parent_id = p.id")}


def _assign(out, key, value):
    # "depends_on[1].op" -> out["depends_on"][1]["op"]
    node = out
    parts = key.replace("]", "").replace("[", ".").split(".")
    for part, nxt in zip(parts, parts[1:]):
        container = [] if nxt.isdigit() else {}
        if part.isdigit():
            idx = int(part)
            while len(node) <= idx:
                node.append(None)
            if node[idx] is None:
                node[idx] = container
            node = node[idx]
        else:
            node = node.setdefault(part, container)
    last = parts[-1]
    if last.isdigit():
        idx = int(last)
        while len(node) <= idx:
            node.append(None)
        node[idx] = value
    else:
        node[last] = value


def convert(tmp_path, name="run", db=None, **options):
    db = db or make_db(tmp_path / f"{name}.sqlite")
    out = tmp_path / f"{name}.pftrace"
    s2p.convert(db, out, **options)
    trace = Trace(TraceProcessor(trace=str(out)))
    # Perfetto accepted every packet as written.
    assert trace.rows("SELECT name, value FROM stats WHERE value > 0 AND "
                      "severity IN ('error', 'data_loss')") == []
    return trace


@pytest.fixture(scope="module")
def full(tmp_path_factory):
    trace = convert(tmp_path_factory.mktemp("full"), flows=True)
    yield trace
    trace.tp.close()


def db_rows(ops=OPS):
    return [dict(zip(ARG_COLUMNS, row), row=i + 1) for i, row in
            enumerate(ops)]


def waiting(ops=OPS):
    return [r for r in db_rows(ops) if r["start"] > r["arrival"]]


def peak_waits(rows):
    peak = defaultdict(int)
    by_resource = defaultdict(list)
    for r in rows:
        by_resource[r["resource"]].append(r)
    for resource, rs in by_resource.items():
        starts = []
        for r in sorted(rs, key=lambda r: r["arrival"]):
            while starts and starts[0] <= r["arrival"]:
                heapq.heappop(starts)
            heapq.heappush(starts, r["start"])
            peak[resource] = max(peak[resource], len(starts))
    return peak


# 1. One work slice per ops row, at its service interval.
def test_one_work_slice_per_row_at_its_service_interval(full):
    got = sorted((s["resource"], s["ts"], s["dur"]) for s in
                 full.slices("work"))
    want = sorted((r["resource"], r["start"], r["finish"] - r["start"]) for r
                  in db_rows())
    assert got == want


# 2/3. Uncrowded resources draw a wait slice per waiting row; crowded ones a
# counter instead, and no wait slices.
def test_waits_are_slices_up_to_two_and_a_counter_beyond(full):
    peak = peak_waits(waiting())
    assert peak["link"] == 2 and peak["dram_q"] == 4

    crowded = {r for r, p in peak.items() if p > 2}
    assert full.counter_resources() == crowded

    got = sorted((s["resource"], s["ts"], s["dur"]) for s in
                 full.slices("wait"))
    want = sorted((r["resource"], r["arrival"], r["start"] - r["arrival"])
                  for r in waiting() if r["resource"] not in crowded)
    assert got == want


def test_forcing_slices_draws_every_wait_and_no_counter(tmp_path):
    trace = convert(tmp_path, wait_slices=True)
    got = sorted((s["resource"], s["ts"], s["dur"]) for s in
                 trace.slices("wait"))
    want = sorted((r["resource"], r["arrival"], r["start"] - r["arrival"])
                  for r in waiting())
    assert got == want
    assert trace.counter_resources() == set()


# 4/5. The counter's value is the queue length, and each sample names what is
# waiting, oldest first.
def test_counter_counts_and_names_the_queue(full):
    rows = [r for r in waiting() if r["resource"] == "dram_q"]
    samples = full.counters("dram_q")
    times = sorted({r["arrival"] for r in rows} | {r["start"] for r in rows})
    assert [c["ts"] for c in samples] == times

    seen = set()
    for c in samples:
        t = c["ts"]
        queued = sorted((r for r in rows if r["arrival"] <= t < r["start"]),
                        key=lambda r: (r["arrival"], r["row"]))
        assert c["value"] == len(queued)
        args = full.args(c["arg_set_id"])
        assert args["queued"] == len(queued)
        assert args.get("omitted", 0) == 0
        listed = args.get("waiting", [])
        assert [w["row"] for w in listed] == [r["row"] for r in queued]
        for w, r in zip(listed, queued):
            assert (w["op"], w["name"], w["arrival"], w["start"]) == (
                r["op"], r["name"], r["arrival"], r["start"])
            assert w["waited"] == t - r["arrival"]
            assert w["wait_cycles"] == r["start"] - r["arrival"]
        seen.update(w["row"] for w in listed)
    # Every waiting row appears somewhere in the counter's details.
    assert seen == {r["row"] for r in rows}


def test_counter_lists_only_the_oldest_and_counts_the_rest(tmp_path):
    trace = convert(tmp_path, queue_detail=2)
    at_3 = next(c for c in trace.counters("dram_q") if c["ts"] == 3)
    args = trace.args(at_3["arg_set_id"])
    assert args["queued"] == 4
    assert args["omitted"] == 2
    assert [w["op"] for w in args["waiting"]] == [6, 7]


# 6. Every slice carries its row, so a click shows the full state.
def test_work_and_wait_slices_carry_their_row(tmp_path):
    trace = convert(tmp_path, wait_slices=True)
    by_row = {r["row"]: r for r in db_rows()}
    slices = trace.slices("work") + trace.slices("wait")
    assert slices
    for s in slices:
        args = trace.args(s["arg_set_id"])
        r = by_row[args["row"]]
        for column in ARG_COLUMNS:
            want = r[column]
            if column == "addr":
                want = hex(want)
            assert args[column] == want, (column, r)
        assert args["wait_cycles"] == r["start"] - r["arrival"]
        assert args["work_cycles"] == r["finish"] - r["start"]
        if s["lane"].startswith("wait"):
            assert s["name"] == f"wait {r['name']}"
        else:
            assert s["name"] == r["name"]


# 7. Dependencies on every slice of the op, and what released it.
def test_slices_show_dependencies_and_what_released_them(full):
    finish = defaultdict(int)
    names = {}
    for r in db_rows():
        finish[r["op"]] = max(finish[r["op"]], r["finish"])
        names[r["op"]] = r["name"]
    deps = defaultdict(list)
    for op, on in DEPS:
        deps[op].append(on)

    checked = 0
    for s in full.slices("work"):
        args = full.args(s["arg_set_id"])
        op = args["op"]
        if op not in deps:
            assert "depends_on" not in args and "released_by" not in args
            continue
        got = sorted((d["op"], d["name"], d["finish"]) for d in
                     args["depends_on"])
        assert got == sorted((d, names[d], finish[d]) for d in deps[op])
        assert finish[args["released_by"]] == max(finish[d] for d in deps[op])
        checked += 1
    # Both rows of the multi-resource copy, and the send.
    assert checked == 3


def test_flows_draw_one_arrow_per_dependency_in_the_output(full):
    flows = full.rows(
        "SELECT o.name AS src, i.name AS dst FROM flow f "
        "JOIN slice o ON f.slice_out = o.id JOIN slice i ON f.slice_in = i.id")
    assert sorted((f["src"], f["dst"]) for f in flows) == sorted(
        [("matmul", "copy"), ("sync", "copy"), ("copy", "send")])


def test_flows_need_both_ends_in_the_output(tmp_path):
    # From cycle 16 on, matmul and sync are cut, so only copy -> send is left.
    trace = convert(tmp_path, cycle_range=(16, 100), flows=True)
    flows = trace.rows(
        "SELECT o.name AS src, i.name AS dst FROM flow f "
        "JOIN slice o ON f.slice_out = o.id JOIN slice i ON f.slice_in = i.id")
    assert [(f["src"], f["dst"]) for f in flows] == [("copy", "send")]


def test_flows_scale_with_a_long_dependency_chain(tmp_path):
    # Each op waits on the one before, as a core port's work does. Looking up
    # an op's ends without a key makes this quadratic: minutes, not seconds.
    n = 20_000
    ops = [(i, "step", "core", 0, 0, 0, 0, i, i, i + 1, 0, 0)
           for i in range(1, n + 1)]
    deps = [(i, i - 1) for i in range(2, n + 1)]
    db = make_db(tmp_path / "chain.sqlite", ops=ops, deps=deps, tasks=[])
    began = time.monotonic()
    s2p.convert(db, tmp_path / "chain.pftrace", flows=True)
    # About 1 s keyed; over 20 s without the key.
    assert time.monotonic() - began < 5


def test_flows_are_off_by_default(tmp_path):
    trace = convert(tmp_path)
    assert trace.rows("SELECT COUNT(*) AS n FROM flow")[0]["n"] == 0


# 8. No lane holds two partially overlapping slices.
def test_no_lane_holds_overlapping_slices(tmp_path):
    trace = convert(tmp_path, wait_slices=True)
    by_lane = defaultdict(list)
    for s in trace.slices():
        by_lane[s["track_id"]].append((s["ts"], s["ts"] + s["dur"]))
    assert by_lane
    for spans in by_lane.values():
        spans.sort()
        for (_, end), (begin, _) in zip(spans, spans[1:]):
            assert begin >= end
    lanes = {(s["resource"], s["lane"]) for s in trace.slices()}
    assert ("buffer", "work 2") in lanes and ("link", "wait 2") in lanes


# 9. The tasks track is the tasks table.
def test_tasks_track_matches_the_table(full):
    got = []
    for s in full.slices("tasks"):
        args = full.args(s["arg_set_id"])
        got.append((s["name"], s["ts"], s["ts"] + s["dur"], args["name"],
                    args["begin_cycle"], args["end_cycle"]))
    assert sorted(got) == sorted((n, b, e, n, b, e) for n, b, e in TASKS)


# 10. Filters.
def overlaps(begin, end, lo, hi):
    return begin < hi and (end > lo or begin >= lo)


def test_cycle_range_keeps_only_overlapping_slices(tmp_path):
    trace = convert(tmp_path, cycle_range=(12, 31))
    for s in trace.slices():
        assert overlaps(s["ts"], s["ts"] + s["dur"], 12, 31)
    got = sorted((s["resource"], s["ts"]) for s in trace.slices("work"))
    want = sorted((r["resource"], r["start"]) for r in db_rows()
                  if overlaps(r["start"], r["finish"], 12, 31))
    assert got == want
    for c in trace.counters("dram_q"):
        assert 12 <= c["ts"] < 31


def test_cycle_range_opens_on_the_queue_already_waiting(tmp_path):
    trace = convert(tmp_path, cycle_range=(5, 31))
    samples = trace.counters("dram_q")
    assert [(c["ts"], c["value"]) for c in samples] == [
        (5, 4), (10, 3), (11, 2), (12, 1), (13, 0)]
    oldest = trace.args(samples[0]["arg_set_id"])["waiting"][0]
    assert (oldest["op"], oldest["waited"]) == (6, 5)


def test_task_filter_uses_that_tasks_span(tmp_path):
    trace = convert(tmp_path, task="steady")
    assert {s["name"] for s in trace.slices("tasks")} == {"steady"}
    got = sorted((s["resource"], s["ts"]) for s in trace.slices("work"))
    want = sorted((r["resource"], r["start"]) for r in db_rows()
                  if overlaps(r["start"], r["finish"], 20, 50))
    assert got == want


def test_counter_is_true_between_spans_of_a_repeated_task(tmp_path):
    # A kernel run twice is two spans. Rows waiting only in the gap are not
    # drawn as slices, but the counter still counts them.
    db = make_db(tmp_path / "run.sqlite",
                 tasks=[("k", 0, 2), ("gap", 2, 12), ("k", 12, 20)])
    trace = convert(tmp_path, db=db, task="k")
    rows = [r for r in waiting() if r["resource"] == "dram_q"]
    samples = trace.counters("dram_q")
    assert samples
    for c in samples:
        assert c["value"] == sum(r["arrival"] <= c["ts"] < r["start"]
                                 for r in rows), c["ts"]


def test_unknown_task_is_an_error(tmp_path):
    with pytest.raises(s2p.ConversionError, match="no task named"):
        s2p.convert(make_db(tmp_path / "run.sqlite"), tmp_path / "o.pftrace",
                    task="missing")


def test_resource_filter_keeps_only_those_resources(tmp_path):
    trace = convert(tmp_path, resources=["link", "dram_q"])
    assert {s["resource"] for s in trace.slices("work")} == {"link", "dram_q"}
    assert trace.counter_resources() == {"dram_q"}


def test_not_a_recording_is_an_error(tmp_path):
    path = tmp_path / "other.sqlite"
    sqlite3.connect(path).executescript("CREATE TABLE t(x);")
    with pytest.raises(s2p.ConversionError, match="ops"):
        s2p.convert(path, tmp_path / "o.pftrace")


def test_recordings_from_before_device_work_read_as_ordinary_work(tmp_path):
    path = tmp_path / "old.sqlite"
    db = sqlite3.connect(path)
    db.executescript(SCHEMA.replace(
        ",\n  finish INTEGER NOT NULL, work INTEGER NOT NULL, parent INTEGER "
        "NOT NULL)", ",\n  finish INTEGER NOT NULL)"))
    db.executemany("INSERT INTO ops VALUES(?,?,?,?,?,?,?,?,?,?)",
                   [row[:10] for row in OPS])
    db.commit()
    db.close()
    trace = convert(tmp_path, name="old", db=path)
    slices = trace.slices("work")
    assert len(slices) == len(OPS)
    for s in slices:
        args = trace.args(s["arg_set_id"])
        assert (args["work"], args["parent"]) == (0, 0)


def test_a_failed_conversion_leaves_no_trace(tmp_path, monkeypatch):
    out = tmp_path / "o.pftrace"
    monkeypatch.setattr(s2p._Writer, "_row_args", lambda *_: 1 / 0)
    with pytest.raises(ZeroDivisionError):
        s2p.convert(make_db(tmp_path / "run.sqlite"), out)
    assert not out.exists()


def test_command_line_writes_a_trace(tmp_path):
    db = make_db(tmp_path / "run.sqlite")
    out = tmp_path / "run.pftrace"
    assert s2p.main([str(db), "-o", str(out), "--resource", "link"]) == 0
    trace = Trace(TraceProcessor(trace=str(out)))
    assert {s["resource"] for s in trace.slices("work")} == {"link"}
