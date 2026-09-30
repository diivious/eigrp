#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Donnie V. Savage
"""Declarative portable EIGRP UUT topology/scenario runner."""
from __future__ import annotations
import argparse, collections, ipaddress, os, selectors, socket, struct, subprocess, sys, tempfile, threading, time
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
        check_keys(r,{"asn","router_id","interfaces","attached_networks","source_networks","summary_metrics"},f"router {rn}")
        if not 1<=int(r.get("asn",0))<=65535: raise SpecError(f"router {rn}: invalid asn")
        ipaddress.IPv4Address(r.get("router_id"))
        if not isinstance(r.get("interfaces",{}),dict): raise SpecError(f"router {rn}.interfaces must be a mapping")
        for name,i in r.get("interfaces",{}).items():
            check_keys(i,{"segment","ipv4","ipv6","mtu","bandwidth","delay","up","summaries"},f"{rn}.{name}")
            summaries=i.get("summaries",[])
            if not isinstance(summaries,list): raise SpecError(f"{rn}.{name}.summaries must be a list")
            for summary in summaries: ipaddress.ip_network(summary,strict=False)
            if i.get("segment") not in t.get("segments",{}): raise SpecError(f"{rn}.{name}: unknown segment {i.get('segment')}")
            for key in ("ipv4","ipv6"):
                vals=i.get(key,[]); vals=[vals] if isinstance(vals,str) else vals
                if not isinstance(vals,list): raise SpecError(f"{rn}.{name}.{key} must be string/list")
                for p in vals: ipaddress.ip_interface(p)
        for sm in r.get("summary_metrics",[]):
            check_keys(sm,{"prefix","bandwidth","delay","reliability","load","mtu"},f"{rn}.summary_metrics")
            ipaddress.ip_network(sm["prefix"],strict=False)
        for key in ("attached_networks","source_networks"):
            vals=r.get(key,[])
            if not isinstance(vals,list): raise SpecError(f"{rn}.{key} must be a list")
            for n in vals:
                if not isinstance(n,dict): raise SpecError(f"{rn}.{key} entries must be mappings")
                check_keys(n,{"prefix","interface","protocol","metric","bandwidth","delay","mtu"},f"{rn}.{key}")
                ipaddress.ip_interface(n["prefix"])
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

def validate_scenario(s):
    check_keys(s,{"schema","name","topology","steps"},"scenario")
    if s.get("schema")!=TEST_SCHEMA: raise SpecError(f"scenario schema must be {TEST_SCHEMA}")
    if not isinstance(s.get("topology"),str) or not s["topology"]: raise SpecError("scenario.topology must name its topology YAML")
    if not isinstance(s.get("steps"),list): raise SpecError("scenario.steps must be a list")
    allowed={"start","stop","link-up","link-down","wait","packet-mark","fault-add","fault-remove","validate-fault","validate-neighbor","validate-route","validate-packet","validate-retransmission","validate-retry","validate-convergence","validate-event"}
    for idx,step in enumerate(s["steps"],1):
        if not isinstance(step,dict) or len(step)!=1: raise SpecError(f"step {idx}: exactly one operation required")
        op=next(iter(step));
        if op not in allowed: raise SpecError(f"step {idx}: unsupported operation {op}")
    return s

