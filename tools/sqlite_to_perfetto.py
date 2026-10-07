# Copyright 2025-2026 Yi-Ping Pan (Cloudlet)
"""Converts an EventStore recording to a Perfetto trace.

Reads the ops, deps, and tasks tables that timeball/event_store.hpp writes and
nothing else, and writes a .pftrace that ui.perfetto.dev opens. An optional
ops.metadata JSON object adds named arguments to each slice:

  tasks             one slice per tasks row
  <resource>
    work, work 2..  one slice per ops row, start to finish
    wait, wait 2..  arrival to start, while at most WAIT_LANES rows wait at once
    queue           otherwise a counter of rows waiting, naming the oldest

Every slice carries its row, its dependencies, and the one that finished
last, so clicking it shows the full state. One cycle is one nanosecond, so the
timestamps the UI shows are cycles.

Rows stream from SQLite to the file. What is held in memory is bounded by the
lanes and the queue at one resource, not by the length of the run.
"""

import argparse
import heapq
import itertools
import json
import math
import os
import sqlite3
import sys
import tempfile
from collections import OrderedDict
from pathlib import Path

from perfetto.protos.perfetto.trace.perfetto_trace_pb2 import (
    CounterDescriptor,
    TrackDescriptor,
    TrackEvent,
)
from perfetto.trace_builder.proto_builder import StreamingTraceProtoBuilder

# More simultaneous waits than this at one resource draw a counter instead.
WAIT_LANES = 2
# Waiting rows named on each counter sample, oldest first.
QUEUE_DETAIL = 16

_SEQUENCE = 1
# String arguments drawn from a small set, so interned rather than repeated.
_INTERNED_VALUES = {"name", "resource"}
_COLUMNS = ("op", "name", "resource", "initiator", "addr", "depth",
            "forwarded", "arrival", "start", "finish", "work", "parent")


class ConversionError(Exception):
    """The input is not a recording, or the options name nothing in it."""


def convert(db_path, out_path, *, cycle_range=None, task=None, resources=None,
            flows=False, wait_slices=False, queue_detail=QUEUE_DETAIL):
    """Writes the recording at db_path as a Perfetto trace at out_path.

    cycle_range (lo, hi) or task keeps the slices that overlap it; resources
    keeps those resources. flows draws an arrow per dependency, wait_slices
    draws every wait as slices, and queue_detail caps the rows named on a
    counter sample (0 names them all).
    """
    _refuse_alias(db_path, out_path)
    db = sqlite3.connect(f"file:{Path(db_path)}?mode=ro", uri=True)
    try:
        columns = _ops_columns(db, db_path)
        spans = _spans(db, cycle_range, task)
        # Written beside the target so the final rename is atomic, and
        # published only once complete: a partial trace would open as if the
        # run ended early, and must never replace a good one.
        fd, tmp = tempfile.mkstemp(dir=Path(out_path).resolve().parent,
                                   prefix=f".{Path(out_path).name}.",
                                   suffix=".tmp")
        try:
            with os.fdopen(fd, "wb") as f:
                _Writer(db, StreamingTraceProtoBuilder(f), columns, spans,
                        resources, flows, wait_slices, queue_detail).write()
            os.chmod(tmp, _output_mode(out_path))
            os.replace(tmp, out_path)
        except BaseException:
            Path(tmp).unlink(missing_ok=True)
            raise
    finally:
        db.close()


def _refuse_alias(db_path, out_path):
    """Rejects an output that is the recording under another name."""
    same = Path(db_path).resolve() == Path(out_path).resolve()
    try:
        same = same or os.path.samefile(db_path, out_path)
    except OSError:
        pass  # the output does not exist yet, or the input is unreadable
    if same:
        raise ConversionError(
            f"output {out_path} is the input recording {db_path}; "
            "choose a different -o path")


def _output_mode(out_path):
    try:
        return os.stat(out_path).st_mode & 0o7777
    except OSError:
        umask = os.umask(0)
        os.umask(umask)
        return 0o666 & ~umask


