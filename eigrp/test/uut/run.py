#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Donnie V. Savage
"""Declarative portable EIGRP UUT topology/scenario runner."""
from __future__ import annotations
import argparse, collections, ipaddress, os, queue, socket, struct, subprocess, sys, tempfile, threading, time
from pathlib import Path
import yaml

TOPOLOGY_SCHEMA="eigrp-uut-topology/v1"; TEST_SCHEMA="eigrp-uut-test/v1"
HDR=struct.Struct("!BBBBIHHHH16s16s"); REGISTER=1; PACKET=2; MULTICAST=1
OPCODES={1:"update",2:"request",3:"query",4:"reply",5:"hello",6:"ipxsap",7:"probe",8:"ack",10:"sia-query",11:"sia-reply"}
ROOT=Path(__file__).resolve().parents[3]; NODE=Path(__file__).with_name("eigrp-uut-node")
sys.path.insert(0,str(ROOT/"unix"/"test"))
from eigrp_wire import Broker

class SpecError(Exception): pass

def load_yaml(path):
    with open(path,encoding="utf-8") as f: v=yaml.safe_load(f)
    if not isinstance(v,dict): raise SpecError(f"{path}: document must be a mapping")
    return v

def check_keys(obj, allowed, where):
    extra=set(obj)-set(allowed)
    if extra: raise SpecError(f"{where}: unsupported keys: {', '.join(sorted(extra))}")

def validate_topology(t):
    check_keys(t,{"schema","routers","segments"},"topology")
    if t.get("schema")!=TOPOLOGY_SCHEMA: raise SpecError(f"topology schema must be {TOPOLOGY_SCHEMA}")
    if not isinstance(t.get("routers"),dict) or not t["routers"]: raise SpecError("topology.routers must be a non-empty mapping")
    if not isinstance(t.get("segments",{}),dict): raise SpecError("topology.segments must be a mapping")
    for rn,r in t["routers"].items():
        if not isinstance(r,dict): raise SpecError(f"router {rn} must be a mapping")
        check_keys(r,{"asn","router_id","active_time","interfaces","attached_networks","source_networks","summary_metrics","redistribute"},f"router {rn}")
        if not 1<=int(r.get("asn",0))<=65535: raise SpecError(f"router {rn}: invalid asn")
        ipaddress.IPv4Address(r.get("router_id"))
        if "active_time" in r and not 1 <= int(r["active_time"]) <= 65535: raise SpecError(f"router {rn}: active_time must be 1..65535 seconds")
        if not isinstance(r.get("interfaces",{}),dict): raise SpecError(f"router {rn}.interfaces must be a mapping")
        for name,i in r.get("interfaces",{}).items():
            check_keys(i,{"segment","ipv4","ipv6","mtu","bandwidth","delay","up","summaries","authentication"},f"{rn}.{name}")
            auth=i.get("authentication")
            if auth is not None:
                if not isinstance(auth,dict): raise SpecError(f"{rn}.{name}.authentication must be a mapping")
                check_keys(auth,{"mode","keychain","key"},f"{rn}.{name}.authentication")
                if auth.get("mode")!="md5": raise SpecError(f"{rn}.{name}: only MD5 UUT authentication is supported")
                for field in ("keychain","key"):
                    value=auth.get(field)
                    if not isinstance(value,str) or not value or any(c.isspace() for c in value):
                        raise SpecError(f"{rn}.{name}.authentication.{field} must be a non-empty string without whitespace")
                if len(auth["keychain"])>=64 or len(auth["key"])>32:
                    raise SpecError(f"{rn}.{name}: UUT authentication keychain/key too long")
            summaries=i.get("summaries",[])
            if not isinstance(summaries,list): raise SpecError(f"{rn}.{name}.summaries must be a list")
            for summary in summaries: ipaddress.ip_network(summary,strict=False)
            if i.get("segment") not in t.get("segments",{}): raise SpecError(f"{rn}.{name}: unknown segment {i.get('segment')}")
            for key in ("ipv4","ipv6"):
                vals=i.get(key,[]); vals=[vals] if isinstance(vals,str) else vals
                if not isinstance(vals,list): raise SpecError(f"{rn}.{name}.{key} must be string/list")
                for p in vals: ipaddress.ip_interface(p)
        sources = r.get("redistribute", [])
        if not isinstance(sources, list) or any(v != "static" for v in sources):
            raise SpecError(f"router {rn}: redistribute must be a list containing only 'static'")
        for sm in r.get("summary_metrics",[]):
            check_keys(sm,{"prefix","bandwidth","delay","reliability","load","mtu"},f"{rn}.summary_metrics")
            ipaddress.ip_network(sm["prefix"],strict=False)
        # Attached destinations create independent host interfaces.  Reject
        # collisions now instead of allowing one node's setup to fail later.
        attached_names=[n.get("interface",f"attached{idx}")
                        for idx,n in enumerate(r.get("attached_networks",[]),1)]
        if len(set(attached_names)) != len(attached_names) or set(attached_names) & set(r["interfaces"]):
            raise SpecError(f"router {rn}: attached interface names must be unique and distinct from transit interfaces")
        for key in ("attached_networks","source_networks"):
            vals=r.get(key,[])
            if not isinstance(vals,list): raise SpecError(f"{rn}.{key} must be a list")
            for n in vals:
                if not isinstance(n,dict): raise SpecError(f"{rn}.{key} entries must be mappings")
                check_keys(n,{"prefix","interface","protocol","metric","bandwidth","delay","mtu"},f"{rn}.{key}")
                address=ipaddress.ip_interface(n["prefix"])
                if key=="attached_networks" and address.version==6 and (
                        address.ip.is_link_local or address.ip.is_multicast or address.ip.is_unspecified or address.ip.is_loopback):
                    raise SpecError(f"{rn}.{key}: IPv6 attached destinations must be routable unicast, not link-local/multicast/unspecified")
    return t

def topology_afi(t):
    families=set()
    for r in t.get("routers",{}).values():
        for i in r.get("interfaces",{}).values():
            for fam in ("ipv4","ipv6"):
                vals=i.get(fam,[])
                vals=[vals] if isinstance(vals,str) else vals
                families.update(ipaddress.ip_interface(p).version for p in vals)
        for key in ("attached_networks","source_networks"):
            families.update(ipaddress.ip_interface(n["prefix"]).version for n in r.get(key,[]))
    if len(families)!=1:
        raise SpecError("UUT topology requires exactly one address family")
    return next(iter(families))

ACCOUNT_ASSERTION_KEYS = {"uut", "neighbor", "interface", "status", "restart_count", "prefix_count", "restart_active", "reset_active", "timeout", "observe"}

def validate_accounting_assertion(e, where):
    if not isinstance(e, dict):
        raise SpecError(f"{where}: accounting assertion must be a mapping")
    check_keys(e, ACCOUNT_ASSERTION_KEYS, where)
    if not isinstance(e.get("uut"), str) or not e["uut"]:
        raise SpecError(f"{where}: uut is required")
    try:
        ipaddress.ip_address(e["neighbor"])
    except (ValueError, KeyError, TypeError) as exc:
        raise SpecError(f"{where}: valid neighbor address is required") from exc
    if e.get("status", "A") not in ("A", "P", "D"):
        raise SpecError(f"{where}: status must be A, P or D")
    for key in ("restart_count", "prefix_count"):
        if key in e and (not isinstance(e[key], int) or isinstance(e[key], bool) or e[key] < 0):
            raise SpecError(f"{where}: {key} must be non-negative integer")
    for key in ("restart_active", "reset_active"):
        if key in e and not isinstance(e[key], bool):
            raise SpecError(f"{where}: {key} must be boolean")

