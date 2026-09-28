# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
import socket, struct, time
from pathlib import Path
from eigrp_wire import Broker, HDR

def _addr(text): return socket.inet_pton(socket.AF_INET,text).ljust(16,b'\0')
def _frame(kind,src,dst,payload=b'',flags=0):
    seg=b'lan'; iface=b'eth0'; return HDR.pack(1,kind,4,flags,1,len(seg),len(iface),len(payload),0,_addr(src),_addr(dst))+seg+iface+payload
def _eigrp(opcode=1,seq=7,ack=0): return struct.pack('!BBHIIII',2,opcode,0,0,seq,ack,4453)
def _connect(path):
    s=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET);s.settimeout(.3);s.connect(str(path));return s
def _register(s,address): s.send(_frame(1,address,'0.0.0.0'))

def test_fault_actions_match_delivery_metadata(tmp_path):
    path=tmp_path/'wire.sock'; owners={(4,_addr('10.0.0.1')[:4]):'r1',(4,_addr('10.0.0.2')[:4]):'r2'}; b=Broker(str(path),owners);b.start();assert b.ready.wait(1)
    a=_connect(path); c=_connect(path); _register(a,'10.0.0.1');_register(c,'10.0.0.2');time.sleep(.05)
    try:
        b.add_fault({'name':'drop','action':'drop','source_uut':'r1','destination_uut':'r2','source_interface':'eth0','destination_interface':'eth0','segment':'lan','afi':4,'multicast':False,'opcode':'update','count':1})
        a.send(_frame(2,'10.0.0.1','10.0.0.2',_eigrp()))
        try: c.recv(4096); assert False,'drop fault delivered packet'
        except TimeoutError: pass
        assert b.fault_state('drop')['hits']==1
        b.add_fault({'name':'dup','action':'duplicate','source_uut':'r1','destination_uut':'r2','opcode':'update','copies':1,'count':1})
        a.send(_frame(2,'10.0.0.1','10.0.0.2',_eigrp(seq=8))); assert c.recv(4096); assert c.recv(4096)
        b.add_fault({'name':'delay','action':'delay','source_uut':'r1','destination_uut':'r2','opcode':'update','delay_ms':120,'count':1})
        t=time.monotonic();a.send(_frame(2,'10.0.0.1','10.0.0.2',_eigrp(seq=9)));assert c.recv(4096);assert time.monotonic()-t>=.10
        b.add_fault({'name':'disc','action':'disconnect','source_uut':'r1','destination_uut':'r2','opcode':'update','count':1})
        a.send(_frame(2,'10.0.0.1','10.0.0.2',_eigrp(seq=10)))
        try:c.recv(4096);assert False,'disconnected endpoint received packet'
        except TimeoutError:pass
        b.add_fault({'name':'reconn','action':'reconnect','source_uut':'r1','destination_uut':'r2','opcode':'update','count':1})
        a.send(_frame(2,'10.0.0.1','10.0.0.2',_eigrp(seq=11)));assert c.recv(4096)
    finally: a.close();c.close();b.close()
