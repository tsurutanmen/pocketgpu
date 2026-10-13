"""試験用の更新サーバー。「正しいキット」のフォルダを配る。PC の中（127.0.0.1）だけで動く。

  python fake_update_server.py --port 28080 --root <正しいキット> --token <合言葉>
update.ps1 が聞く /u/<合言葉>/update.json に、フォルダの全ファイルの一覧（大きさ・SHA-256・URL）を返し、/f/<パス> でファイルを返す。
tools/update.ini は人ごとの物なので、一覧に入れない（update.ps1 は一覧にあると断る）。
"""
import argparse
import hashlib
import http.server
import json
import os
import socketserver

SKIP_TOP = {'update-backup', 'fix-backup'}
SKIP_FILES = {'tools/update.ini', 'pocketgpu-start-log.txt', 'pocketgpu-update-log.txt', 'pocketgpu-fix-log.txt', 'pocketgpu-fix-summary.txt'}


def build(root, port):
    files = []
    for d, dirs, names in os.walk(root):
        rel_dir = os.path.relpath(d, root).replace('\\', '/')
        if rel_dir == '.':
            dirs[:] = [x for x in dirs if x not in SKIP_TOP]
        for n in names:
            rel = n if rel_dir == '.' else rel_dir + '/' + n
            if rel in SKIP_FILES:
                continue
            full = os.path.join(d, n)
            data = open(full, 'rb').read()
            files.append({'path': rel, 'size': len(data), 'sha': hashlib.sha256(data).hexdigest(), 'url': 'http://127.0.0.1:%d/f/%s' % (port, rel)})
    return {'version': '9.9.9', 'rel': 'test', 'mark': 'T' * 32, 'notes': {'ja': '試験', 'en': 'test'}, 'files': files, 'remove': [], 'apps': []}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--root', required=True)
    ap.add_argument('--token', required=True)
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    manifest = json.dumps(build(root, a.port)).encode()

    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            if self.path == '/u/%s/update.json' % a.token:
                body, ctype = manifest, 'application/json'
            elif self.path.startswith('/f/'):
                rel = self.path[3:]
                full = os.path.abspath(os.path.join(root, rel))
                if not full.startswith(root + os.sep) or not os.path.isfile(full):
                    self.send_error(404)
                    return
                body, ctype = open(full, 'rb').read(), 'application/octet-stream'
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header('Content-Type', ctype)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
        daemon_threads = True

    S(('127.0.0.1', a.port), H).serve_forever()


if __name__ == '__main__':
    main()