NEIGHBOR_ASSERTION_KEYS = {"uut", "neighbor", "interface", "state", "timeout", "observe",
                           "reliable_q", "rto", "srtt_valid", "srtt", "retransmissions"}

def validate_neighbor_assertion(e, where):
    """Do not let an unsupported neighbor filter silently become a passing test."""
    if not isinstance(e, dict):
        raise SpecError(f"{where}: neighbor assertion must be a mapping")
    check_keys(e, NEIGHBOR_ASSERTION_KEYS, where)
    if not isinstance(e.get("uut"), str) or not e["uut"]:
        raise SpecError(f"{where}: uut is required")
    if not isinstance(e.get("neighbor"), str) or not e["neighbor"]:
        raise SpecError(f"{where}: neighbor address is required")
    try:
        ipaddress.ip_address(e["neighbor"])
    except ValueError as exc:
        raise SpecError(f"{where}: invalid neighbor address {e['neighbor']!r}") from exc
    state = e.get("state", "up")
    if not isinstance(state, str) or state.lower() not in ("up", "present", "down", "limit-suspended", "limit-locked"):
        raise SpecError(f"{where}: state must be up, present, down, limit-suspended, or limit-locked")
    if state.lower() == "down" and any(k in e for k in
                                        ("reliable_q", "rto", "srtt_valid", "srtt", "retransmissions")):
        raise SpecError(f"{where}: cannot check live neighbor counters when state is down")

ROUTE_ASSERTION_KEYS = {"uut", "prefix", "table", "present", "count", "min_count", "max_count", "timeout",
                        "next_hop", "metric", "distance", "active", "fd", "successors", "interface",
                        "connected", "successor", "feasible_successor", "rd", "protocol"}
ROUTE_FIELDS = {
    "rib": {"next_hop", "metric", "distance"},
    "source": {"protocol", "metric"},
    "path": {"next_hop", "interface", "connected", "successor", "feasible_successor", "distance", "rd"},
    "topology": {"active", "fd", "successors"},
}
PACKET_FILTERS = {"uut", "source_uut", "destination_uut", "interface", "source_interface",
                  "destination_interface", "segment", "afi", "opcode", "multicast", "sequence",
                  "auth_sequence", "ack", "cr", "event", "prefix"}
PACKET_ASSERTION_KEYS = PACKET_FILTERS | {"since", "min", "max", "timeout", "group_by",
                                        "min_groups", "max_groups", "max_per_group"}
PACKET_EVENTS = {"sent", "delivered", "delayed", "duplicated", "dropped", "fault", "disconnected", "send-error"}
PACKET_GROUP_KEYS = {"source_uut", "destination_uut", "source_interface", "destination_interface",
                     "segment", "afi", "opcode", "multicast", "sequence", "auth_sequence", "ack", "cr", "event"}
STEP_FIELDS = {
    "link-up": {"uut", "interface"}, "link-down": {"uut", "interface"},
    "peer-limit": {"uut", "interface", "neighbor", "operation"},
    "peer-limit-config": {"uut", "maximum", "warning_only"},
    "redist-limit-config": {"uut", "maximum", "warning_only"},
    "process-limit-config": {"uut", "maximum", "warning_only", "dampened"},
    "inbound-filter-config": {"uut", "prefix", "enabled"},
    "process-limit-clear": {"uut"}, "process-limit-expire": {"uut"}, "process-limit-reset-expire": {"uut"}, "process-limit-stale": {"uut"}, "process-limit-age": {"uut"}, "validate-process-limit": {"uut", "state", "restarts", "count", "restart_active", "reset_active", "penalty", "penalty_min", "penalty_max", "penalty_after_age", "timeout"},
    "redist-limit-clear": {"uut"}, "redist-limit-expire": {"uut"}, "validate-redist-limit": {"uut", "state", "restarts", "count", "restart_active", "reset_active", "penalty", "timeout"},
    "source-route-add": {"uut", "prefix", "metric"}, "source-route-remove": {"uut", "prefix"},
    "redistribute-add": {"uut", "source"}, "redistribute-remove": {"uut", "source"},
    "wait": {"seconds"}, "fault-add": {"name", "action", "source_uut", "destination_uut",
               "source_interface", "destination_interface", "segment", "afi", "multicast",
               "opcode", "prefix", "sequence", "ack", "cr", "delay_ms", "copies", "count"},
    "fault-remove": {"name"}, "validate-fault": {"name", "hits", "remaining", "timeout"},
    "validate-event": {"uut", "contains", "after", "min", "max", "timeout"},
    "validate-log": {"uut", "contains", "min", "timeout"},
    "validate-retransmission": {"source_uut", "destination_uut", "opcode", "since", "timeout"},
    "validate-retry": {"source_uut", "opcode", "since", "timeout"},
}

def _count(value, name, minimum=0):
    if type(value) is not int or value < minimum:
        raise SpecError(f"{name}: must be an integer >= {minimum}")

def validate_route_assertion(e, where):
    if not isinstance(e, dict): raise SpecError(f"{where}: route assertion must be a mapping")
    check_keys(e, ROUTE_ASSERTION_KEYS, where)
    if not isinstance(e.get("uut"), str) or not e["uut"]: raise SpecError(f"{where}: uut is required")
    try: ipaddress.ip_network(e["prefix"], strict=False)
    except (KeyError, TypeError, ValueError) as exc: raise SpecError(f"{where}: valid prefix is required") from exc
    table = e.get("table", "rib")
    if not isinstance(table,str) or table not in ROUTE_FIELDS: raise SpecError(f"{where}: unsupported route table {table!r}")
    extraneous = set(e) - ROUTE_FIELDS[table] - {"uut", "prefix", "table", "present", "count", "min_count", "max_count", "timeout"}
    if extraneous: raise SpecError(f"{where}: invalid {table} filters: {', '.join(sorted(extraneous))}")
    for key in ("count", "min_count", "max_count"):
        if key in e: _count(e[key], f"{where}.{key}")
    if "count" in e and ("min_count" in e or "max_count" in e):
        raise SpecError(f"{where}: count cannot be combined with min_count/max_count")
    if e.get("present") is False and any(k in e for k in ("count", "min_count", "max_count")):
        raise SpecError(f"{where}: absence cannot specify row counts")
    if "present" in e and type(e["present"]) is not bool: raise SpecError(f"{where}: present must be boolean")

