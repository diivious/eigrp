# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
"""Fault-capable Unix virtual wire used by portable EIGRP UUT qualification."""
from __future__ import annotations
import collections, heapq, os, selectors, socket, struct, threading, time

HDR=struct.Struct("!BBBBIHHHH16s16s"); REGISTER=1; PACKET=2; MULTICAST=1; CR_FLAG=0x02
OPCODES={1:"update",2:"request",3:"query",4:"reply",5:"hello",6:"ipxsap",7:"probe",8:"ack",10:"sia-query",11:"sia-reply"}

def send_frame(sock, frame):
    sock.sendall(frame)

def recv_frame(sock):
    header = bytearray()
    while len(header) < HDR.size:
        chunk = sock.recv(HDR.size - len(header))
        if not chunk:
            return b""
        header.extend(chunk)
    _, _, _, _, _, sl, il, pl, _, _, _ = HDR.unpack(header)
    body_len = sl + il + pl
    body = bytearray()
    while len(body) < body_len:
        chunk = sock.recv(body_len - len(body))
        if not chunk:
            return b""
        body.extend(chunk)
    return bytes(header + body)

class WireFaultError(ValueError): pass

def _packet_meta(payload):
    opcode=payload[1] if len(payload)>1 else -1
    flags=struct.unpack_from("!I",payload,4)[0] if len(payload)>=16 else 0
    sequence=struct.unpack_from("!I",payload,8)[0] if len(payload)>=16 else 0
    ack=struct.unpack_from("!I",payload,12)[0] if len(payload)>=16 else 0
    return {"opcode":"ack" if opcode==5 and ack else OPCODES.get(opcode,str(opcode)),
            "sequence":sequence,"ack":ack,"cr":bool(flags&CR_FLAG)}

