#!/usr/bin/env python3
"""Development-only TCP relay for two PCSX2 instances using Sockets + DHCP.
Run: python3 tools/pcsx2_tcp_relay.py --host 0.0.0.0 --port 39513
Only use on a trusted LAN. No authentication or encryption.
"""
import argparse
import selectors
import socket

def main():
    p=argparse.ArgumentParser()
    p.add_argument("--host",default="127.0.0.1")
    p.add_argument("--port",type=int,default=39513)
    args=p.parse_args()
    selector=selectors.DefaultSelector()
    listener=socket.socket(socket.AF_INET,socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
    listener.bind((args.host,args.port))
    listener.listen()
    listener.setblocking(False)
    selector.register(listener,selectors.EVENT_READ)
    clients={}
    print("Relay listening",args.host,args.port,flush=True)
    def disconnect(sock):
        entry=clients.pop(sock,None)
        if entry:
            print("Disconnected",entry["address"],flush=True)
        try: selector.unregister(sock)
        except Exception: pass
        sock.close()
    try:
        while True:
            for key,_ in selector.select(timeout=1):
                sock=key.fileobj
                if sock is listener:
                    peer,address=listener.accept()
                    peer.setblocking(False)
                    clients[peer]={"address":address,"role":None,"pending":b""}
                    selector.register(peer,selectors.EVENT_READ)
                    print("Connected",address,flush=True)
                    continue
                try: data=sock.recv(4096)
                except (ConnectionError,OSError): data=b""
                if not data:
                    disconnect(sock)
                    continue
                entry=clients[sock]
                if entry["role"] is None:
                    entry["pending"]+=data
                    if b"\\n" not in entry["pending"]:
                        if len(entry["pending"])>128: disconnect(sock)
                        continue
                    line,_,remaining=entry["pending"].partition(b"\\n")
                    role=line.strip().decode("ascii","replace")
                    if role not in ("PS2HOST/1","PS2CLIENT/1","PS2AUTO/1"):
                        print("Invalid handshake",role,flush=True)
                        disconnect(sock)
                        continue
                    entry["role"]=role
                    entry["pending"]=b""
                    print("Registered",entry["address"],role,flush=True)
                    data=remaining
                else:
                    print("Message",entry["role"],repr(data[:80]),flush=True)
                if not data: continue
                for other,peer in list(clients.items()):
                    if other is sock or peer["role"] is None: continue
                    if entry["role"]=="PS2HOST/1" and peer["role"]!="PS2CLIENT/1": continue
                    if entry["role"]=="PS2CLIENT/1" and peer["role"]!="PS2HOST/1": continue
                    try: other.sendall(data)
                    except (ConnectionError,OSError): disconnect(other)
    except KeyboardInterrupt:
        pass
    finally:
        for sock in list(clients): disconnect(sock)
        selector.unregister(listener)
        listener.close()
if __name__=="__main__":
    main()