def validate_packet_assertion(e, where, marks=None):
    if not isinstance(e, dict): raise SpecError(f"{where}: packet assertion must be a mapping")
    check_keys(e, PACKET_ASSERTION_KEYS, where)
    if "event" not in e: raise SpecError(f"{where}: event is required (sent, delivered, dropped, or any for negative checks)")
    event=e["event"]
    if not isinstance(event,str) or event not in PACKET_EVENTS | {"any"}: raise SpecError(f"{where}: unsupported packet event {event!r}")
    for key in ("min", "max", "min_groups", "max_groups", "max_per_group"):
        if key in e: _count(e[key], f"{where}.{key}")
    if "min" in e and "max" in e and e["min"]>e["max"]: raise SpecError(f"{where}: min exceeds max")
    if e.get("max") == 0 and e.get("min",1) != 0:
        raise SpecError(f"{where}: zero-packet checks require min: 0, max: 0")
    if event == "any" and not (e.get("max") == 0 and e.get("min",0)==0):
        raise SpecError(f"{where}: event any is only permitted for zero-packet assertions")
    if e.get("min",1)>0 and event in {"fault", "send-error", "disconnected"} and "destination_uut" not in e:
        raise SpecError(f"{where}: non-delivery event requires destination_uut")
    if "group_by" in e:
        groups = [e["group_by"]] if isinstance(e["group_by"],str) else e["group_by"]
        if not isinstance(groups,list) or not groups or any(not isinstance(g,str) or g not in PACKET_GROUP_KEYS for g in groups):
            raise SpecError(f"{where}: group_by must contain supported journal fields")
    elif any(k in e for k in ("min_groups", "max_groups", "max_per_group")):
        raise SpecError(f"{where}: group counts require group_by")
    if "since" in e:
        if not isinstance(e["since"], str) or not e["since"]:
            raise SpecError(f"{where}: since must be a named packet mark")
        if marks is not None and e["since"] not in marks:
            raise SpecError(f"{where}: unknown packet mark {e['since']!r}")
    if "prefix" in e:
        try: ipaddress.ip_network(e["prefix"],strict=False)
        except (TypeError,ValueError) as exc: raise SpecError(f"{where}: invalid packet prefix") from exc

def validate_scenario(s):
    check_keys(s,{"schema","name","topology","steps"},"scenario")
    if s.get("schema")!=TEST_SCHEMA: raise SpecError(f"scenario schema must be {TEST_SCHEMA}")
    if not isinstance(s.get("topology"),str) or not s["topology"]: raise SpecError("scenario.topology must name its topology YAML")
    if not isinstance(s.get("steps"),list) or not s["steps"]: raise SpecError("scenario.steps must be a non-empty list")
    allowed={"start","stop","link-up","link-down","links-up","links-down","wait","packet-mark","fault-add","fault-remove","validate-fault","validate-neighbor","validate-accounting","validate-route","validate-packet","validate-retransmission","validate-retry","validate-convergence","validate-event","validate-log","peer-limit","peer-limit-config","redist-limit-config","process-limit-config","inbound-filter-config","process-limit-clear","process-limit-expire","process-limit-reset-expire","process-limit-stale","process-limit-age","validate-process-limit","source-route-add","source-route-remove","redistribute-add","redistribute-remove","redist-limit-clear","redist-limit-expire","validate-redist-limit","checkpoint"}
    marks=set()
    for idx,step in enumerate(s["steps"],1):
        if not isinstance(step,dict) or len(step)!=1: raise SpecError(f"step {idx}: exactly one operation required")
        op=next(iter(step))
        if op not in allowed: raise SpecError(f"step {idx}: unsupported operation {op}")
        arg=step[op]; where=f"step {idx} {op}"
        if op=="packet-mark":
            if not isinstance(arg,str) or not arg: raise SpecError(f"{where}: mark must be a non-empty string")
            if arg in marks: raise SpecError(f"{where}: duplicate mark {arg!r}")
            marks.add(arg)
        elif op=="validate-neighbor": validate_neighbor_assertion(arg,where)
        elif op=="validate-accounting": validate_accounting_assertion(arg,where)
        elif op=="validate-route": validate_route_assertion(arg,where)
        elif op=="validate-packet": validate_packet_assertion(arg,where,marks)
        elif op=="validate-convergence":
            if not isinstance(arg,dict): raise SpecError(f"{where}: must be a mapping")
            check_keys(arg,{"timeout","expect","stable_for"},where)
            if "stable_for" in arg and (type(arg["stable_for"]) not in (float,int) or arg["stable_for"] < 0):
                raise SpecError(f"{where}: stable_for must be nonnegative seconds")
            if not isinstance(arg.get("expect"),list) or not arg["expect"]: raise SpecError(f"{where}: expect must be non-empty")
            for n,check in enumerate(arg["expect"],1):
                if not isinstance(check,dict) or len(check)!=1: raise SpecError(f"{where} expectation {n}: exactly one check required")
                kind,value=next(iter(check.items()))
                if kind=="neighbor": validate_neighbor_assertion(value,f"{where} expectation {n}")
                elif kind=="route": validate_route_assertion(value,f"{where} expectation {n}")
                elif kind=="packet": validate_packet_assertion(value,f"{where} expectation {n}",marks)
                else: raise SpecError(f"{where} expectation {n}: unsupported {kind!r}")
        elif op in ("links-up","links-down"):
            if isinstance(arg,dict):
                check_keys(arg,{"links"},where)
                links=arg.get("links")
            else: links=arg
            if not isinstance(links,list) or not links: raise SpecError(f"{where}: links must be non-empty")
            for item in links:
                if not isinstance(item,dict): raise SpecError(f"{where}: each link must be a mapping")
                check_keys(item,{"uut","interface"},where)
                if not all(isinstance(item.get(k),str) and item[k] for k in ("uut","interface")):
                    raise SpecError(f"{where}: each link needs uut and interface")
        elif op in ("start","stop"):
            if arg is None or arg=="all" or isinstance(arg,str): continue
            if not isinstance(arg,dict): raise SpecError(f"{where}: invalid node selector")
            check_keys(arg,{"uuts"},where)
            if not isinstance(arg.get("uuts"),list) or not arg["uuts"] or any(not isinstance(v,str) or not v for v in arg["uuts"]):
                raise SpecError(f"{where}: uuts must be a non-empty list of names")
        elif op=="checkpoint":
            if not isinstance(arg,str) or not arg.strip():
                raise SpecError(f"{where}: checkpoint requires a non-empty label")
        elif op=="wait":
            if isinstance(arg,dict): check_keys(arg,{"seconds"},where)
            elif not isinstance(arg,(int,float)) or isinstance(arg,bool): raise SpecError(f"{where}: invalid wait")
        elif op=="fault-remove" and isinstance(arg,str):
            if not arg: raise SpecError(f"{where}: fault name required")
        elif op in STEP_FIELDS:
            if not isinstance(arg,dict): raise SpecError(f"{where}: must be a mapping")
            check_keys(arg,STEP_FIELDS[op],where)
            needed={"link-up":("uut","interface"),"link-down":("uut","interface"),
                    "peer-limit":("uut","interface","neighbor","operation"),
                    "peer-limit-config":("uut","maximum"),
                    "redist-limit-config":("uut","maximum"),
                    "process-limit-config":("uut","maximum"),
                    "inbound-filter-config":("uut","prefix"),
                    "process-limit-clear":("uut",),"process-limit-expire":("uut",),"process-limit-reset-expire":("uut",),"process-limit-stale":("uut",),"process-limit-age":("uut",),"validate-process-limit":("uut","state"),
                    "redist-limit-clear":("uut",),"redist-limit-expire":("uut",),"validate-redist-limit":("uut","state"),
                    "source-route-add":("uut","prefix"),"source-route-remove":("uut","prefix"),
                    "redistribute-add":("uut","source"),"redistribute-remove":("uut","source"),
                    "fault-add":("action",),"fault-remove":("name",),"validate-fault":("name",),
                    "validate-event":("uut","contains"),
                    "validate-log":("uut","contains")}.get(op,())
            if any(k not in arg for k in needed): raise SpecError(f"{where}: missing required fields {needed}")
            if op == "validate-log":
                if not isinstance(arg["contains"], str) or not arg["contains"]:
                    raise SpecError(f"{where}: contains must be a nonempty string")
                if "min" in arg: _count(arg["min"], f"{where}.min", 1)
            if op in ("peer-limit-config", "redist-limit-config", "process-limit-config"):
                _count(arg["maximum"], f"{where}.maximum")
                if "warning_only" in arg and type(arg["warning_only"]) is not bool:
                    raise SpecError(f"{where}: warning_only must be boolean")
            if op == "inbound-filter-config":
                try: ipaddress.ip_network(arg["prefix"], strict=False)
                except (ValueError, TypeError) as exc:
                    raise SpecError(f"{where}: invalid prefix") from exc
                if "enabled" in arg and type(arg["enabled"]) is not bool:
                    raise SpecError(f"{where}: enabled must be boolean")
            if op == "validate-process-limit":
                for field in ("penalty", "penalty_min", "penalty_max"):
                    if field in arg: _count(arg[field], f"{where}.{field}")
                if arg.get("penalty_min",0) > arg.get("penalty_max",4294967295):
                    raise SpecError(f"{where}: penalty bounds are inverted")
                if "penalty_after_age" in arg:
                    if type(arg["penalty_after_age"]) is not bool:
                        raise SpecError(f"{where}: penalty_after_age must be boolean")
                    if any(k in arg for k in ("penalty", "penalty_min", "penalty_max")):
                        raise SpecError(f"{where}: penalty_after_age conflicts with fixed penalty bounds")
            if op == "process-limit-config" and "dampened" in arg and type(arg["dampened"]) is not bool:
                raise SpecError(f"{where}: dampened must be boolean")
            if op == "peer-limit":
                if arg["operation"] not in ("suspend", "lock", "release", "delete", "violate", "expire"):
                    raise SpecError(f"{where}: unsupported operation")
                try: ipaddress.ip_address(arg["neighbor"])
                except ValueError as exc: raise SpecError(f"{where}: invalid neighbor") from exc
            if "since" in arg and arg["since"] not in marks: raise SpecError(f"{where}: unknown packet mark {arg['since']!r}")
    return s

