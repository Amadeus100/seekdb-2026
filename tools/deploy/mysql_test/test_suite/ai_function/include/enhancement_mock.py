#!/usr/bin/env python3
"""Deterministic protocol/fault fixture for the AI enhancement SQL tests."""
import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

events = []
lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def send_json(self, value, status=200):
        data = json.dumps(value, ensure_ascii=False, allow_nan=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        with lock:
            self.send_json(list(events))

    def do_DELETE(self):
        with lock:
            events.clear()
        self.send_json({})

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        if length > 8 * 1024 * 1024:
            self.send_json({"error": "request too large"}, 413)
            return
        body = json.loads(self.rfile.read(length))
        with lock:
            events.append({"path": self.path, "body": body})
        if "/fail/" in self.path:
            self.send_json({"error": "injected service failure"}, 503)
        elif self.path.endswith("/embeddings") or self.path.endswith("/embed"):
            inputs = body.get("input")
            if not isinstance(inputs, list) or not all(isinstance(x, str) for x in inputs):
                self.send_json({"error": "input must be a string array"}, 400)
                return
            dimension = body.get("dimensions", 768)
            vectors = [[float(sum(x.encode("utf-8")) % 997)] + [0.25] * (dimension - 1) for x in inputs]
            if "/bad-dimension/" in self.path:
                vectors[0].append(0.5)
            if "/bad-element/" in self.path:
                vectors[0][0] = "not a number"
            if self.path.endswith("/embed"):
                self.send_json({"embeddings": vectors})
            else:
                data = [{"index": i, "embedding": x} for i, x in enumerate(vectors)]
                if "/duplicate-index/" in self.path and len(data) > 1:
                    data[1]["index"] = 0
                self.send_json({"data": data[::-1]})
        elif self.path.endswith("/rerank"):
            query = body["query"]
            documents = body["documents"]
            if not all(isinstance(x, str) for x in documents):
                self.send_json({"error": "documents must be strings"}, 400)
                return
            results = [{"index": i, "relevance_score": 1 if query in text else 0}
                       for i, text in enumerate(documents)]
            self.send_json({"results": results[::-1]})
        elif self.path.endswith("/generate"):
            if body.get("stream") is not False:
                self.send_json({"error": "non-streaming generate is required"}, 400)
                return
            self.send_json({"response": "reply:" + body["prompt"], "done": True})
        else:
            value = "reply:" + next(x["content"] for x in body["messages"] if x["role"] == "user")
            if "/empty/" in self.path:
                value = ""
            self.send_json({"choices": [{"message": {"content": value}}]})


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 18000), Handler).serve_forever()
