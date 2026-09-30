import socket, concurrent.futures, ipaddress, sys
ports=[554,8554,8080,8000,37777,34567,8899,80]
def chk(ip,port,to=0.7):
    s=socket.socket(); s.settimeout(to)
    try:
        s.connect((str(ip),port)); s.close(); return True
    except Exception: return False
def sweep(net, ports):
    ips=[str(ip) for ip in ipaddress.ip_network(net,False).hosts()]
    hits=[]
    with concurrent.futures.ThreadPoolExecutor(max_workers=300) as ex:
        futs={ex.submit(chk,ip,p):(ip,p) for ip in ips for p in ports}
        for f in concurrent.futures.as_completed(futs):
            ip,p=futs[f]
            try:
                if f.result(): hits.append((ip,p))
            except Exception: pass
    return hits
for net in sys.argv[1:]:
    h=sweep(net,ports)
    cams=[x for x in h if x[1] in (554,8554,37777,34567,8899,8000)]
    print(f"[{net}] 开放端口 {len(h)} 条；其中摄像头常用口 {len(cams)} 条")
    for ip,p in sorted(h): print("   ",ip,p, "  <== 摄像头口" if p in (554,8554,37777,34567,8899,8000) else "")