def _ops_columns(db, db_path):
    """Checks db is a recording; returns SQL selecting each ops column."""
    tables = {r[0] for r in db.execute(
        "SELECT name FROM sqlite_master WHERE type = 'table'")}
    missing = {"ops", "deps", "tasks"} - tables
    if missing:
        raise ConversionError(
            f"{db_path}: not an EventStore recording; missing table(s) "
            f"{', '.join(sorted(missing))}")
    present = {r[1] for r in db.execute("PRAGMA table_info(ops)")}
    # Recordings from before device work had no work or parent; their rows
    # are all ordinary work, which records zeros there.
    missing = set(_COLUMNS) - present - {"work", "parent"}
    if missing:
        raise ConversionError(
            f"{db_path}: ops is missing column(s) "
            f"{', '.join(sorted(missing))}")
    columns = ", ".join(f"o.{c}" if c in present else f"0 AS {c}"
                        for c in _COLUMNS)
    return columns + (", o.metadata" if "metadata" in present
                      else ", NULL AS metadata")


def _spans(db, cycle_range, task):
    """Disjoint, sorted (lo, hi) spans to keep, or None for everything."""
    if task is not None:
        spans = db.execute("SELECT begin_cycle, end_cycle FROM tasks "
                           "WHERE name = ? ORDER BY begin_cycle",
                           (task,)).fetchall()
        if not spans:
            raise ConversionError(f"no task named {task!r}")
    elif cycle_range is not None:
        spans = [cycle_range]
    else:
        return None
    merged = []
    for lo, hi in sorted(spans):
        if merged and lo <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], hi))
        else:
            merged.append((lo, hi))
    return merged


class _Lanes:
    """First-fit lanes, so no lane holds two partially overlapping slices."""

    def __init__(self):
        self.ends = []

    def place(self, begin, end):
        for i, lane_end in enumerate(self.ends):
            if lane_end <= begin:
                self.ends[i] = end
                return i
        self.ends.append(end)
        return len(self.ends) - 1


