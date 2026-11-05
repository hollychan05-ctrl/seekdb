#!/usr/bin/env python3
"""
Deterministic mock AI service for seekdb AI function development.

It answers the exact wire formats that the seekdb AI function clients parse:

  provider              request                                   parsed by seekdb
  --------------------  ----------------------------------------  ------------------------------
  openai completion     POST /v1/chat/completions  {model,messages} $.choices[0].message.content
  ollama completion     POST /api/generate         {model,prompt}   $.response
  openai embedding      POST /v1/embeddings       {model,input}    $.data[i].embedding / .index
  ollama embedding      POST /api/embed            {model,input}    $.embeddings
  dashscope rerank      POST /v1/rerank  {model,input:{query,documents}}  $.output.results
  siliconflow rerank    POST /v1/rerank  {model,query,documents}          $.results

Rerank elements are objects with an integer "index" and a double
"relevance_score", sorted by score descending.

Everything is deterministic (seeded by sha256 of the model + input), so a
statement can be asserted twice with the same expected result.

Fault injection is driven by a control endpoint, e.g.

    curl -s -XPOST localhost:8000/__control -d '{"embed_shuffle":true}'
    curl -s localhost:8000/__requests          # inspect what the DB actually sent
    curl -s -XPOST localhost:8000/__reset

Usage:
    python3 tools/dev/mock_ai_service.py --port 8000
    python3 tools/dev/mock_ai_service.py --port 8000 --dim 4 --latency-ms 50
"""

import argparse
import hashlib
import json
import random
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

DEFAULT_DIM = 4
CONTROL_KEYS = (
    "latency_ms",
    "fail_next",
    "fail_status",
    "embed_shuffle",
    "embed_drop_index",
    "embed_dim_override",
    "embed_bad_element",
    "embed_non_array_element",
    "embed_data_as_object",
    "embed_extra_vector",
    "complete_empty",
    "complete_missing_choice",
    "complete_stream",
    "rerank_reverse",
    "rerank_drop_index",
    "rerank_extra_result",
    "rerank_top_n_only",
    "ollama_real_stream_default",
)


def _seed(*parts):
    payload = "\x00".join(str(p) for p in parts).encode("utf-8")
    return int.from_bytes(hashlib.sha256(payload).digest()[:8], "big")


def make_vector(text, dim):
    """A deterministic pseudo-random unit-ish vector for `text`."""
    rnd = random.Random(_seed("vector", text))
    return [round(rnd.uniform(-1.0, 1.0), 6) for _ in range(dim)]


def make_text(model, prompt, as_json=False):
    """A deterministic completion that echoes a short digest of the prompt."""
    digest = hashlib.sha256(prompt.encode("utf-8")).hexdigest()[:8]
    lowered = prompt.lower()
    if as_json or '"type": "json_object"' in lowered or "json" in lowered:
        return json.dumps(
            {
                "answer": "ok",
                "digest": digest,
                "prompt_len": len(prompt),
                "model": model,
            },
            ensure_ascii=False,
        )
    return "mock[%s] len=%d model=%s" % (digest, len(prompt), model)


def make_score(query, document):
    """Deterministic rerank score in [0, 1); documents sharing words with the
    query score higher so that ordering is meaningful and stable."""
    q_words = set(query.lower().split())
    d_words = set(str(document).lower().split())
    overlap = len(q_words & d_words)
    base = random.Random(_seed("score", query, document)).random()
    return round(min(0.9999, 0.5 * base + 0.5 * (overlap / max(1, len(q_words)))), 6)


class State(object):
    def __init__(self, dim):
        self.lock = threading.Lock()
        self.dim = dim
        self.fault = {k: None for k in CONTROL_KEYS}
        self.fault["latency_ms"] = 0
        self.requests = []
        self.counters = {}

    def merge(self, patch):
        with self.lock:
            for key, value in patch.items():
                if key in CONTROL_KEYS:
                    self.fault[key] = value
            return dict(self.fault)

    def snapshot(self):
        with self.lock:
            return {"dim": self.dim, "fault": dict(self.fault), "counters": dict(self.counters)}

    def reset(self):
        with self.lock:
            self.fault = {k: None for k in CONTROL_KEYS}
            self.fault["latency_ms"] = 0
            self.requests = []
            self.counters = {}

    def record(self, entry):
        with self.lock:
            key = entry.get("kind", "?")
            self.counters[key] = self.counters.get(key, 0) + 1
            if len(self.requests) < 1000:
                self.requests.append(entry)

    def next_failure(self):
        """Return an HTTP status if this request must fail, else None."""
        with self.lock:
            f = self.fault
            if f.get("fail_next"):
                f["fail_next"] = int(f["fail_next"]) - 1
                return int(f.get("fail_status") or 500)
            return None

    def fault_value(self, key, default=None):
        with self.lock:
            value = self.fault.get(key)
            return default if value is None else value


