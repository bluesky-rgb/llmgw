# D3（首字前、响应头前断开）：发完请求 200ms 后断开，测量 mock 多久后统计到取消
import json, socket, sys, time, urllib.request
port = int(sys.argv[1])
def c(): return json.load(urllib.request.urlopen("http://127.0.0.1:18081/_mock/stats"))["streams_cancelled"]
res=[]
for _ in range(10):
    b=c(); s=socket.create_connection(("127.0.0.1",port))
    body='{"model":"mock:header_delay=60s","stream":true}'
    s.sendall(("POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n%s"%(len(body),body)).encode())
    time.sleep(0.2); s.close(); t0=time.perf_counter()
    while c()==b and time.perf_counter()-t0<30: time.sleep(0.002)
    res.append((time.perf_counter()-t0)*1000)
res.sort(); print("n=%d 中位数=%.1fms 最大=%.1fms"%(len(res),res[len(res)//2],res[-1]))