class _Writer:

    def __init__(self, db, builder, columns, spans, resources, flows,
                 wait_slices, queue_detail):
        self.db = db
        self.builder = builder
        self.columns = columns
        self.spans = spans
        self.resources = resources
        self.flows = flows
        self.wait_slices = wait_slices
        self.queue_detail = queue_detail
        self.next_uuid = 1
        self.groups = {}  # resource -> its track uuid
        self.lanes = {}  # (resource, kind) -> ([lane uuid], _Lanes)
        # Strings repeated on most packets are written once and then named by
        # id: {interned_data field: {string: iid}}.
        self.interned = {"event_names": {}, "debug_annotation_names": {},
                         "debug_annotation_string_values": {}}
        self.packet = None  # being written; interned strings land in it

    # Selection, in SQL so it streams.

    def _overlap(self, begin, end, hull=False):
        """SQL for: [begin, end] overlaps a kept span, or with hull, the
        cycles from the first span to the last. A zero-length slice overlaps
        a span it lies inside, including its start."""
        if self.spans is None:
            return "1"
        if hull:
            lo, hi = self.spans[0][0], self.spans[-1][1]
            return f"({begin} < {hi} AND ({end} > {lo} OR {begin} >= {lo}))"
        # Spans are disjoint and sorted, so only the first to end after begin
        # can overlap.
        return (f"EXISTS (SELECT 1 FROM (SELECT lo, hi FROM temp.spans "
                f"WHERE hi > {begin} ORDER BY hi LIMIT 1) "
                f"WHERE {end} > lo OR {begin} >= lo)")

    def _kept(self, begin, end, hull=False, resources=None):
        """SQL and parameters for: an ops row o is drawn."""
        where = [self._overlap(begin, end, hull)]
        params = []
        for names in (self.resources, resources):
            if names is not None:
                where.append(f"o.resource IN ({', '.join('?' * len(names))})")
                params.extend(names)
        return " AND ".join(where), params

    def _rows(self, begin, end, order, extra="", hull=False, resources=None):
        where, params = self._kept(begin, end, hull, resources)
        if extra:
            where = f"{where} AND {extra}"
        flow_columns = ("fo.ids AS flow_out, fi.ids AS flow_in" if self.flows
                        else "NULL AS flow_out, NULL AS flow_in")
        flow_joins = ("LEFT JOIN temp.flow_out fo ON fo.row = o.rowid "
                      "LEFT JOIN temp.flow_in fi ON fi.row = o.rowid"
                      if self.flows else "")
        sql = (f"SELECT o.rowid, {self.columns}, "
               f"d.deps, d.released_by, {flow_columns} FROM ops o "
               f"LEFT JOIN temp.op_deps d ON d.op = o.op AND o.op != 0 "
               f"{flow_joins} WHERE {where} ORDER BY {order}")
        return self.db.execute(sql, params)

    def _prepare(self):
        db = self.db
        if self.spans is not None:
            db.execute("CREATE TEMP TABLE spans(lo INTEGER, hi INTEGER)")
            db.executemany("INSERT INTO temp.spans VALUES(?, ?)", self.spans)
            db.execute("CREATE INDEX temp.spans_hi ON spans(hi)")
        # An op's name, and when it finished: its last row.
        db.execute("CREATE TEMP TABLE op_info(op INTEGER PRIMARY KEY, "
                   "name TEXT, finish INTEGER)")
        db.execute("INSERT INTO temp.op_info SELECT op, MIN(name), "
                   "MAX(finish) FROM ops WHERE op != 0 GROUP BY op")
        # Each op's dependencies, and the one that finished last.
        db.execute("CREATE TEMP TABLE op_deps(op INTEGER PRIMARY KEY, "
                   "deps TEXT, released_by INTEGER)")
        db.execute("""
            INSERT INTO temp.op_deps
            SELECT op, json_group_array(json_array(on_op, name, finish)),
                   MAX(CASE WHEN rank = 1 THEN on_op END)
            FROM (SELECT d.op, d.depends_on AS on_op, i.name, i.finish,
                         ROW_NUMBER() OVER (PARTITION BY d.op
                           ORDER BY i.finish DESC, d.depends_on) AS rank
                  FROM deps d LEFT JOIN temp.op_info i
                  ON i.op = d.depends_on)
            GROUP BY op""")
        if self.flows:
            self._prepare_flows()

    def _prepare_flows(self):
        # An arrow leaves the row where the dependency finished and enters
        # the row where the dependent arrived, when both are drawn.
        db = self.db
        db.execute("""
            CREATE TEMP TABLE ends(op INTEGER PRIMARY KEY, last_row INTEGER,
                                   first_row INTEGER)""")
        # Keyed by op: every dependency looks up both of its ends here.
        db.execute("""
            INSERT INTO temp.ends
            SELECT op, MAX(CASE WHEN last = 1 THEN row END) AS last_row,
                       MAX(CASE WHEN first = 1 THEN row END) AS first_row
            FROM (SELECT op, rowid AS row,
                    ROW_NUMBER() OVER (PARTITION BY op
                      ORDER BY finish DESC, rowid DESC) AS last,
                    ROW_NUMBER() OVER (PARTITION BY op
                      ORDER BY arrival, rowid) AS first
                  FROM ops WHERE op != 0)
            GROUP BY op""")
        drawn, params = self._kept("o.start", "o.finish")
        db.execute(f"""
            CREATE TEMP TABLE flows AS
            SELECT d.rowid AS id, s.last_row AS src, t.first_row AS dst
            FROM deps d JOIN temp.ends s ON s.op = d.depends_on
            JOIN temp.ends t ON t.op = d.op
            WHERE EXISTS (SELECT 1 FROM ops o WHERE o.rowid = s.last_row
                          AND {drawn})
            AND EXISTS (SELECT 1 FROM ops o WHERE o.rowid = t.first_row
                        AND {drawn})""", params * 2)
        db.execute("CREATE TEMP TABLE flow_out AS SELECT src AS row, "
                   "group_concat(id) AS ids FROM temp.flows GROUP BY src")
        db.execute("CREATE INDEX temp.flow_out_row ON flow_out(row)")
        db.execute("CREATE TEMP TABLE flow_in AS SELECT dst AS row, "
                   "group_concat(id) AS ids FROM temp.flows GROUP BY dst")
        db.execute("CREATE INDEX temp.flow_in_row ON flow_in(row)")

    # Output.

    def write(self):
        self.db.row_factory = sqlite3.Row
        self._prepare()
        self._write_tasks()
        crowded = self._crowded()
        for row in self._rows("o.start", "o.finish",
                              "o.resource, o.start, o.finish, o.rowid"):
            self._slice(row, "work", row["start"], row["finish"], row["name"])
        for row in self._waits():
            if row["resource"] not in crowded:
                self._slice(row, "wait", row["arrival"], row["start"],
                            f"wait {row['name']}")
        # A counter is a value at every cycle, so between the spans of a
        # repeated task it still counts what waits there.
        if crowded:
            for resource, rows in itertools.groupby(
                    self._waits(hull=True, resources=sorted(crowded)),
                    key=lambda row: row["resource"]):
                self._queue(resource, rows)

    def _waits(self, hull=False, resources=None):
        return self._rows("o.arrival", "o.start",
                          "o.resource, o.arrival, o.rowid",
                          "o.start > o.arrival", hull, resources)

    def _crowded(self):
        """Resources where more than WAIT_LANES rows wait at once."""
        if self.wait_slices:
            return set()
        crowded = set()
        resource, starts = None, []
        for row in self._waits():
            if row["resource"] != resource:
                resource, starts = row["resource"], []
            while starts and starts[0] <= row["arrival"]:
                heapq.heappop(starts)
            heapq.heappush(starts, row["start"])
            if len(starts) > WAIT_LANES:
                crowded.add(resource)
        return crowded

    def _queue(self, resource, rows):
        """A counter of rows waiting at resource, naming the oldest."""
        track = self._track("queue", self._group(resource), 10_000,
                            counter=True,
                            description="Rows that have arrived and not "
                                        "started; click a sample for the "
                                        "oldest of them")
        lo, hi = -math.inf, math.inf
        if self.spans is not None:
            lo, hi = self.spans[0][0], self.spans[-1][1]
        for t, queue in _queue_states(rows, lo):
            if t >= hi:
                break
            self._sample(track, t, queue)

    def _sample(self, track, t, queue):
        packet, event = self._event(t, track, TrackEvent.TYPE_COUNTER)
        event.counter_value = len(queue)
        listed = list(itertools.islice(queue.values(),
                                       self.queue_detail or None))
        self._annotate(event, "queued", len(queue))
        self._annotate(event, "omitted", len(queue) - len(listed))
        if listed:
            waiting = self._array(event, "waiting")
            for row in listed:
                self._dict(waiting.array_values.add(), (
                    ("row", row["rowid"]), ("op", row["op"]),
                    ("name", row["name"]), ("initiator", row["initiator"]),
                    ("arrival", row["arrival"]), ("start", row["start"]),
                    ("waited", t - row["arrival"]),
                    ("wait_cycles", row["start"] - row["arrival"])))
        self.builder.write_packet(packet)

    def _packet(self):
        packet = self.builder.create_packet()
        packet.trusted_packet_sequence_id = _SEQUENCE
        packet.sequence_flags = (
            packet.SEQ_NEEDS_INCREMENTAL_STATE if self.packet is not None
            else packet.SEQ_INCREMENTAL_STATE_CLEARED)
        self.packet = packet
        return packet

    def _iid(self, table, text):
        ids = self.interned[table]
        if text not in ids:
            ids[text] = len(ids) + 1
            entry = getattr(self.packet.interned_data, table).add()
            entry.iid = ids[text]
            if table == "debug_annotation_string_values":
                entry.str = text.encode()
            else:
                entry.name = text
        return ids[text]

    def _arg(self, annotation, name, value):
        annotation.name_iid = self._iid("debug_annotation_names", name)
        if isinstance(value, bool):
            annotation.bool_value = value
        elif isinstance(value, int):
            annotation.int_value = value
        elif name in _INTERNED_VALUES:
            annotation.string_value_iid = self._iid(
                "debug_annotation_string_values", value)
        else:
            annotation.string_value = str(value)

    def _annotate(self, event, name, value):
        self._arg(event.debug_annotations.add(), name, value)

    def _dict(self, annotation, entries):
        for name, value in entries:
            if value is not None:
                self._arg(annotation.dict_entries.add(), name, value)

    def _array(self, event, name):
        annotation = event.debug_annotations.add()
        annotation.name_iid = self._iid("debug_annotation_names", name)
        return annotation

    def _row_args(self, event, row):
        """The ops row behind a slice, its dependencies, and what released
        it."""
        self._annotate(event, "row", row["rowid"])
        for column in _COLUMNS:
            value = row[column]
            if column == "addr":
                value = hex(value)
            elif column == "forwarded":
                value = bool(value)
            self._annotate(event, column, value)
        self._annotate(event, "wait_cycles", row["start"] - row["arrival"])
        self._annotate(event, "work_cycles", row["finish"] - row["start"])
        if row["metadata"] is not None:
            try:
                metadata = json.loads(row["metadata"])
            except json.JSONDecodeError as error:
                raise ConversionError(
                    f"ops row {row['rowid']}: invalid metadata JSON") from error
            if not isinstance(metadata, dict):
                raise ConversionError(f"ops row {row['rowid']}: invalid metadata object")
            for name, value in metadata.items():
                self._annotate(event, name, value)
        if row["deps"] is None:
            return
        depends_on = self._array(event, "depends_on")
        for op, name, finish in json.loads(row["deps"]):
            self._dict(depends_on.array_values.add(),
                       (("op", op), ("name", name), ("finish", finish)))
        self._annotate(event, "released_by", row["released_by"])

    def _track(self, name, parent=None, rank=None, counter=False,
               description=None, explicit_order=False):
        uuid = self.next_uuid
        self.next_uuid += 1
        packet = self._packet()
        track = packet.track_descriptor
        track.uuid = uuid
        track.name = name
        track.sibling_merge_behavior = (
            TrackDescriptor.SIBLING_MERGE_BEHAVIOR_NONE)
        if parent is not None:
            track.parent_uuid = parent
        if rank is not None:
            track.sibling_order_rank = rank
        if counter:
            track.counter.CopyFrom(CounterDescriptor())
        if description:
            track.description = description
        if explicit_order:
            track.child_ordering = TrackDescriptor.EXPLICIT
        self.builder.write_packet(packet)
        return uuid

    def _group(self, resource):
        if resource not in self.groups:
            # Ordered by rank: work lanes above the waits.
            self.groups[resource] = self._track(resource,
                                                explicit_order=True)
        return self.groups[resource]

    def _lane(self, resource, kind, begin, end):
        key = (resource, kind)
        if key not in self.lanes:
            self.lanes[key] = ([], _Lanes())
        uuids, lanes = self.lanes[key]
        index = lanes.place(begin, end)
        if index == len(uuids):
            name = kind if index == 0 else f"{kind} {index + 1}"
            rank = index if kind == "work" else 10_000 + index
            uuids.append(self._track(name, self._group(resource), rank))
        return uuids[index]

    def _event(self, ts, track, kind, name=None):
        packet = self._packet()
        packet.timestamp = ts
        event = packet.track_event
        event.type = kind
        event.track_uuid = track
        if name is not None:
            event.name_iid = self._iid("event_names", name)
        return packet, event

    def _slice(self, row, kind, begin, end, name):
        track = self._lane(row["resource"], kind, begin, end)
        packet, event = self._event(begin, track,
                                    TrackEvent.TYPE_SLICE_BEGIN, name)
        self._row_args(event, row)
        if kind == "work":
            if row["flow_out"]:
                event.flow_ids.extend(
                    int(i) for i in row["flow_out"].split(","))
            if row["flow_in"]:
                event.terminating_flow_ids.extend(
                    int(i) for i in row["flow_in"].split(","))
        self.builder.write_packet(packet)
        packet, _ = self._event(end, track, TrackEvent.TYPE_SLICE_END)
        self.builder.write_packet(packet)

    def _write_tasks(self):
        rows = self.db.execute(
            f"SELECT name, begin_cycle, end_cycle FROM tasks "
            f"WHERE {self._overlap('begin_cycle', 'end_cycle')} "
            f"ORDER BY begin_cycle, end_cycle, rowid")
        lanes, uuids = _Lanes(), []
        for name, begin, end in rows:
            index = lanes.place(begin, end)
            if index == len(uuids):
                uuids.append(self._track(
                    "tasks" if index == 0 else f"tasks {index + 1}", rank=-1,
                    description="Named spans of cycles from the tasks table"))
            packet, event = self._event(begin, uuids[index],
                                        TrackEvent.TYPE_SLICE_BEGIN, name)
            self._annotate(event, "name", name)
            self._annotate(event, "begin_cycle", begin)
            self._annotate(event, "end_cycle", end)
            self.builder.write_packet(packet)
            packet, _ = self._event(end, uuids[index],
                                    TrackEvent.TYPE_SLICE_END)
            self.builder.write_packet(packet)