def _first(body, *names):
    for name in names:
        if isinstance(body, dict) and name in body:
            return body[name]
    return None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    state = None
    verbose = True

    # ---------------------------------------------------------------- plumbing
    def log_message(self, fmt, *args):  # keep the console readable
        if self.verbose:
            sys.stderr.write("[mock] %s\n" % (fmt % args))

    def _send(self, obj, status=200):
        payload = obj if isinstance(obj, bytes) else json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def _read_body(self):
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b""
        if not raw:
            return {}
        try:
            return json.loads(raw.decode("utf-8"))
        except ValueError:
            return {"__raw__": raw.decode("utf-8", "replace")}

    def _slow_down(self):
        delay = self.state.fault_value("latency_ms", 0)
        if delay:
            time.sleep(delay / 1000.0)

    # ------------------------------------------------------------------ routes
    def do_GET(self):
        path = urlparse(self.path).path.rstrip("/") or "/"
        if path == "/__health":
            return self._send({"status": "ok"})
        if path == "/__control":
            return self._send(self.state.snapshot())
        if path == "/__requests":
            with self.state.lock:
                return self._send({"requests": list(self.state.requests)})
        if path.endswith("/api/tags"):
            return self._send({"models": [{"name": "mock-embed:latest"}, {"name": "mock-complete:latest"}]})
        return self._send({"error": "not found", "path": path}, 404)

    def do_POST(self):
        path = urlparse(self.path).path
        body = self._read_body()
        if path.rstrip("/").endswith("/__control"):
            return self._send(self.state.merge(body if isinstance(body, dict) else {}))
        if path.rstrip("/").endswith("/__reset"):
            self.state.reset()
            return self._send({"status": "reset"})

        status = self.state.next_failure()
        if status is not None:
            self.state.record({"kind": "injected_failure", "path": path, "status": status})
            return self._send(
                {"error": {"message": "injected failure", "type": "mock_error", "code": status}},
                status,
            )

        self._slow_down()
        try:
            shape = self._shape(path, body)
            if shape in ("openai_complete", "ollama_complete"):
                return self._completion(path, body, shape)
            if shape in ("openai_embed", "ollama_embed"):
                return self._embedding(path, body, shape)
            if shape == "rerank":
                return self._rerank(path, body)
        except Exception as exc:  # noqa: BLE001 - a mock must never die
            self.state.record({"kind": "exception", "path": path, "error": repr(exc)})
            return self._send({"error": {"message": repr(exc)}}, 500)
        return self._send({"error": "not found", "path": path}, 404)

    # ------------------------------------------------------------------ shapes
    @staticmethod
    def _shape(path, body):
        """Decide the response shape from the path first, then from the body."""
        p = path.lower()
        if p.endswith("/api/generate") or p.endswith("/api/chat"):
            return "ollama_complete"
        if p.endswith("/api/embed") or p.endswith("/api/embeddings"):
            return "ollama_embed"
        if "rerank" in p:
            return "rerank"
        if "embed" in p:
            return "openai_embed"
        if "chat" in p or "completion" in p:
            return "openai_complete"
        # unknown path: fall back to the body
        inner = _first(body, "input")
        if isinstance(inner, dict) and ("documents" in inner or "query" in inner):
            return "rerank"
        if _first(body, "documents") is not None:
            return "rerank"
        if inner is not None:
            return "ollama_embed" if _first(body, "prompt") is not None else "openai_embed"
        return "openai_complete"

    # -------------------------------------------------------------- completion
    def _completion(self, path, body, shape):
        model = str(_first(body, "model") or "mock-model")
        if shape == "ollama_complete":
            prompt = str(_first(body, "prompt") or "")
        else:
            messages = _first(body, "messages") or []
            prompt = "\n".join(str(m.get("content", "")) for m in messages if isinstance(m, dict))
        as_json = bool(_first(body, "format")) or bool(_first(body, "response_format"))
        content = "" if self.state.fault_value("complete_empty") else make_text(model, prompt, as_json)
        self.state.record(
            {
                "kind": shape,
                "path": path,
                "model": model,
                "prompt_len": len(prompt),
                "as_json": as_json,
            }
        )

        if shape == "ollama_complete":
            if self.state.fault_value("complete_stream") or (
                self.state.fault_value("ollama_real_stream_default") and body.get("stream") is None
            ):
                # real ollama streams by default: newline delimited json chunks
                payload = b"".join(
                    json.dumps({"model": model, "response": chunk, "done": done}).encode() + b"\n"
                    for chunk, done in ((content[: len(content) // 2], False), (content[len(content) // 2 :], True))
                )
                self.send_response(200)
                self.send_header("Content-Type", "application/x-ndjson")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
                return
            return self._send(
                {"model": model, "created_at": "2026-01-01T00:00:00Z", "response": content, "done": True}
            )

        if self.state.fault_value("complete_missing_choice"):
            response = {"id": "mock", "object": "chat.completion", "model": model, "choices": []}
        else:
            response = {
                "id": "mock",
                "object": "chat.completion",
                "created": 1767225600,
                "model": model,
                "choices": [
                    {"index": 0, "message": {"role": "assistant", "content": content}, "finish_reason": "stop"}
                ],
                "usage": {"prompt_tokens": len(prompt), "completion_tokens": len(content), "total_tokens": 0},
            }
        return self._send(response)

    # --------------------------------------------------------------- embedding
    def _embedding(self, path, body, shape):
        model = str(_first(body, "model") or "mock-embed")
        raw_input = _first(body, "input")
        if raw_input is None:
            raw_input = _first(body, "prompt")
        inputs = raw_input if isinstance(raw_input, list) else ["" if raw_input is None else raw_input]
        inputs = ["" if item is None else str(item) for item in inputs]

        dim = self.state.fault_value("embed_dim_override")
        if dim is None:
            dim = _first(body, "dimensions") or self.state.dim
        dim = int(dim)
        self.state.record(
            {"kind": shape, "path": path, "model": model, "n_inputs": len(inputs), "dim": dim}
        )

        vectors = [make_vector(text, dim) for text in inputs]
        if self.state.fault_value("embed_bad_element") and vectors:
            vectors[0] = list(vectors[0])
            if vectors[0]:
                vectors[0][0] = "not-a-number"
            else:
                vectors[0] = ["not-a-number"]
        if self.state.fault_value("embed_non_array_element") and vectors:
            vectors[0] = {"embedding": vectors[0]}
        if self.state.fault_value("embed_extra_vector"):
            vectors.append(make_vector("__extra__", dim))

        if shape == "ollama_embed":
            return self._send({"model": model, "embeddings": vectors, "total_duration": 1})

        data = []
        for index, vector in enumerate(vectors):
            item = {"object": "embedding", "embedding": vector}
            if not self.state.fault_value("embed_drop_index"):
                item["index"] = index
            data.append(item)
        if self.state.fault_value("embed_shuffle"):
            random.Random(1234).shuffle(data)  # index fields still carry the truth
        payload = {
            "object": "list",
            "model": model,
            "data": {} if self.state.fault_value("embed_data_as_object") else data,
            "usage": {"prompt_tokens": sum(len(t) for t in inputs), "total_tokens": 0},
        }
        return self._send(payload)

    # ------------------------------------------------------------------ rerank
    def _rerank(self, path, body):
        model = str(_first(body, "model") or "mock-rerank")
        inner = _first(body, "input")
        if isinstance(inner, dict):
            query = str(inner.get("query", ""))
            documents = inner.get("documents") or []
            dashscope = True
        else:
            query = str(_first(body, "query") or "")
            documents = _first(body, "documents") or []
            dashscope = False

        raw = _first(body, "top_n")
        if raw is None and isinstance(_first(body, "parameters"), dict):
            raw = _first(body["parameters"], "top_n", "top_n_documents")
        top_n = int(raw) if raw else 0

        self.state.record(
            {
                "kind": "rerank",
                "path": path,
                "model": model,
                "n_documents": len(documents),
                "top_n": top_n,
            }
        )

        scored = []
        for index, document in enumerate(documents):
            text = document
            if isinstance(document, dict):
                text = document.get("text") or document.get("content") or json.dumps(document, ensure_ascii=False)
            scored.append({"index": index, "relevance_score": make_score(query, text), "document": document})
        # stable: equal scores keep the original document order
        scored.sort(key=lambda item: -item["relevance_score"])
        if self.state.fault_value("rerank_reverse"):
            scored.reverse()
        if top_n:
            scored = scored[:top_n]
        if self.state.fault_value("rerank_extra_result"):
            scored.append({"index": len(documents), "relevance_score": 0.0})
        if self.state.fault_value("rerank_drop_index"):
            for item in scored:
                item.pop("index", None)

        results = [
            {"index": item["index"], "relevance_score": item["relevance_score"], "document": item["document"]}
            for item in scored
        ]
        if dashscope:
            return self._send(
                {"output": {"results": results}, "usage": {"total_tokens": 0}, "request_id": "mock"}
            )
        return self._send({"id": "mock", "model": model, "results": results, "usage": {"total_tokens": 0}})


def main():
    parser = argparse.ArgumentParser(description="mock AI service for seekdb AI functions")
    parser.add_argument("--host", default="0.0.0.0", help="bind address (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=8000, help="bind port (default: 8000)")
    parser.add_argument("--dim", type=int, default=DEFAULT_DIM, help="default embedding dimension")
    parser.add_argument("--latency-ms", type=int, default=0, help="artificial latency per request")
    parser.add_argument("--quiet", action="store_true", help="do not log every request")
    args = parser.parse_args()

    state = State(args.dim)
    state.fault["latency_ms"] = args.latency_ms
    Handler.state = state
    Handler.verbose = not args.quiet

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.daemon_threads = True
    print(
        "[mock] listening on http://%s:%d  (dim=%d)  control: /__control  requests: /__requests"
        % (args.host, args.port, args.dim),
        flush=True,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[mock] bye", flush=True)


if __name__ == "__main__":
    main()
