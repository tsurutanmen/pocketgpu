"""PC の中に OpenAI と同じ形の窓口を立てて、中身を USB の iPhone の Apple Intelligence（Foundation Models）にする。
OpenAI の API につなぐ形の Windows のアプリなら、つなぎ先を http://127.0.0.1:8099/v1 にするだけで使える。
この PC の中（127.0.0.1）だけで待つ。外からはつながらない。

  python apple_openai.py              # 127.0.0.1:8099 で待つ
  python apple_openai.py --port 8100

  使える口：GET /v1/models・POST /v1/chat/completions（stream にも形だけ合わせる。中身は最後にまとめて 1 回で返す）
  会話は覚えない。毎回、それまでのやりとりを 1 つの頼みにまとめて送る。system の役の文は「指示」として渡す
"""
import argparse
import json
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from apple_ocr import call, connect

MODEL = "apple-foundation-iphone"
ADDR = None  # 試験用：USB を使わずにこの番地へ（--addr）


def to_request(body):
    msgs = body.get("messages", [])
    sys_parts = [m.get("content", "") for m in msgs if m.get("role") == "system"]
    turns = [m for m in msgs if m.get("role") in ("user", "assistant")]
    def text(c):
        if isinstance(c, list):
            return "".join(p.get("text", "") for p in c if p.get("type") == "text")
        return c or ""
    if len(turns) == 1:
        prompt = text(turns[0].get("content"))
    else:
        lines = [("ユーザー：" if m["role"] == "user" else "アシスタント：") + text(m.get("content")) for m in turns]
        prompt = "\n".join(lines) + "\nアシスタント："
    req = {"op": "llm", "prompt": prompt}
    if sys_parts:
        req["instructions"] = "\n".join(text(p) for p in sys_parts)
    if body.get("temperature") is not None:
        req["temperature"] = float(body["temperature"])
    mt = body.get("max_tokens") or body.get("max_completion_tokens")
    if mt:
        req["max_tokens"] = int(mt)
    return req


class Handler(BaseHTTPRequestHandler):
    def _json(self, code, obj):
        b = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        if self.path.rstrip("/") in ("/v1/models", "/models"):
            self._json(200, {"object": "list", "data": [{"id": MODEL, "object": "model", "owned_by": "apple"}]})
        else:
            self._json(404, {"error": {"message": "無い口"}})

    def do_POST(self):
        if self.path.rstrip("/") not in ("/v1/chat/completions", "/chat/completions"):
            self._json(404, {"error": {"message": "無い口"}})
            return
        n = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(n) or b"{}")
        try:
            with connect(ADDR) as s:
                res = call(s, to_request(body))
        except Exception as e:
            self._json(502, {"error": {"message": f"iPhone につながらない：{e}"}})
            return
        if "error" in res:
            self._json(503, {"error": {"message": res["error"]}})
            return
        cid = "chatcmpl-" + uuid.uuid4().hex[:24]
        now = int(time.time())
        if body.get("stream"):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            for delta, fin in (({"role": "assistant", "content": res["text"]}, None), ({}, "stop")):
                chunk = {"id": cid, "object": "chat.completion.chunk", "created": now, "model": MODEL,
                         "choices": [{"index": 0, "delta": delta, "finish_reason": fin}]}
                self.wfile.write(b"data: " + json.dumps(chunk, ensure_ascii=False).encode() + b"\n\n")
            self.wfile.write(b"data: [DONE]\n\n")
            return
        self._json(200, {"id": cid, "object": "chat.completion", "created": now, "model": MODEL,
                         "choices": [{"index": 0, "message": {"role": "assistant", "content": res["text"]}, "finish_reason": "stop"}],
                         "usage": {"prompt_tokens": 0, "completion_tokens": 0, "total_tokens": 0}})
        print(f"答えた：{res.get('ms')} ms", flush=True)

    def log_message(self, *a):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("--addr", help="試験用：USB を使わずに、この番地の受け口へ")
    a = ap.parse_args()
    global ADDR
    ADDR = a.addr
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print(f"http://127.0.0.1:{a.port}/v1 で待っている（中身は USB の iPhone の Apple Intelligence）", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