def _queue_states(rows, lo):
    """(t, queue) at each cycle from lo on where the queue changes, and at lo
    itself if rows were already waiting. queue maps rowid to row, oldest
    arrival first, and is only valid until the next step. rows come in
    arrival order."""
    queue = OrderedDict()
    starts = []
    rows = iter(rows)
    pending = next(rows, None)
    before = False  # a change happened before lo
    while pending is not None or starts:
        t = min(pending["arrival"] if pending is not None else math.inf,
                starts[0][0] if starts else math.inf)
        if before and t > lo:
            yield lo, queue
            before = False
        while starts and starts[0][0] == t:
            del queue[heapq.heappop(starts)[1]]
        while pending is not None and pending["arrival"] == t:
            queue[pending["rowid"]] = pending
            heapq.heappush(starts, (pending["start"], pending["rowid"]))
            pending = next(rows, None)
        if t >= lo:
            yield t, queue
        else:
            before = True


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Convert an EventStore SQLite recording to a Perfetto "
                    "trace for ui.perfetto.dev.")
    parser.add_argument("db", help="recording written by EventStore")
    parser.add_argument("-o", "--output",
                        help="trace to write (default: DB with .pftrace)")
    span = parser.add_mutually_exclusive_group()
    span.add_argument("--task", help="keep the cycles of the task(s) with "
                                     "this name")
    span.add_argument("--from", dest="lo", type=int,
                      help="keep slices overlapping cycles from here")
    parser.add_argument("--to", dest="hi", type=int,
                        help="keep slices overlapping cycles before here")
    parser.add_argument("--resource", action="append", dest="resources",
                        help="keep this resource; repeatable")
    parser.add_argument("--flows", action="store_true",
                        help="draw an arrow per dependency")
    parser.add_argument("--wait-slices", action="store_true",
                        help="draw every wait as slices, never a counter")
    parser.add_argument("--queue-detail", type=int, default=QUEUE_DETAIL,
                        help="waiting rows named per counter sample, oldest "
                             "first; 0 names all (default: %(default)s)")
    args = parser.parse_args(argv)
    if args.task is not None and args.hi is not None:
        parser.error("--to cannot be combined with --task")

    cycle_range = None
    if args.lo is not None or args.hi is not None:
        cycle_range = (args.lo if args.lo is not None else 0,
                       args.hi if args.hi is not None else 2**63 - 1)
    output = args.output or str(Path(args.db).with_suffix(".pftrace"))
    try:
        convert(args.db, output, cycle_range=cycle_range, task=args.task,
                resources=args.resources, flows=args.flows,
                wait_slices=args.wait_slices, queue_detail=args.queue_detail)
    except (ConversionError, sqlite3.Error, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
