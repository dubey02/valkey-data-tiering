#!/usr/bin/env python3
"""Touch every key in [1..N] with GET across C connections.

STRICT REQUEST-RESPONSE: each connection sends one GET, reads the complete
reply, then sends the next. NO PIPELINING. Concurrency comes only from the
number of connections (one process per connection). Prints:
    <elapsed_seconds> <replies> <nils>
"""
import socket, sys, time, multiprocessing as mp


def worker(port, lo, hi, q):
    s = socket.create_connection(('127.0.0.1', port))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    wf = s.makefile('wb', buffering=0)
    rf = s.makefile('rb', buffering=1 << 16)
    nils = 0
    got = 0
    readline = rf.readline
    read = rf.read
    write = wf.write
    for i in range(lo, hi + 1):
        write(b'GET key:%d\r\n' % i)
        line = readline()
        if not line:
            raise RuntimeError('connection closed after %d replies' % got)
        c = line[0:1]
        if c == b'$':
            l = int(line[1:-2])
            if l == -1:
                nils += 1
            else:
                body = read(l + 2)
                if len(body) != l + 2:
                    raise RuntimeError('short read at reply %d' % got)
        elif c == b'-':
            raise RuntimeError('server error: %r' % line)
        else:
            raise RuntimeError('unexpected reply: %r' % line[:80])
        got += 1
    q.put((got, nils))


if __name__ == '__main__':
    port, n, conns = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
    per = (n + conns - 1) // conns
    q = mp.Queue()
    procs = []
    t0 = time.time()
    for k in range(conns):
        lo, hi = k * per + 1, min((k + 1) * per, n)
        if lo > hi:
            continue
        p = mp.Process(target=worker, args=(port, lo, hi, q))
        p.start()
        procs.append(p)
    tot = nils = 0
    for _ in procs:
        g, nl = q.get()
        tot += g
        nils += nl
    for p in procs:
        p.join()
    print('%.3f %d %d' % (time.time() - t0, tot, nils))