class Node:
    def __init__(self,name,config,sock): self.name=name; self.config=config; self.sock=sock; self.p=None; self.stderr=[]; self.err_thread=None
    def start(self):
        if self.p and self.p.poll() is None: return
        env=os.environ.copy(); env["EIGRP_UNIX_WIRE_SOCKET"]=self.sock
        self.p=subprocess.Popen([str(NODE),str(self.config)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,bufsize=1,env=env)
        def drain():
            for line in self.p.stderr: self.stderr.append(line.rstrip())
        self.err_thread=threading.Thread(target=drain,daemon=True); self.err_thread.start()
        line=self._readline(5)
        if line!="READY": raise RuntimeError(f"{self.name}: failed to start: {line!r}; returncode={self.p.poll()}; stderr={list(self.stderr)}")
    def _readline(self,timeout):
        sel=selectors.DefaultSelector(); sel.register(self.p.stdout,selectors.EVENT_READ); events=sel.select(timeout); sel.close()
        if not events: raise TimeoutError(f"{self.name}: response timeout")
        return self.p.stdout.readline().rstrip("\n")
    def cmd(self,line,until=None,timeout=3):
        if not self.p or self.p.poll() is not None: raise RuntimeError(f"{self.name}: UUT is not running")
        self.p.stdin.write(line+"\n"); self.p.stdin.flush()
        if until is None: return self._readline(timeout)
        out=[]
        deadline=time.monotonic()+timeout
        row=self._readline(max(.01,deadline-time.monotonic()))
        while True:
            if row==until: return out
            out.append(row)
            # Once the response starts, the UUT emits the complete snapshot
            # synchronously.  Read the TextIO buffer directly; select() only
            # sees the underlying fd and can miss lines already buffered here.
            row=self.p.stdout.readline().rstrip("\n")
            if not row and self.p.poll() is not None: raise RuntimeError(f"{self.name}: exited during response")
    def state(self):
        rows=self.cmd("STATE",until="END"); st={"interfaces":[],"neighbors":[],"topology":[],"paths":[],"rib":[],"source":[]}; current_prefix=None
        for row in rows:
            f=row.split("|"); typ=f[0]; kv=dict(part.split("=",1) for part in f[1:] if "=" in part)
            if typ=="INTF": st["interfaces"].append({"interface":kv["name"],"runtime":kv["runtime_present"]=="1","shutdown":kv["shutdown"]=="1","peers":int(kv["peer_count"]),"output_q":int(kv["output_queue"]),"reliable_q":int(kv["reliable_queue"])})
            elif typ=="NBR": st["neighbors"].append({"address":kv["address"],"interface":kv["interface"],"state":kv["state"],"runtime":kv["runtime_present"]=="1","reliable_q":int(kv["reliable_queue"]),"rto":int(kv["rto_ms"]),"srtt_valid":kv["srtt_valid"]=="1","srtt":int(kv["srtt_ms"]),"retransmissions":int(kv["retransmits"])})
            elif typ=="TOPO":
                current_prefix=kv["prefix"]; st["topology"].append({"prefix":current_prefix,"active":kv["active"]=="1","fd":int(kv["feasible_distance"]),"successors":int(kv["successors"])})
            elif typ=="PATH": st["paths"].append({"prefix":current_prefix,"next_hop":kv["next_hop"],"interface":kv["interface"],"connected":kv["connected"]=="1","successor":kv["successor"]=="1","feasible_successor":kv.get("feasible_successor")=="1","distance":int(kv["distance"]),"rd":int(kv["reported_distance"])})
            elif typ=="RIB": st["rib"].append({"prefix":kv["prefix"],"next_hop":kv["next_hop"],"metric":int(kv["metric"]),"distance":int(kv["admin_distance"])})
            elif typ=="SRC": st["source"].append({"prefix":kv["prefix"],"protocol":int(kv["protocol"]),"metric":int(kv["metric"])})
        return st
    def link(self,up,ifname):
        r=self.cmd(("UP" if up else "DOWN")+" "+ifname)
        if r!="RESULT|0": raise RuntimeError(f"{self.name}:{ifname}: link operation failed: {r}")
    def stop(self):
        if not self.p: return
        if self.p.poll() is None:
            try: self.p.stdin.write("STOP\n"); self.p.stdin.flush(); self.p.wait(3)
            except Exception: self.p.terminate()
        try: self.p.wait(2)
        except subprocess.TimeoutExpired: self.p.kill(); self.p.wait()

class Runner:
    def __init__(self,t,s,work,verbose=False,report=False): self.t=t; self.s=s; self.work=Path(work); self.verbose=verbose; self.report=report; self.report_emitted=False; self.nodes={}; self.broker=None; self.packet_marks={}
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
            lines.append(f"router-id {r['router_id']}")
            for name,i in r.get("interfaces",{}).items():
                for p in (i.get("ipv4",[]) if isinstance(i.get("ipv4",[]),list) else [i["ipv4"]]): lines.append(f"network {ipaddress.ip_interface(p).network}")
            for n in r.get("attached_networks",[]):
                p=ipaddress.ip_interface(n["prefix"])
                if p.version==4: lines.append(f"network {p.network}")
            for n in r.get("attached_networks",[]):
                if "delay" in n: lines.append(f"delay {n.get('interface')} {int(n['delay'])}")
            for name,i in r.get("interfaces",{}).items():
                if "delay" in i: lines.append(f"delay {name} {int(i['delay'])}")
                for summary in i.get("summaries",[]): lines.append(f"summary {name} {ipaddress.ip_network(summary,strict=False)}")
            for sm in r.get("summary_metrics",[]):
                lines.append(f"summary-metric {ipaddress.ip_network(sm['prefix'],strict=False)} {int(sm['bandwidth'])} {int(sm['delay'])} {int(sm.get('reliability',255))} {int(sm.get('load',1))} {int(sm.get('mtu',1500))}")
            path=self.work/f"{rn}.conf"; path.write_text("\n".join(lines)+"\n"); self.nodes[rn]=Node(rn,path,str(self.work/"wire.sock"))
        return owners
    def diagnostics(self,focus=None):
        names=[focus] if focus in self.nodes else list(self.nodes)
        out=[]
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
        keys=("time","event","source_uut","source_interface","destination_uut","destination_interface","segment","afi","opcode","length","multicast","sequence","ack","cr","copy","fault","action","fault_hit","fault_remaining")
        fields=[]
        for key in keys:
            if key in packet: fields.append(f"{key}={packet[key]}")
        for key in sorted(set(packet)-set(keys)):
            fields.append(f"{key}={packet[key]}")
        return "PACKET|"+"|".join(fields)
    def neighbor_ok(self,e):
        st=self.nodes[e["uut"]].state(); addr=e.get("neighbor"); iface=e.get("interface"); state=e.get("state","up").lower()
        matches=[n for n in st["neighbors"] if (not addr or n["address"].lower()==str(addr).lower()) and (not iface or n["interface"]==iface)]
        if state not in ("up","present"):
            return not matches
        if not matches:
            return False
        return all(k not in e or n.get(k)==e[k] for n in matches for k in ("reliable_q","rto","srtt_valid","srtt","retransmissions"))
    def route_ok(self,e):
        st=self.nodes[e["uut"]].state(); prefix=str(ipaddress.ip_network(e["prefix"],strict=False)); table=e.get("table","rib"); rows=st["rib"] if table=="rib" else st["source"] if table=="source" else st["paths"] if table=="path" else st["topology"]
        matches=[r for r in rows if r["prefix"].lower()==prefix.lower()]
        if not e.get("present",True): return not matches
        if not matches: return False
        keys=("next_hop","metric","distance") if table=="rib" else (("active","fd","successors") if table=="topology" else (("next_hop","interface","connected","successor","feasible_successor","distance","rd") if table=="path" else ("protocol","metric")))
        return any(all(k not in e or r.get(k)==e[k] for k in keys) for r in matches)
    def packet_ok(self,e):
        rows=list(self.broker.journal); mark=e.get("since"); rows=rows[self.packet_marks.get(mark,0):] if mark else rows
        fields={"prefix":"prefix","uut":"source_uut","source_uut":"source_uut","destination_uut":"destination_uut","interface":"source_interface","source_interface":"source_interface","destination_interface":"destination_interface","segment":"segment","afi":"afi","opcode":"opcode","multicast":"multicast","sequence":"sequence","ack":"ack","cr":"cr","event":"event"}
        count=sum(1 for p in rows if all(k not in e or ((str(e[k]).lower() in [str(x).lower() for x in p.get("prefixes",[])]) if k=="prefix" else p.get(v)==e[k]) for k,v in fields.items()))
        return count>=int(e.get("min",1)) and ("max" not in e or count<=int(e["max"]))
    def event_ok(self,e):
        rows=self.nodes[e["uut"]].cmd("EVENTS",until="END",timeout=float(e.get("timeout",3)))
        needle=str(e["contains"])
        count=sum(1 for row in rows if needle in row)
        return count>=int(e.get("min",1)) and ("max" not in e or count<=int(e["max"]))
    def fault_ok(self,e):
        f=self.broker.fault_state(e["name"]); return bool(f) and ("hits" not in e or f["hits"]>=int(e["hits"])) and ("remaining" not in e or f["remaining"]==int(e["remaining"]))

    def retransmission_ok(self,e):
        rows=list(self.broker.journal); mark=e.get("since"); rows=rows[self.packet_marks.get(mark,0):] if mark else rows
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
        rows=list(self.broker.journal); mark=e.get("since"); rows=rows[self.packet_marks.get(mark,0):] if mark else rows; seen=set()
        for p in rows:
            if p.get("event")!="sent" or not p.get("sequence"): continue
            if e.get("source_uut") and p.get("source_uut")!=e["source_uut"]: continue
            if e.get("opcode") and p.get("opcode")!=e["opcode"]: continue
            key=(p.get("source_uut"),p.get("source_interface"),p.get("opcode"),p.get("sequence"))
            if key in seen: return True
            seen.add(key)
        return False
    def expect(self,kind,e):
        timeout=float(e.get("timeout",5)); deadline=time.monotonic()+timeout; okfn={"neighbor":self.neighbor_ok,"route":self.route_ok,"packet":self.packet_ok,"fault":self.fault_ok,"retransmission":self.retransmission_ok,"retry":self.retry_ok}[kind]
        if kind=="packet" and int(e.get("max",-1))==0:
            while time.monotonic()<deadline:
                if not okfn(e): raise AssertionError(f"validate-{kind} failed: expected {e}")
                time.sleep(.05)
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
        elif op=="wait": time.sleep(float(arg if isinstance(arg,(int,float)) else arg.get("seconds",1)))
        elif op=="packet-mark": self.packet_marks[str(arg)]=len(self.broker.journal)
        elif op=="fault-add": self.broker.add_fault(arg)
        elif op=="fault-remove":
            if not self.broker.remove_fault(arg if isinstance(arg,str) else arg["name"]): raise SpecError(f"unknown fault {arg}")
        elif op=="validate-fault": self.expect("fault",arg)
        elif op=="validate-neighbor": self.expect("neighbor",arg)
        elif op=="validate-route": self.expect("route",arg)
        elif op=="validate-packet": self.expect("packet",arg)
        elif op=="validate-retransmission": self.expect("retransmission",arg)
        elif op=="validate-retry": self.expect("retry",arg)
        elif op=="validate-event":
            if not self.event_ok(arg): raise AssertionError(f"validate-event failed: {arg}")
        elif op=="validate-convergence":
            timeout=float(arg.get("timeout",10)); deadline=time.monotonic()+timeout; expectations=arg.get("expect",[])
            while time.monotonic()<deadline:
                if all((self.neighbor_ok(v) if k=="neighbor" else self.route_ok(v) if k=="route" else self.packet_ok(v)) for x in expectations for k,v in x.items()): return
                time.sleep(.1)
            raise AssertionError(f"validate-convergence failed: {arg}")
    def run(self):
        self.build(); owners=self.configs(); self.broker=Broker(str(self.work/"wire.sock"),owners); self.broker.start(); self.broker.ready.wait(2)
        if self.broker.error: raise self.broker.error
        try:
            for i,step in enumerate(self.s["steps"],1):
                op,arg=next(iter(step.items()))
                if self.report and op=="stop" and arg in (None,"all") and not self.report_emitted:
                    print(self.diagnostics(),flush=True)
                    self.report_emitted=True
                try: self.step(op,arg)
                except Exception as e: raise RuntimeError(f"step {i} ({op}) failed: {e}\n{self.diagnostics()}") from e
            if self.report and not self.report_emitted:
                print(self.diagnostics(),flush=True)
                self.report_emitted=True
        finally:
            for n in self.nodes.values(): n.stop()
            self.broker.close()

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
