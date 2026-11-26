"""Controllable local stand-in for the board's SSE slots (tests of e1_sse_diag only).
Not a firmware model: every behaviour is a switch, so each diagnostic outcome can be forced."""
import json, socket, threading, time


class FakeSseBoard:
    def __init__(self, slots=3, release='prompt', release_delay=0.0, leak=0, period=0.1, accept_delay=0.0,
                 fragment=False, drop_slow_after=1.0, drop_slow=True, pad=20000, boot='b' * 32):
        self.slots, self.release, self.release_delay, self.leak = slots, release, release_delay, leak
        self.period, self.accept_delay, self.fragment = period, accept_delay, fragment
        self.drop_slow_after, self.drop_slow, self.pad, self.boot = drop_slow_after, drop_slow, pad, boot
        self.lock = threading.Lock()
        self.occupied, self.closes, self.seq, self.running = 0, [], 0, True
        self.socks = set()
        self.srv = socket.socket(); self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(('127.0.0.1', 0)); self.srv.listen(32); self.port = self.srv.getsockname()[1]
        threading.Thread(target=self._accept, daemon=True).start()

    def stop(self):
        self.running = False
        try: self.srv.close()
        except OSError: pass
        for c in list(self.socks):
            try: c.close()
            except OSError: pass

    def _accept(self):
        while self.running:
            try: c, _ = self.srv.accept()
            except OSError: return
            self.socks.add(c)
            threading.Thread(target=self._serve, args=(c,), daemon=True).start()

    def _send(self, c, data):
        if self.fragment and len(data) > 8:
            for i in range(0, len(data), max(1, len(data) // 3)):
                c.sendall(data[i:i + max(1, len(data) // 3)]); time.sleep(0.003)
        else:
            c.sendall(data)

    def _serve(self, c):
        try:
            if self.accept_delay: time.sleep(self.accept_delay)
            req = b''
            while b'\r\n\r\n' not in req:
                chunk = c.recv(4096)
                if not chunk: return
                req += chunk
            path = req.split(b' ', 2)[1].decode()
            if path.startswith('/api/info'): return self._json(c, {'bootId': self.boot, 'firmware': 'fake'})
            if path.startswith('/api/session'): return self._json(c, {'token': 't'})
            if not path.startswith('/api/events'): return self._json(c, {'error': 'nf'}, 404)
            with self.lock:
                full = self.occupied >= self.slots
                if not full: self.occupied += 1
            if full: return self._json(c, {'error': 'Three status viewers already connected'}, 503)
            self._stream(c, v1='v=1' in path)
        except OSError:
            pass
        finally:
            try: c.close()
            except OSError: pass
            self.socks.discard(c)

    def _json(self, c, body, code=200):
        data = json.dumps(body).encode()
        self._send(c, f'HTTP/1.1 {code} X\r\nContent-Type: application/json\r\nContent-Length: {len(data)}\r\nConnection: close\r\n\r\n'.encode() + data)

    def _free(self, how):
        self.closes.append(how)
        if self.leak > 0:
            self.leak -= 1; return  # this slot is never freed
        def go():
            with self.lock: self.occupied -= 1
        if self.release_delay: threading.Timer(self.release_delay, go).start()
        else: go()

    def _stream(self, c, v1):
        c.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4096)  # small like the ESP32 lwIP buffer
        self._send(c, b'HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\nretry: 3000\n\n')
        c.setblocking(False)
        pending, since = b'', None
        while self.running:
            if self.release == 'prompt':
                try:
                    d = c.recv(1, socket.MSG_PEEK)
                    if d == b'': return self._free('fin')
                    c.recv(4096)
                except (BlockingIOError, InterruptedError): pass
                except ConnectionResetError: return self._free('rst')
                except OSError: return self._free('error')
            if not pending:
                self.seq += 1
                ev = {'v': 1, 'bootId': self.boot, 'seq': self.seq, 'status': {'pad': 'x' * self.pad}} if v1 else {'configRevision': 1, 'pad': 'x' * self.pad}
                pending, since = f'event: status\ndata: {json.dumps(ev)}\n\n'.encode(), time.monotonic()
            # Like the firmware (hub.cpp): a pending write older than the limit drops the client.
            if self.drop_slow and pending and time.monotonic() - since > self.drop_slow_after: return self._free('slow-drop')
            try:
                n = c.send(pending); pending = pending[n:]
            except (BlockingIOError, InterruptedError):
                pass
            except (BrokenPipeError, ConnectionResetError, OSError):
                return self._free('write-error')
            if not pending: time.sleep(self.period)
            else: time.sleep(0.002)
