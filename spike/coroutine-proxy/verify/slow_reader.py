# 慢速客户端：小接收缓冲区，固定速率读取，用来观察代理的背压
import socket, sys, time
host, port, model, rate_kb, secs = sys.argv[1], int(sys.argv[2]), sys.argv[3], int(sys.argv[4]), float(sys.argv[5])
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 16384); s.connect((host, port))
body = '{"model":"%s","stream":true}' % model
s.sendall(("POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s" % (len(body), body)).encode())
got, t0, step = 0, time.time(), 0.05
per = rate_kb * 1024 * step
while time.time() - t0 < secs:
    want = per
    while want > 0:
        d = s.recv(int(min(want, 4096)))
        if not d: print("EOF", got); sys.exit()
        got += len(d); want -= len(d)
    time.sleep(step)
print("received", got)