class Broker(threading.Thread):
    """Shared-segment delivery broker. Faults affect delivery only, never EIGRP state."""
    def __init__(self,path,address_owner):
        super().__init__(daemon=True); self.path=path; self.address_owner=address_owner
        self.stop_event=threading.Event(); self.ready=threading.Event(); self.journal=collections.deque(maxlen=2048); self.error=None
        self._lock=threading.Lock(); self._faults={}; self._next_fault=1; self._delayed=[]; self._delay_serial=0; self._disconnected=set()
    def add_fault(self,spec):
        if not isinstance(spec,dict): raise WireFaultError("fault must be a mapping")
        allowed={"name","action","source_uut","destination_uut","source_interface","destination_interface","segment","afi","multicast","opcode","sequence","ack","cr","delay_ms","copies","count"}
        extra=set(spec)-allowed
        if extra: raise WireFaultError(f"unsupported fault keys: {', '.join(sorted(extra))}")
        action=str(spec.get("action","")).lower()
        if action not in {"drop","delay","duplicate","disconnect","reconnect"}: raise WireFaultError(f"unsupported fault action {action}")
        if action=="delay" and float(spec.get("delay_ms",0))<0: raise WireFaultError("delay_ms must be >= 0")
        if action=="duplicate" and int(spec.get("copies",1))<1: raise WireFaultError("copies must be >= 1")
        if "count" in spec and int(spec["count"])<1: raise WireFaultError("count must be >= 1")
        with self._lock:
            fid=str(spec.get("name") or f"fault-{self._next_fault}"); self._next_fault+=1
            if fid in self._faults: raise WireFaultError(f"fault {fid} already exists")
            self._faults[fid]={**spec,"name":fid,"action":action,"hits":0,"remaining":int(spec["count"]) if "count" in spec else None}
        return fid
    def remove_fault(self,name):
        with self._lock: return self._faults.pop(str(name),None) is not None
    def fault_state(self,name):
        with self._lock:
            f=self._faults.get(str(name)); return dict(f) if f else None
    def _matches(self,f,m):
        for k in ("source_uut","destination_uut","source_interface","destination_interface","segment","afi","multicast","opcode","sequence","ack","cr"):
            if k in f and f[k] != m.get(k): return False
        return True
    def _actions(self,meta):
        actions=[]
        with self._lock:
            for f in self._faults.values():
                if f["remaining"]==0 or not self._matches(f,meta): continue
                f["hits"]+=1
                if f["remaining"] is not None: f["remaining"]-=1
                actions.append(dict(f))
        return actions
    def _send(self,endpoint,frame,meta,event="delivered"):
        c,_,eif,_,_,_,_=endpoint; out=bytearray(frame); struct.pack_into("!I",out,4,eif)
        try: send_frame(c,out); self.journal.append({**meta,"event":event})
        except OSError: self.journal.append({**meta,"event":"send-error"})
    def _queue_delay(self,when,endpoint,frame,meta):
        self._delay_serial+=1; heapq.heappush(self._delayed,(when,self._delay_serial,endpoint,bytes(frame),dict(meta)))
    def _flush_delayed(self):
        now=time.monotonic()
        while self._delayed and self._delayed[0][0]<=now:
            _,_,endpoint,frame,meta=heapq.heappop(self._delayed)
            key=(meta["destination_uut"],meta["destination_interface"])
            if key in self._disconnected: self.journal.append({**meta,"event":"disconnected"})
            else: self._send(endpoint,frame,meta,"delayed")
    def run(self):
        try:
            try: os.unlink(self.path)
            except FileNotFoundError: pass
            ls=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); ls.bind(self.path); ls.listen(); ls.setblocking(False)
            sel=selectors.DefaultSelector(); sel.register(ls,selectors.EVENT_READ,("listen",None)); endpoints=[]; self.ready.set()
            while not self.stop_event.is_set():
                self._flush_delayed()
                for key,_ in sel.select(.02):
                    if key.data[0]=="listen":
                        c,_=ls.accept(); c.setblocking(True); sel.register(c,selectors.EVENT_READ,("client",c)); continue
                    c=key.data[1]
                    try: frame=recv_frame(c)
                    except OSError: frame=b""
                    if not frame:
                        endpoints[:]=[e for e in endpoints if e[0] is not c]
                        try: sel.unregister(c); c.close()
                        except Exception: pass
                        continue
                    if len(frame)<HDR.size: continue
                    version,typ,afi,flags,ifindex,sl,il,pl,res,src,dst=HDR.unpack_from(frame)
                    if version!=1 or HDR.size+sl+il+pl!=len(frame): continue
                    seg=frame[HDR.size:HDR.size+sl].decode(); ifname=frame[HDR.size+sl:HDR.size+sl+il].decode(); payload=frame[HDR.size+sl+il:]
                    src_key=(afi,src[:4] if afi==4 else src); owner=self.address_owner.get(src_key,"?")
                    if typ==REGISTER:
                        item=(c,afi,ifindex,seg,ifname,src,owner)
                        if not any(e[:6]==item[:6] for e in endpoints): endpoints.append(item)
                        continue
                    if typ!=PACKET: continue
                    pm=_packet_meta(payload); mc=bool(flags&MULTICAST)
                    sent=set(); destinations=[]
                    for e in list(endpoints):
                        ec,eafi,eif,eseg,eifname,eaddr,eowner=e
                        if ec is c or eafi!=afi or eseg!=seg: continue
                        if not mc and eaddr[:4 if afi==4 else 16]!=dst[:4 if afi==4 else 16]: continue
                        if mc and (id(ec),eif) in sent: continue
                        sent.add((id(ec),eif)); destinations.append(e)
                    base={"time":time.monotonic(),"uut":owner,"source_uut":owner,"interface":ifname,"source_interface":ifname,"segment":seg,"afi":afi,"opcode":pm["opcode"],"length":len(payload),"multicast":mc,"sequence":pm["sequence"],"ack":pm["ack"],"cr":pm["cr"]}
                    self.journal.append({**base,"destination_uut":None,"destination_interface":None,"event":"sent"})
                    for e in destinations:
                        meta={**base,"destination_uut":e[6],"destination_interface":e[4]}; actions=self._actions(meta)
                        copies=1; delay=0.0; drop=False
                        for f in actions:
                            self.journal.append({**meta,"event":"fault","fault":f["name"],"action":f["action"]})
                            if f["action"]=="drop": drop=True
                            elif f["action"]=="delay": delay=max(delay,float(f.get("delay_ms",0))/1000.0)
                            elif f["action"]=="duplicate": copies+=int(f.get("copies",1))
                            elif f["action"]=="disconnect": self._disconnected.add((e[6],e[4])); drop=True
                            elif f["action"]=="reconnect": self._disconnected.discard((e[6],e[4]))
                        if (e[6],e[4]) in self._disconnected: drop=True
                        if drop: self.journal.append({**meta,"event":"dropped"}); continue
                        for copy in range(copies):
                            cm={**meta,"copy":copy}
                            if delay: self._queue_delay(time.monotonic()+delay,e,frame,cm)
                            else: self._send(e,frame,cm,"duplicated" if copy else "delivered")
            self._flush_delayed()
            for key in list(sel.get_map().values()):
                try: key.fileobj.close()
                except Exception: pass
            sel.close()
        except Exception as e: self.error=e; self.ready.set()
        finally:
            try: os.unlink(self.path)
            except FileNotFoundError: pass
    def close(self): self.stop_event.set(); self.join(2)
