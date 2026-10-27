"""Ad-hoc smoke test for mock_ai_endpoints.py (local only, not committed)."""
import json
import socket
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

PORT = 8765
MOCK = Path(__file__).with_name("mock_ai_endpoints.py")


def post(path, body, headers=None):
    data = json.dumps(body).encode()
    request = urllib.request.Request("http://127.0.0.1:%d%s" % (PORT, path), data=data,
                                     headers=headers or {"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=10) as response:
        return json.loads(response.read().decode())


def main():
    proc = subprocess.Popen([sys.executable, str(MOCK), "--port", str(PORT), "--log", "mock-smoke.log"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    try:
        for _ in range(100):
            try:
                with socket.create_connection(("127.0.0.1", PORT), timeout=0.2):
                    break
            except OSError:
                time.sleep(0.05)
        else:
            raise SystemExit("mock did not start")

        ok = True

        chat = post("/compatible-mode/v1/chat/completions",
                    {"model": "qwen3-32b", "messages": [{"role": "user", "content": "hello"}]},
                    {"Content-Type": "application/json", "Authorization": "Bearer mock-key"})
        content = chat["choices"][0]["message"]["content"]
        print("chat ->", content)
        ok &= content.startswith("mock:")

        embed = post("/compatible-mode/v1/embeddings",
                     {"model": "text-embedding-v1", "input": ["single-row"]},
                     {"Content-Type": "application/json", "Authorization": "Bearer mock-key"})
        dim = len(embed["data"][0]["embedding"])
        print("embed dim", dim, "index", embed["data"][0].get("index"))
        ok &= dim == 768

        embed2 = post("/compatible-mode/v1/embeddings",
                      {"model": "m", "input": ["a", "b"], "dimensions": 16},
                      {"Content-Type": "application/json", "Authorization": "Bearer k"})
        print("embed dims honoured", [len(d["embedding"]) for d in embed2["data"]],
              "indices", [d["index"] for d in embed2["data"]])
        ok &= [len(d["embedding"]) for d in embed2["data"]] == [16, 16]

        rerank = post("/v1/rerank",
                      {"model": "BAAI/bge-reranker-v2-m3", "query": "apple",
                       "documents": ["apple", "banana", "fruit", "vegetable"]},
                      {"Content-Type": "application/json", "Authorization": "Bearer mock-key"})
        print("rerank", rerank)
        scores = {item["index"]: item["relevance_score"] for item in rerank["results"]}
        ok &= sorted(scores) == [0, 1, 2, 3] and max(scores, key=scores.get) == 0

        generate = post("/api/generate", {"model": "qwen2.5", "prompt": "hello", "stream": False})
        print("generate", generate["response"])
        ok &= generate["response"].startswith("mock:")

        ollama_embed = post("/api/embed", {"model": "nomic-embed-text", "input": ["hello"]})
        print("ollama embed dims", [len(v) for v in ollama_embed["embeddings"]])
        ok &= len(ollama_embed["embeddings"][0]) == 768

        print("SMOKE", "OK" if ok else "FAILED")
        return 0 if ok else 1
    finally:
        proc.terminate()


if __name__ == "__main__":
    sys.exit(main())
