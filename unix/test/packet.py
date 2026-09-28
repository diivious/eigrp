# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
import socket, struct, subprocess, time
from pathlib import Path
ROOT=next(p for p in Path(__file__).resolve().parents if (p/'unix'/'code').is_dir())
BROKER=ROOT/'unix'/'code'/'eigrp-wire'
HDR='!BBBBIHHHH16s16s'
HDR_SIZE=struct.calcsize(HDR)
def send_frame(sock,data): sock.sendall(data)
def recv_frame(sock):
    header=b''
    while len(header)<HDR_SIZE:
        chunk=sock.recv(HDR_SIZE-len(header))
        if not chunk: return b''
        header+=chunk
    h=struct.unpack(HDR,header); body_len=h[5]+h[6]+h[7]; body=b''
    while len(body)<body_len:
        chunk=sock.recv(body_len-len(body))
        if not chunk: return b''
        body+=chunk
    return header+body
def addr(afi,text):
    raw=socket.inet_pton(socket.AF_INET if afi==4 else socket.AF_INET6,text)
    return raw+b'\0'*(16-len(raw))
def frame(kind,afi,idx,seg,iface,src,dst=None,payload=b''):
    sb,ib=seg.encode(),iface.encode(); flags=1 if dst and dst[0]==0xff or (dst and afi==4 and dst[0]&0xf0==0xe0) else 0
    return struct.pack(HDR,1,kind,afi,flags,idx,len(sb),len(ib),len(payload),0,src,dst or bytes(16))+sb+ib+payload
def register(s,afi,idx,name,address): send_frame(s,frame(1,afi,idx,'lan0',name,addr(afi,address)))
def packet(s,afi,idx,name,source,dest,payload): send_frame(s,frame(2,afi,idx,'lan0',name,addr(afi,source),addr(afi,dest),payload))
def payload_of(data):
    h=struct.unpack(HDR,data[:struct.calcsize(HDR)]); off=struct.calcsize(HDR)+h[5]+h[6]; return h,data[off:]
def test_wire_multicast_unicast_dual_stack(tmp_path):
    path=tmp_path/'wire.sock'; proc=subprocess.Popen([str(BROKER),str(path)])
    try:
        for _ in range(50):
            if path.exists(): break
            time.sleep(.02)
        r1=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);r2=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);r3=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
        for s in (r1,r2,r3): s.settimeout(.15);s.connect(str(path))
        for s,idx,n,v4,v6 in ((r1,1,'eth0','10.0.0.1','fe80::1'),(r2,2,'eth0','10.0.0.2','fe80::2'),(r3,3,'eth0','10.0.0.3','fe80::3')):
            register(s,4,idx,n,v4);register(s,6,idx,n,v6)
        time.sleep(.03)
        p=b'\x02\x05EIGRP-v4-multicast';packet(r1,4,1,'eth0','10.0.0.1','224.0.0.10',p)
        assert payload_of(recv_frame(r2))[1]==p;assert payload_of(recv_frame(r3))[1]==p
        p=b'\x02\x05EIGRP-v6-multicast';packet(r1,6,1,'eth0','fe80::1','ff02::a',p)
        assert payload_of(recv_frame(r2))[1]==p;assert payload_of(recv_frame(r3))[1]==p
        p=b'\x02\x01EIGRP-v4-unicast';packet(r1,4,1,'eth0','10.0.0.1','10.0.0.2',p);h,got=payload_of(recv_frame(r2));assert got==p and h[4]==2
        try:r3.recv(4096);assert False,'R3 received IPv4 unicast for R2'
        except TimeoutError:pass
        p=b'\x02\x01EIGRP-v6-unicast';packet(r1,6,1,'eth0','fe80::1','fe80::2',p);h,got=payload_of(recv_frame(r2));assert got==p and h[4]==2
        try:r3.recv(4096);assert False,'R3 received IPv6 unicast for R2'
        except TimeoutError:pass
    finally:
        proc.terminate();proc.wait(timeout=2)
