# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Local adversarial HTTP fixture for the actual Windows.Web.Http downloader."""
import hashlib, http.server, threading, subprocess, tempfile, pathlib, sys
payload=bytes(range(256))*8192
sha=hashlib.sha256(payload).hexdigest()
requests=[]
cut_once=True
class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def do_GET(self):
        global cut_once
        mode=self.path[1:];start=0
        requests.append((mode,self.headers.get('Range'),self.headers.get('If-Range')))
        if mode in ('redirect','cycle','foreign'):
            self.send_response(302);self.send_header('Location','/valid' if mode=='redirect' else '/cycle' if mode=='cycle' else 'http://attacker.invalid/');self.end_headers();return
        if mode=='cache': self.send_response(304);self.end_headers();return
        if self.headers.get('Range'):
            assert self.headers.get('If-Range')=='"fixture"'
            start=int(self.headers['Range'][6:-1])
        self.send_response(206 if start and mode!='ignore' else 200)
        if mode=='ignore': start=0
        self.send_header('ETag','"fixture"')
        if start: self.send_header('Content-Range',f'bytes {start+int(mode=="badrange")}-{len(payload)-1}/{len(payload)}')
        if mode!='fetch': self.send_header('Content-Length',str(len(payload)-start+int(mode=='oversized')))
        self.end_headers()
        try:
            if mode=='cut' and cut_once:
                cut_once=False;self.wfile.write(payload[start:start+262144]);self.wfile.flush();self.close_connection=True
            else:self.wfile.write(payload[start:])
        except (BrokenPipeError,ConnectionResetError):pass
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
threading.Thread(target=server.serve_forever,daemon=True).start()
try:
    with tempfile.TemporaryDirectory() as directory:
        def run(mode,path='part',digest=sha,expected=''):
            subprocess.run([sys.argv[1],f'http://127.0.0.1:{server.server_port}',str(pathlib.Path(directory)/path),digest,mode,str(len(payload)),expected],check=True,timeout=45)
        run('valid');run('valid') # Already complete: hash without requesting another body.
        run('pause','paused');run('valid','paused')
        run('cut','cut',expected='cut');run('cut','cut')
        partial=pathlib.Path(directory)/'ignore';partial.write_bytes(payload[:262144])
        pathlib.Path(str(partial)+'.identity').write_text(f'http://127.0.0.1:{server.server_port}/ignore\n'+sha+'\n'+str(len(payload)),newline='')
        pathlib.Path(str(partial)+'.validator').write_text('"fixture"')
        run('redirect','redirect');run('cycle','cycle');run('foreign','foreign');run('ignore','ignore');run('wronghash','bad','0'*64);run('oversized','large');run('fetch','fetch')
        # Bind a partial to the exact URL before injecting a wrong Content-Range.
        bad=pathlib.Path(directory)/'range';url=f'http://127.0.0.1:{server.server_port}/badrange'
        bad.write_bytes(payload[:262144]);pathlib.Path(str(bad)+'.identity').write_text(url+'\n'+sha+'\n'+str(len(payload)),newline='');pathlib.Path(str(bad)+'.validator').write_text('"fixture"')
        run('badrange','range')
        assert any(mode=='cut' and header is not None for mode,header,_ in requests)
        assert not (pathlib.Path(directory)/'bad').exists()
    print('HTTP fixture: resume, cancellation, disconnect, exact size, bad range, hash and bounded GET passed')
finally:server.shutdown();server.server_close()