def process_penalty_bounds(aged_msec, extra_msec):
    """Bracket a second Q16 penalty across elapsed replay time.

    The exact recurrence can occur any time after process-limit-age and
    before the next STATE reply. Decay continues throughout that interval.
    """
    def penalty(elapsed):
        return 65536 + 65536 * 2 ** (-elapsed / 450000)

    return (int(penalty(aged_msec + extra_msec)) - 256,
            int(penalty(aged_msec)) + 256)


class Node:
    def __init__(self,name,config,sock):
        self.name=name; self.config=config; self.sock=sock; self.p=None
        self.stderr=[]; self.err_thread=None; self.stdout_thread=None
        self.process_limit_age_elapsed=None
        self.process_limit_age_started=None
        self.responses=queue.Queue(); self.cmd_lock=threading.Lock()
    def _start_stdout_reader(self):
        # Read from the buffered TextIO stream in exactly one thread. select()
        # on the underlying fd can miss lines already held in TextIO's buffer;
        # reading additional lines on the caller thread can block forever.
        self.responses=queue.Queue()
        stream=self.p.stdout
        responses=self.responses
        def read_responses():
            try:
                for row in stream:
                    responses.put(row.rstrip("\r\n"))
            finally:
                responses.put(None)  # EOF, including abnormal child exit
        self.stdout_thread=threading.Thread(target=read_responses,daemon=True)
        self.stdout_thread.start()
    def start(self):
        if self.p and self.p.poll() is None: return
        env=os.environ.copy(); env["EIGRP_UNIX_WIRE_SOCKET"]=self.sock
        self.p=subprocess.Popen([str(NODE),str(self.config)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,bufsize=1,env=env)
        self.stderr=[]
        stderr_stream=self.p.stderr
        def drain():
            for line in stderr_stream: self.stderr.append(line.rstrip())
        self.err_thread=threading.Thread(target=drain,daemon=True); self.err_thread.start()
        self._start_stdout_reader()
        line=self._readline(5)
        if line!="READY": raise RuntimeError(f"{self.name}: failed to start: {line!r}; returncode={self.p.poll()}; stderr={list(self.stderr)}")
    def _readline(self,timeout):
        try:
            row=self.responses.get(timeout=max(0,timeout))
        except queue.Empty as exc:
            raise TimeoutError(f"{self.name}: response timeout") from exc
        if row is None:
            raise RuntimeError(f"{self.name}: UUT closed command output; "
                               f"returncode={self.p.poll()}; stderr={list(self.stderr)}")
        return row
    def cmd(self,line,until=None,timeout=3):
        # Batched topology operations may target multiple interfaces on one
        # UUT. Keep each stdin/stdout transaction atomic per node while
        # allowing commands to different UUTs to remain concurrent.
        with self.cmd_lock:
            if not self.p or self.p.poll() is not None: raise RuntimeError(f"{self.name}: UUT is not running; returncode={None if not self.p else self.p.poll()}; stderr={list(self.stderr)}")
            deadline=time.monotonic()+timeout
            self.p.stdin.write(line+"\n"); self.p.stdin.flush()
            out=[]
            while True:
                remaining=deadline-time.monotonic()
                if remaining<=0:
                    raise TimeoutError(f"{self.name}: {line} response timeout "
                                       f"before {until or 'first line'} ({len(out)} lines received)")
                row=self._readline(remaining)
                if until is None: return row
                if row==until: return out
                out.append(row)
    def state(self):
        rows=self.cmd("STATE",until="END"); st={"interfaces":[],"neighbors":[],"accounting":[],"topology":[],"paths":[],"rib":[],"source":[]}; current_prefix=None
        for row in rows:
            f=row.split("|"); typ=f[0]; kv=dict(part.split("=",1) for part in f[1:] if "=" in part)
            if typ=="INTF": st["interfaces"].append({"interface":kv["name"],"runtime":kv["runtime_present"]=="1","shutdown":kv["shutdown"]=="1","peers":int(kv["peer_count"]),"output_q":int(kv["output_queue"]),"reliable_q":int(kv["reliable_queue"])})
            elif typ=="NBR": st["neighbors"].append({"address":kv["address"],"interface":kv["interface"],"state":kv["state"],"runtime":kv["runtime_present"]=="1","reliable_q":int(kv["reliable_queue"]),"rto":int(kv["rto_ms"]),"srtt_valid":kv["srtt_valid"]=="1","srtt":int(kv["srtt_ms"]),"retransmissions":int(kv["retransmits"])})
            elif typ=="ACCOUNT": st["accounting"].append({"address":kv["address"],"interface":kv["interface"],"status":kv["status"],"prefix_count":int(kv["prefix_count"]),"restart_count":int(kv["restart_count"]),"restart_active":int(kv["restart_seconds"])>0,"reset_active":int(kv["reset_seconds"])>0})
            elif typ=="TOPO":
                current_prefix=kv["prefix"]; st["topology"].append({"prefix":current_prefix,"active":kv["active"]=="1","fd":int(kv["feasible_distance"]),"successors":int(kv["successors"])})
            elif typ=="PATH": st["paths"].append({"prefix":current_prefix,"next_hop":kv["next_hop"],"interface":kv["interface"],"connected":kv["connected"]=="1","successor":kv["successor"]=="1","feasible_successor":kv.get("feasible_successor")=="1","distance":int(kv["distance"]),"rd":int(kv["reported_distance"])})
            elif typ=="RIB": st["rib"].append({"prefix":kv["prefix"],"next_hop":kv["next_hop"],"metric":int(kv["metric"]),"distance":int(kv["admin_distance"])})
            elif typ=="REDIST-LIMIT": st["redist_limit"]={"state":int(kv["state"]),"restarts":int(kv["restarts"]),"count":int(kv["count"]),"restart_active":kv["restart_active"]=="1","reset_active":kv["reset_active"]=="1", "penalty":int(kv.get("penalty",0))}
            elif typ=="PROCESS-LIMIT": st["process_limit"]={"state":int(kv["state"]),"restarts":int(kv["restarts"]),"count":int(kv["count"]),"restart_active":kv["restart_active"]=="1","reset_active":kv["reset_active"]=="1", "penalty":int(kv.get("penalty",0))}
            elif typ=="SRC": st["source"].append({"prefix":kv["prefix"],"protocol":int(kv["protocol"]),"metric":int(kv["metric"])})
        return st
    def link(self,up,ifname):
        # Link transitions can legitimately block behind an in-flight serialized
        # protocol callback.  Use a control timeout long enough to observe the
        # result instead of declaring a live UUT dead under dense-mesh load.
        r=self.cmd(("UP" if up else "DOWN")+" "+ifname,timeout=10)
        if r!="RESULT|0": raise RuntimeError(f"{self.name}:{ifname}: link operation failed: {r}")
    def peer_limit(self, interface, neighbor, operation):
        result = self.cmd(f"LIMIT {interface} {neighbor} {operation}", timeout=10)
        if result != "RESULT|0":
            raise RuntimeError(f"{self.name}: peer limit operation failed: {result}")

    def peer_limit_config(self, maximum, warning_only=False):
        result = self.cmd(f"LIMIT-CFG {maximum} {int(warning_only)}", timeout=10)
        if result != "RESULT|0":
            raise RuntimeError(f"{self.name}: limit configuration failed: {result}")

    def redist_limit_config(self, maximum, warning_only=False):
        result = self.cmd(f"REDIST-LIMIT-CFG {maximum} {int(warning_only)}", timeout=10)
        if result != "RESULT|0":
            raise RuntimeError(f"{self.name}: redistribution limit configuration failed: {result}")

    def process_limit_config(self, maximum, warning_only=False, dampened=False):
        result = self.cmd(f"PROCESS-LIMIT-CFG {maximum} {int(warning_only)} {int(dampened)}", timeout=10)
        if result != "RESULT|0":
            raise RuntimeError(f"{self.name}: process limit configuration failed: {result}")

    def inbound_filter_config(self, prefix, enabled=True):
        result = self.cmd(f"INBOUND-DENY {prefix if enabled else 'off'}", timeout=10)
        if result != "RESULT|0":
            raise RuntimeError(f"{self.name}: inbound policy update failed: {result}")

    def process_limit_expire(self):
        result = self.cmd("PROCESS-LIMIT-EXPIRE", timeout=10)
        if result != "RESULT|0":
            raise RuntimeError(f"{self.name}: process restart expiry failed: {result}")

    def process_limit_reset_expire(self):
        if self.cmd("PROCESS-LIMIT-RESET-EXPIRE", timeout=10) != "RESULT|0":
            raise RuntimeError(f"{self.name}: process reset timer expiry failed")

    def process_limit_age(self):
        started = time.monotonic()
        result = self.cmd("PROCESS-LIMIT-AGE", timeout=10)
        if not result.startswith("RESULT|0|elapsed="):
            raise RuntimeError(f"{self.name}: process penalty aging failed: {result}")
        self.process_limit_age_elapsed = int(result.split("=", 1)[1])
        self.process_limit_age_started = started

    def process_limit_stale(self):
        if self.cmd("PROCESS-LIMIT-STALE", timeout=10) != "RESULT|0":
            raise RuntimeError(f"{self.name}: process stale callback failed")

    def process_limit_clear(self):
        result = self.cmd("PROCESS-LIMIT-CLEAR", timeout=10)
        if result != "RESULT|0":
            raise RuntimeError(f"{self.name}: process penalty clear failed: {result}")

    def redist_limit_expire(self):
        result = self.cmd("REDIST-LIMIT-EXPIRE", timeout=10)
        if result != "RESULT|0":
            raise RuntimeError(f"{self.name}: redistribution restart expiry failed: {result}")

    def redist_limit_clear(self):
        result = self.cmd("REDIST-LIMIT-CLEAR", timeout=10)
        if result != "RESULT|0":
            raise RuntimeError(f"{self.name}: redistribution penalty clear failed: {result}")

    def source_route_change(self, prefix, add, metric=1):
        command = (f"SOURCE-ADD {prefix} {metric}" if add
                   else f"SOURCE-DEL {prefix}")
        result = self.cmd(command, timeout=10)
        if result != "RESULT|0":
            raise RuntimeError(f"{self.name}: source route change failed: {result}")

    def redistribute_change(self, source, add):
        result = self.cmd(f"REDIST-{'ON' if add else 'OFF'} {source}", timeout=10)
        if result != "RESULT|0":
            raise RuntimeError(f"{self.name}: redistribution change failed: {result}")

    def stop(self):
        if not self.p: return
        if self.p.poll() is None:
            try: self.p.stdin.write("STOP\n"); self.p.stdin.flush(); self.p.wait(3)
            except Exception: self.p.terminate()
        try: self.p.wait(2)
        except subprocess.TimeoutExpired: self.p.kill(); self.p.wait()

class Runner:
    def __init__(self,t,s,work,verbose=False,report=False): self.t=t; self.s=s; self.work=Path(work); self.verbose=verbose; self.report=report; self.report_emitted=False; self.nodes={}; self.broker=None; self.packet_marks={}; self.checkpoints=[]
    def build(self): subprocess.run(["make","-C",str(Path(__file__).parent)],cwd=ROOT,check=True,stdout=None if self.verbose else subprocess.DEVNULL)
    def configs(self):
        owners={}; next_if=1; afi=topology_afi(self.t)
        for rn,r in self.t["routers"].items():
            lines=[f"router {rn} {r['asn']}", f"afi {'ipv6' if afi==6 else 'ipv4'}"]
            for name,i in r.get("interfaces",{}).items():
                lines.append(f"interface {name} {next_if} {int(i.get('bandwidth',1000000))} {int(i.get('mtu',1500))}"); next_if+=1
                for fam in ("ipv4","ipv6"):
                    vals=i.get(fam,[]); vals=[vals] if isinstance(vals,str) else vals
                    for p in vals:
                        lines.append(f"address {name} {p}"); ip=ipaddress.ip_interface(p).ip; raw=ip.packed.ljust(16,b"\0"); owners[(4 if ip.version==4 else 6,raw[:4] if ip.version==4 else raw)]=rn
                lines.append(f"segment {name} {i['segment']}")
                if i.get("up",True): lines.append(f"up {name}")
            for idx,n in enumerate(r.get("attached_networks",[]),1):
                p=ipaddress.ip_interface(n["prefix"]); name=n.get("interface",f"attached{idx}")
                lines += [f"interface {name} {next_if} {int(n.get('bandwidth',1000000))} {int(n.get('mtu',1500))}",f"address {name} {p}",f"up {name}"]; next_if+=1
            for n in r.get("source_networks",[]):
                lines.append(f"source {ipaddress.ip_interface(n['prefix']).network} {int(n.get('metric', 1))}")
            for source in r.get("redistribute", []):
                lines.append(f"redistribute {source}")
            lines.append(f"router-id {r['router_id']}")
            if "active_time" in r: lines.append(f"active-time {int(r['active_time'])}")
            for name,i in r.get("interfaces",{}).items():
                for p in (i.get("ipv4",[]) if isinstance(i.get("ipv4",[]),list) else [i["ipv4"]]): lines.append(f"network {ipaddress.ip_interface(p).network}")
            for idx,n in enumerate(r.get("attached_networks",[]),1):
                p=ipaddress.ip_interface(n["prefix"])
                if p.version==4:
                    lines.append(f"network {p.network}")
                else:
                    lines.append(f"passive {n.get('interface',f'attached{idx}')}")
            for n in r.get("attached_networks",[]):
                if "delay" in n: lines.append(f"delay {n.get('interface')} {int(n['delay'])}")
            for name,i in r.get("interfaces",{}).items():
                if "delay" in i: lines.append(f"delay {name} {int(i['delay'])}")
                for summary in i.get("summaries",[]): lines.append(f"summary {name} {ipaddress.ip_network(summary,strict=False)}")
            for name,i in r.get("interfaces",{}).items():
                auth=i.get("authentication")
                if auth:
                    lines.append(f"auth-md5 {name} {auth['keychain']} {auth['key']}")
            for sm in r.get("summary_metrics",[]):
                lines.append(f"summary-metric {ipaddress.ip_network(sm['prefix'],strict=False)} {int(sm['bandwidth'])} {int(sm['delay'])} {int(sm.get('reliability',255))} {int(sm.get('load',1))} {int(sm.get('mtu',1500))}")
            path=self.work/f"{rn}.conf"; path.write_text("\n".join(lines)+"\n"); self.nodes[rn]=Node(rn,path,str(self.work/"wire.sock"))
        return owners
    def checkpoint(self, label):
        """Preserve bounded diagnostic snapshots before later events overwrite the ring."""
        out = [f"===== UUT checkpoint {label!r} ====="]
        for name, node in self.nodes.items():
            if not node.p or node.p.poll() is not None:
                continue
            out.append(f"===== checkpoint {label!r}: {name} STATE =====")
            try:
                out.extend(node.cmd("STATE", until="END", timeout=5))
            except Exception as exc:
                raise RuntimeError(f"checkpoint {label!r} STATE {name}: {exc}") from exc
            out.append(f"===== checkpoint {label!r}: {name} EVENTS =====")
            try:
                events=node.cmd("EVENTS", until="END", timeout=5)
                out.extend(events)
            except Exception as exc:
                raise RuntimeError(f"checkpoint {label!r} EVENTS {name}: {exc}") from exc
        packets=list(self.broker.journal)
        previous=self.checkpoints[-1][1] if self.checkpoints else 0
        out.append(f"===== checkpoint {label!r}: broker journal [{previous}:{len(packets)}] =====")
        out.extend(self._packet_line(packet) for packet in packets[previous:])
        self.checkpoints.append((label,len(packets),"\n".join(out)))

    def diagnostics(self,focus=None):
        names=[focus] if focus in self.nodes else list(self.nodes)
        out=[snapshot for _, _, snapshot in self.checkpoints]
        for n in names:
            node=self.nodes[n]
            out.append(f"===== UUT {n}: show eigrp tech-support =====")
            try: out.extend(node.cmd("TECH",until="END",timeout=3))
            except Exception as e: out.append(f"tech-support unavailable: {e}")
            out.append(f"===== UUT {n}: show eigrp events =====")
            try: out.extend(node.cmd("EVENTS",until="END",timeout=3))
            except Exception as e: out.append(f"events unavailable: {e}")
            out.append(f"===== UUT {n}: debug output =====")
            out.extend(node.stderr if node.stderr else ["<none>"])
        packets=list(self.broker.journal) if self.broker else []
        if self.packet_marks:
            mark,index=max(self.packet_marks.items(),key=lambda item:item[1])
            out.append(f"===== packet journal since mark {mark!r} =====")
            marked=packets[index:]
            out.extend(self._packet_line(packet) for packet in marked)
            if not marked: out.append("<none>")
        out.append("===== packet journal (last 20) =====")
        tail=packets[-20:]
        out.extend(self._packet_line(packet) for packet in tail)
        if not tail: out.append("<none>")
        return "\n".join(out)
    @staticmethod
    def _packet_line(packet):
        keys=("time","event","source_uut","source_interface","destination_uut","destination_interface","segment","afi","opcode","length","multicast","sequence","auth_sequence","ack","cr","copy","fault","action","fault_hit","fault_remaining")
        fields=[]
        for key in keys:
            if key in packet: fields.append(f"{key}={packet[key]}")
        for key in sorted(set(packet)-set(keys)):
            fields.append(f"{key}={packet[key]}")
        return "PACKET|"+"|".join(fields)
    def neighbor_ok(self,e):
        validate_neighbor_assertion(e, "validate-neighbor")
        st=self.nodes[e["uut"]].state(); addr=e["neighbor"]; iface=e.get("interface"); state=e.get("state","up").lower()
        matches=[n for n in st["neighbors"] if (not addr or n["address"].lower()==str(addr).lower()) and (not iface or n["interface"]==iface)]
        if state == "down":
            return not matches
        if not matches:
            return False
        # A discovered neighbor can remain PENDING while RTP INIT is in
        # progress.  'up' means adjacency established, not merely present.
        if state == "up" and any(n["state"].lower() != "up" for n in matches):
            return False
        if state == "limit-suspended" and any(n["state"] != "Prefix-limit Pending" for n in matches):
            return False
        if state == "limit-locked" and any(n["state"] != "Prefix-limit Locked" for n in matches):
            return False
        return all(k not in e or n.get(k)==e[k] for n in matches for k in ("reliable_q","rto","srtt_valid","srtt","retransmissions"))
    def accounting_ok(self,e):
        validate_accounting_assertion(e, "validate-accounting")
        st=self.nodes[e["uut"]].state()
        for row in st["accounting"]:
            if row["address"].lower()!=e["neighbor"].lower():
                continue
            if "interface" in e and row["interface"]!=e["interface"]:
                continue
            if row["status"]!=e.get("status", "A"):
                continue
            if all(k not in e or row[k]==e[k] for k in ("restart_count", "prefix_count", "restart_active", "reset_active")):
                return True
        return False
    def route_ok(self,e):
        validate_route_assertion(e,"validate-route")
        st=self.nodes[e["uut"]].state(); prefix=str(ipaddress.ip_network(e["prefix"],strict=False)); table=e.get("table","rib"); rows=st["rib"] if table=="rib" else st["source"] if table=="source" else st["paths"] if table=="path" else st["topology"]
        keys=("next_hop","metric","distance") if table=="rib" else (("active","fd","successors") if table=="topology" else (("next_hop","interface","connected","successor","feasible_successor","distance","rd") if table=="path" else ("protocol","metric")))
        matches=[r for r in rows if r["prefix"].lower()==prefix.lower() and all(k not in e or r.get(k)==e[k] for k in keys)]
        if not e.get("present",True): return not matches
        if "count" in e: return len(matches)==int(e["count"])
        if "min_count" in e and len(matches)<int(e["min_count"]): return False
        if "max_count" in e and len(matches)>int(e["max_count"]): return False
        return bool(matches)
    def packet_ok(self,e):
        validate_packet_assertion(e,"validate-packet",self.packet_marks)
        rows=list(self.broker.journal); mark=e.get("since"); rows=rows[self.packet_marks[mark]:] if mark else rows
        fields={"prefix":"prefix","uut":"source_uut","source_uut":"source_uut","destination_uut":"destination_uut","interface":"source_interface","source_interface":"source_interface","destination_interface":"destination_interface","segment":"segment","afi":"afi","opcode":"opcode","multicast":"multicast","sequence":"sequence","auth_sequence":"auth_sequence","ack":"ack","cr":"cr","event":"event"}
        def matches_filter(packet, key, field):
            if key not in e: return True
            if key=="prefix": return str(e[key]).lower() in [str(x).lower() for x in packet.get("prefixes",[])]
            if key=="event" and e[key]=="any": return True
            return packet.get(field)==e[key]
        matches=[p for p in rows if all(matches_filter(p,k,v) for k,v in fields.items())]
        count=len(matches)
        if count<int(e.get("min",1)) or ("max" in e and count>int(e["max"])): return False
        group_by=e.get("group_by")
        if group_by:
            if isinstance(group_by,str): group_by=[group_by]
            groups=collections.Counter(tuple(p.get(k) for k in group_by) for p in matches)
            if "min_groups" in e and len(groups)<int(e["min_groups"]): return False
            if "max_groups" in e and len(groups)>int(e["max_groups"]): return False
            if "max_per_group" in e and any(n>int(e["max_per_group"]) for n in groups.values()): return False
        return True
    def event_ok(self,e):
        rows=self.nodes[e["uut"]].cmd("EVENTS",until="END",timeout=float(e.get("timeout",3)))
        needle=str(e["contains"])
        # EVENTS returns newest first.  When `after` is requested, the
        # prerequisite event must appear later in that list (older in time).
        prior=str(e["after"]) if "after" in e else None
        count=sum(1 for pos,row in enumerate(rows) if needle in row and
                  (prior is None or any(prior in older for older in rows[pos+1:])))
        return count>=int(e.get("min",1)) and ("max" not in e or count<=int(e["max"]))
    def fault_ok(self,e):
        f=self.broker.fault_state(e["name"]); return bool(f) and ("hits" not in e or f["hits"]>=int(e["hits"])) and ("remaining" not in e or f["remaining"]==int(e["remaining"]))

    def retransmission_ok(self,e):
        rows=list(self.broker.journal); mark=e.get("since")
        if mark and mark not in self.packet_marks: raise SpecError(f"unknown packet mark {mark!r}")
        rows=rows[self.packet_marks[mark]:] if mark else rows
        for i,p in enumerate(rows):
            if p.get("event")!="dropped" or not p.get("sequence"): continue
            if e.get("source_uut") and p.get("source_uut")!=e["source_uut"]: continue
            if e.get("destination_uut") and p.get("destination_uut")!=e["destination_uut"]: continue
            if e.get("opcode") and p.get("opcode")!=e["opcode"]: continue
            for q in rows[i+1:]:
                if q.get("event") in ("delivered","delayed","duplicated") and q.get("source_uut")==p.get("source_uut") and q.get("destination_uut")==p.get("destination_uut") and q.get("opcode")==p.get("opcode") and q.get("sequence")==p.get("sequence") and not q.get("multicast"):
                    return True
        return False
    def retry_ok(self,e):
        rows=list(self.broker.journal); mark=e.get("since")
        if mark and mark not in self.packet_marks: raise SpecError(f"unknown packet mark {mark!r}")
        rows=rows[self.packet_marks[mark]:] if mark else rows; seen=set()
        for p in rows:
            if p.get("event")!="sent" or not p.get("sequence"): continue
            if e.get("source_uut") and p.get("source_uut")!=e["source_uut"]: continue
            if e.get("opcode") and p.get("opcode")!=e["opcode"]: continue
            key=(p.get("source_uut"),p.get("source_interface"),p.get("opcode"),p.get("sequence"))
            if key in seen: return True
            seen.add(key)
        return False
    def expect(self,kind,e):
        timeout=float(e.get("timeout",5)); deadline=time.monotonic()+timeout; okfn={"neighbor":self.neighbor_ok,"accounting":self.accounting_ok,"route":self.route_ok,"packet":self.packet_ok,"fault":self.fault_ok,"retransmission":self.retransmission_ok,"retry":self.retry_ok}[kind]
        if kind=="packet" and any(k in e for k in ("max","max_groups","max_per_group")):
            upper=dict(e); upper["min"]=0; upper.pop("min_groups",None)
            while time.monotonic()<deadline:
                if not okfn(upper): raise AssertionError(f"validate-{kind} upper bound failed: expected {e}")
                time.sleep(.05)
            if okfn(e): return
            raise AssertionError(f"validate-{kind} failed at end of observation window: expected {e}")
        if ((kind=="neighbor" and e.get("state", "up").lower() not in ("up", "present")) or kind=="accounting") and e.get("observe"):
            deadline=time.monotonic()+float(e["observe"])
            while time.monotonic()<deadline:
                if not okfn(e):
                    raise AssertionError(f"validate-neighbor failed during observation: expected {e}")
                time.sleep(.1)
            return
        while time.monotonic()<deadline:
            if okfn(e): return
            time.sleep(.1)
        raise AssertionError(f"validate-{kind} failed: expected {e}")
    def step(self,op,arg):
        if self.verbose: print(f"-- {op}: {arg}",flush=True)
        if op=="start":
            names=list(self.nodes) if arg in (None,"all") else ([arg] if isinstance(arg,str) else arg.get("uuts",list(self.nodes)))
            for n in names:
                try: self.nodes[n].start()
                except Exception:
                    if self.broker and self.broker.error: raise RuntimeError(f"wire broker failed: {self.broker.error}")
                    raise
        elif op=="stop":
            names=list(self.nodes) if arg in (None,"all") else ([arg] if isinstance(arg,str) else arg.get("uuts",list(self.nodes)))
            for n in names: self.nodes[n].stop()
        elif op in ("link-up","link-down"):
            self.nodes[arg["uut"]].link(op=="link-up",arg["interface"])
        elif op in ("links-up","links-down"):
            links=arg if isinstance(arg,list) else arg.get("links",[])
            errors=[]; gate=threading.Barrier(len(links)) if len(links)>1 else None
            def change(link):
                try:
                    if gate: gate.wait()
                    self.nodes[link["uut"]].link(op=="links-up",link["interface"])
                except Exception as exc: errors.append((link,exc))
            threads=[threading.Thread(target=change,args=(link,)) for link in links]
            for thread in threads: thread.start()
            for thread in threads: thread.join()
            if errors: raise RuntimeError(f"batched link operation failed: {errors}")
        elif op == "peer-limit":
            self.nodes[arg["uut"]].peer_limit(
                arg["interface"], arg["neighbor"], arg["operation"])
        elif op == "peer-limit-config":
            self.nodes[arg["uut"]].peer_limit_config(
                arg["maximum"], arg.get("warning_only", False))
        elif op == "redist-limit-config":
            self.nodes[arg["uut"]].redist_limit_config(
                arg["maximum"], arg.get("warning_only", False))
        elif op == "process-limit-config":
            self.nodes[arg["uut"]].process_limit_config(arg["maximum"], arg.get("warning_only", False), arg.get("dampened", False))
        elif op == "inbound-filter-config":
            self.nodes[arg["uut"]].inbound_filter_config(
                arg["prefix"], arg.get("enabled", True))
        elif op == "process-limit-expire":
            self.nodes[arg["uut"]].process_limit_expire()
        elif op == "process-limit-reset-expire":
            self.nodes[arg["uut"]].process_limit_reset_expire()
        elif op == "process-limit-stale":
            self.nodes[arg["uut"]].process_limit_stale()
        elif op == "process-limit-age":
            self.nodes[arg["uut"]].process_limit_age()
        elif op == "process-limit-clear":
            self.nodes[arg["uut"]].process_limit_clear()
        elif op == "validate-process-limit":
            deadline = time.monotonic() + float(arg.get("timeout", 10))
            penalty_min = arg.get("penalty_min", 0)
            penalty_max = arg.get("penalty_max", 4294967295)
            if arg.get("penalty_after_age"):
                node = self.nodes[arg["uut"]]
                if node.process_limit_age_elapsed is None or node.process_limit_age_started is None:
                    raise RuntimeError("process penalty age was not recorded")
            while True:
                actual = self.nodes[arg["uut"]].state().get("process_limit", {})
                if arg.get("penalty_after_age"):
                    # The second violation occurs during asynchronous route
                    # replay, not when PROCESS-LIMIT-AGE returns. Bound its
                    # actual decay by the time elapsed through this STATE read.
                    extra = max(0, int((time.monotonic() - node.process_limit_age_started) * 1000))
                    penalty_min, penalty_max = process_penalty_bounds(node.process_limit_age_elapsed, extra)
                if (all(actual.get(k) == v for k,v in arg.items()
                        if k not in ("uut", "timeout", "penalty_min", "penalty_max", "penalty_after_age"))
                    and penalty_min <= actual.get("penalty", 0) <= penalty_max):
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError(f"process limit state mismatch: {actual} expected {arg} penalty bounds [{penalty_min}, {penalty_max}]")
                time.sleep(0.1)
        elif op == "redist-limit-expire":
            self.nodes[arg["uut"]].redist_limit_expire()
        elif op == "redist-limit-clear":
            self.nodes[arg["uut"]].redist_limit_clear()
        elif op == "validate-redist-limit":
            deadline = time.monotonic() + float(arg.get("timeout", 10))
            while True:
                actual = self.nodes[arg["uut"]].state().get("redist_limit", {})
                if (all(actual.get(k) == v for k,v in arg.items()
                        if k not in ("uut", "timeout", "penalty_min", "penalty_max"))
                    and actual.get("penalty", 0) >= arg.get("penalty_min", 0)
                    and actual.get("penalty", 0) <= arg.get("penalty_max", 4294967295)):
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError(f"redistribution limit state mismatch: {actual} expected {arg}")
                time.sleep(0.1)
        elif op in ("source-route-add", "source-route-remove"):
            prefix = str(ipaddress.ip_network(arg["prefix"], strict=False))
            self.nodes[arg["uut"]].source_route_change(
                prefix, op=="source-route-add", int(arg.get("metric", 1)))
        elif op in ("redistribute-add", "redistribute-remove"):
            self.nodes[arg["uut"]].redistribute_change(
                arg["source"], op=="redistribute-add")
        elif op=="wait": time.sleep(float(arg if isinstance(arg,(int,float)) else arg.get("seconds",1)))
        elif op=="packet-mark":
            self.packet_marks[str(arg)]=len(self.broker.journal)
            # Preserve event/STATE histories at every phase boundary in
            # qualification reports, including scenarios without explicit
            # checkpoint steps. No production ring-buffer growth is required.
            if self.report: self.checkpoint(f"packet-mark:{arg}")
        elif op=="checkpoint": self.checkpoint(str(arg))
        elif op=="fault-add": self.broker.add_fault(arg)
        elif op=="fault-remove":
            if not self.broker.remove_fault(arg if isinstance(arg,str) else arg["name"]): raise SpecError(f"unknown fault {arg}")
        elif op=="validate-fault": self.expect("fault",arg)
        elif op=="validate-neighbor": self.expect("neighbor",arg)
        elif op=="validate-accounting": self.expect("accounting",arg)
        elif op=="validate-route": self.expect("route",arg)
        elif op=="validate-packet": self.expect("packet",arg)
        elif op=="validate-retransmission": self.expect("retransmission",arg)
        elif op=="validate-retry": self.expect("retry",arg)
        elif op=="validate-log":
            node=self.nodes[arg["uut"]]
            deadline=time.monotonic()+float(arg.get("timeout",3))
            while True:
                matching=[line for line in list(node.stderr) if arg["contains"] in line]
                if len(matching)>=arg.get("min",1): break
                if time.monotonic()>=deadline:
                    raise AssertionError(f"validate-log failed: {arg}; stderr={list(node.stderr)}")
                time.sleep(.05)
        elif op=="validate-event":
            if not self.event_ok(arg): raise AssertionError(f"validate-event failed: {arg}")
        elif op=="validate-convergence":
            timeout=float(arg.get("timeout",10)); deadline=time.monotonic()+timeout; expectations=arg.get("expect",[])
            stable_for=float(arg.get("stable_for",0)); stable_since=None
            while time.monotonic()<deadline:
                now=time.monotonic()
                if all((self.neighbor_ok(v) if k=="neighbor" else self.route_ok(v) if k=="route" else self.packet_ok(v)) for x in expectations for k,v in x.items()):
                    if stable_since is None: stable_since=now
                    if now-stable_since>=stable_for: return
                else:
                    stable_since=None
                time.sleep(.1)
            raise AssertionError(f"validate-convergence failed (stable_for={stable_for}): {arg}")
    def unexpected_wire_errors(self):
        """No passing scenario may conceal a failed broker send on the test fabric."""
        errors=[p for p in self.broker.journal if p.get("event")=="send-error"]
        if errors:
            raise AssertionError(f"unexpected wire transmit errors: {errors[:3]!r}")

    def run(self):
        self.build(); owners=self.configs(); self.broker=Broker(str(self.work/"wire.sock"),owners); self.broker.start(); self.broker.ready.wait(2)
        if self.broker.error: raise self.broker.error
        try:
            for i,step in enumerate(self.s["steps"],1):
                op,arg=next(iter(step.items()))
                if self.report and op=="stop" and arg in (None,"all") and not self.report_emitted:
                    print(self.diagnostics(),flush=True)
                    self.report_emitted=True
                try:
                    # A terminal STOP tears routers down one process at a time.
                    # Quiesce the virtual wire *before* that teardown: otherwise a
                    # surviving router can transmit to a socket just closed by
                    # another router and produce a spurious send-error.  Closing
                    # the broker only at the terminal step preserves observation
                    # of genuine send errors throughout the active scenario.
                    if op == "stop" and i == len(self.s["steps"]):
                        self.broker.close()
                        self.unexpected_wire_errors()
                    self.step(op,arg)
                except Exception as e: raise RuntimeError(f"step {i} ({op}) failed: {e}\n{self.diagnostics()}") from e
            self.unexpected_wire_errors()
            if self.report and not self.report_emitted:
                print(self.diagnostics(),flush=True)
                self.report_emitted=True
        finally:
            # The same ordering is required on assertion failures (when YAML
            # has not reached an explicit terminal STOP step).
            self.broker.close()
            for n in self.nodes.values(): n.stop()

def main():
    ap=argparse.ArgumentParser(); ap.add_argument("topology"); ap.add_argument("scenario"); ap.add_argument("-v","--verbose",action="store_true"); ap.add_argument("--report",action="store_true",help="emit final tech-support, events, debug, and packet diagnostics"); a=ap.parse_args()
    try:
        t=validate_topology(load_yaml(a.topology)); s=validate_scenario(load_yaml(a.scenario))
        with tempfile.TemporaryDirectory(prefix="eigrp-uut-") as d: Runner(t,s,d,a.verbose,a.report).run()
        print(f"PASS: {s.get('name',Path(a.scenario).name)}")
        return 0
    except Exception as e:
        print(f"FAIL: {e}",file=sys.stderr); return 1
if __name__=="__main__": raise SystemExit(main())
