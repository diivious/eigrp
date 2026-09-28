# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
import socket, struct, subprocess, time
from pathlib import Path
ROOT=next(p for p in Path(__file__).resolve().parents if (p/'unix'/'code').is_dir())
BROKER=ROOT/'unix'/'code'/'eigrp-wire'
HDR='!BBBBIHHHH16s16s'
def addr(afi,text):
    raw=socket.inet_pton(socket.AF_INET if afi==4 else socket.AF_INET6,text)
    return raw+b'\0'*(16-len(raw))
def frame(kind,afi,idx,seg,iface,src,dst=None,payload=b''):
    sb,ib=seg.encode(),iface.encode(); flags=1 if dst and dst[0]==0xff or (dst and afi==4 and dst[0]&0xf0==0xe0) else 0
    return struct.pack(HDR,1,kind,afi,flags,idx,len(sb),len(ib),len(payload),0,src,dst or bytes(16))+sb+ib+payload
def register(s,afi,idx,name,address): s.send(frame(1,afi,idx,'lan0',name,addr(afi,address)))
def packet(s,afi,idx,name,source,dest,payload): s.send(frame(2,afi,idx,'lan0',name,addr(afi,source),addr(afi,dest),payload))
def payload_of(data):
    h=struct.unpack(HDR,data[:struct.calcsize(HDR)]); off=struct.calcsize(HDR)+h[5]+h[6]; return h,data[off:]
def test_wire_multicast_unicast_dual_stack(tmp_path):
    path=tmp_path/'wire.sock'; proc=subprocess.Popen([str(BROKER),str(path)])
    try:
        for _ in range(50):
            if path.exists(): break
            time.sleep(.02)
        r1=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET);r2=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET);r3=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET)
        for s in (r1,r2,r3): s.settimeout(.15);s.connect(str(path))
        for s,idx,n,v4,v6 in ((r1,1,'eth0','10.0.0.1','fe80::1'),(r2,2,'eth0','10.0.0.2','fe80::2'),(r3,3,'eth0','10.0.0.3','fe80::3')):
            register(s,4,idx,n,v4);register(s,6,idx,n,v6)
        time.sleep(.03)
        p=b'\x02\x05EIGRP-v4-multicast';packet(r1,4,1,'eth0','10.0.0.1','224.0.0.10',p)
        assert payload_of(r2.recv(4096))[1]==p;assert payload_of(r3.recv(4096))[1]==p
        p=b'\x02\x05EIGRP-v6-multicast';packet(r1,6,1,'eth0','fe80::1','ff02::a',p)
        assert payload_of(r2.recv(4096))[1]==p;assert payload_of(r3.recv(4096))[1]==p
        p=b'\x02\x01EIGRP-v4-unicast';packet(r1,4,1,'eth0','10.0.0.1','10.0.0.2',p);h,got=payload_of(r2.recv(4096));assert got==p and h[4]==2
        try:r3.recv(4096);assert False,'R3 received IPv4 unicast for R2'
        except TimeoutError:pass
        p=b'\x02\x01EIGRP-v6-unicast';packet(r1,6,1,'eth0','fe80::1','fe80::2',p);h,got=payload_of(r2.recv(4096));assert got==p and h[4]==2
        try:r3.recv(4096);assert False,'R3 received IPv6 unicast for R2'
        except TimeoutError:pass
    finally:
        proc.terminate();proc.wait(timeout=2)
