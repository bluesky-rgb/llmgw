# D3：客户端读到第一块后断开，测量 mock 多久后观察到上游请求被取消
import json, socket, sys, time, urllib.request
port, model, auth, runs = int(sys.argv[1]), sys.argv[2], sys.argv[3], int(sys.argv[4])
until = (sys.argv[5] if len(sys.argv) > 5 else "data:").encode()
def cancelled():
    return json.load(urllib.request.urlopen("http://127.0.0.1:18081/_mock/stats"))["streams_cancelled"]
res = []
for _ in range(runs):
    before = cancelled()
    s = socket.create_connection(("127.0.0.1", port))
    body = json.dumps({"model": model, "stream": True, "messages": [{"role": "user", "content": "x"}]})
    hdr = "POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
    if auth: hdr += "Authorization: Bearer %s\r\n" % auth
    s.sendall((hdr + "Content-Length: %d\r\n\r\n%s" % (len(body), body)).encode())
    buf = b""
    while until not in buf: buf += s.recv(4096)
    s.close(); t0 = time.perf_counter()
    while cancelled() == before:
        if time.perf_counter() - t0 > 30: break
        time.sleep(0.002)
    res.append((time.perf_counter() - t0) * 1000)
    time.sleep(0.05)
res.sort()
print("n=%d 中位数=%.1fms 最大=%.1fms" % (len(res), res[len(res)//2], res[-1]))
