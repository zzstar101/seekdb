#!/usr/bin/env python3
"""Mock AI provider endpoints used by the seekdb AI-function evaluation.

The official evaluation suite (tools/deploy/mysql_test/test_suite/ai_function/t/
ai_function_enhancement.test) points every AI model endpoint at
http://0.0.0.0:8000 with five different URL shapes.  The judge machine runs its
own mock there; this file reproduces the shapes it must answer so the suite can
be executed against a locally built binary instead of waiting for the queue.

Protocol notes (kept deliberately boring, like a real provider):
  * /compatible-mode/v1/chat/completions  OpenAI chat completions
  * /compatible-mode/v1/embeddings        OpenAI embeddings, honours `dimensions`
  * /v1/rerank                            SiliconFlow rerank (`results` + `index`)
  * /api/generate                         Ollama native generate (no bearer)
  * /api/embed                            Ollama native embed (no bearer)

Every embedding is 768 wide unless the request asks for another width, matching
the dimension the official result file expects.
"""

import argparse
import json
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DEFAULT_DIM = 768

_log_lock = threading.Lock()
_log_path = None


def log(message):
    with _log_lock:
        print(message, flush=True)
        if _log_path:
            with open(_log_path, "a", encoding="utf-8") as handle:
                handle.write(message + "\n")


def vector_for(text, dimension):
    """Deterministic finite vector, one value per element."""
    base = float(len(text) % 17)
    return [base + index / 1000.0 for index in range(dimension)]


def embeddings_payload(texts, dimension, omit_index):
    data = []
    for position, text in enumerate(texts):
        entry = {"embedding": vector_for(text, dimension)}
        if not omit_index:
            entry["index"] = position
        data.append(entry)
    return {"data": data, "model": "mock-embed", "usage": {"prompt_tokens": 1, "total_tokens": 1}}


def rerank_payload(body, query, documents):
    scored = []
    for position, document in enumerate(documents):
        text = document if isinstance(document, str) else json.dumps(document, ensure_ascii=False)
        if text == query:
            score = 0.99
        else:
            score = 0.50 - position * 0.01
        scored.append({"index": position, "relevance_score": score})
    # Real providers do not promise that the response is already ordered.
    scored.reverse()
    payload = {"results": scored, "model": "mock-rerank"}
    if "input" in body:
        return {"output": payload}
    return payload


def documents_of(request):
    documents = request.get("documents")
    if documents is None:
        documents = request.get("docs")
    if documents is None and isinstance(request.get("input"), list):
        documents = request["input"]
    return documents or []


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def _send(self, status, payload):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        self._send(200, {"status": "ok", "path": self.path})

    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", "0") or 0))
        try:
            body = json.loads(raw.decode("utf-8")) if raw else {}
        except ValueError as error:
            log("REJECT {} bad json: {}".format(self.path, error))
            self._send(400, {"error": {"message": str(error)}})
            return

        path = re.sub(r"/+", "/", self.path.split("?")[0])
        log("REQUEST {} {}".format(path, json.dumps(body, ensure_ascii=False)[:2000]))

        try:
            payload = self.route(path, body)
        except Exception as error:  # noqa: BLE001 - report anything as a 400
            log("REJECT {} {}".format(path, error))
            self._send(400, {"error": {"message": str(error)}})
            return

        log("RESPONSE {} {}".format(path, json.dumps(payload, ensure_ascii=False)[:1000]))
        self._send(200, payload)

    def route(self, path, body):
        if path.endswith("/chat/completions") or path.endswith("/v1/completions") or path.endswith("/completions"):
            content = ""
            for message in body.get("messages") or []:
                if message.get("role") == "user":
                    content = message.get("content") or ""
            if not content:
                content = body.get("prompt") or ""
            if not isinstance(content, str):
                content = json.dumps(content, ensure_ascii=False)
            return {
                "id": "mock-completion",
                "object": "chat.completion",
                "model": body.get("model") or "mock",
                "choices": [{"index": 0, "message": {"role": "assistant", "content": "mock:" + content}, "finish_reason": "stop"}],
                "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2},
            }

        if path.endswith("/embeddings"):
            inputs = body.get("input")
            if isinstance(inputs, str):
                inputs = [inputs]
            if not isinstance(inputs, list) or not inputs:
                raise ValueError("embedding request without input")
            dimension = body.get("dimensions") or body.get("dim")
            if not isinstance(dimension, int) or isinstance(dimension, bool) or dimension <= 0:
                dimension = DEFAULT_DIM
            return embeddings_payload(inputs, dimension, omit_index=False)

        if path.endswith("/rerank"):
            request = body.get("input") if isinstance(body.get("input"), dict) else body
            query = request.get("query")
            if not isinstance(query, str):
                raise ValueError("rerank request without query")
            documents = documents_of(request)
            if not documents:
                raise ValueError("rerank request without documents")
            return rerank_payload(body, query, documents)

        if path.endswith("/generate") or path.endswith("/api/generate"):
            prompt = body.get("prompt")
            if not isinstance(prompt, str):
                raise ValueError("generate request without prompt")
            if body.get("stream") is True:
                raise ValueError("streaming is not supported by the evaluation mock")
            return {"model": body.get("model") or "mock", "response": "mock:" + prompt, "done": True, "done_reason": "stop"}

        if path.endswith("/api/embed") or path.endswith("/embed"):
            inputs = body.get("input")
            if isinstance(inputs, str):
                inputs = [inputs]
            if not isinstance(inputs, list) or not inputs:
                raise ValueError("embed request without input")
            return {"model": body.get("model") or "mock", "embeddings": [vector_for(text, DEFAULT_DIM) for text in inputs]}

        if path.endswith("/api/embeddings"):
            prompt = body.get("prompt")
            if not isinstance(prompt, str):
                raise ValueError("legacy embed request without prompt")
            return {"embedding": vector_for(prompt, DEFAULT_DIM)}

        raise ValueError("unrecognised protocol URL {}".format(path))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--log", default="")
    args = parser.parse_args()

    global _log_path
    _log_path = args.log or None

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.daemon_threads = True
    log("mock AI endpoints listening on {}:{}".format(args.host, args.port))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        log("mock AI endpoints stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
