# Dev tools

Development helpers that are **not** part of the graded code path.

## `mock_ai_service.py`

A deterministic stand-in for the organizer's Mock AI Service. It speaks the exact
wire formats the seekdb AI function clients parse, so `AI_PROMPT`, `AI_COMPLETE`,
`AI_EMBED` and `AI_RERANK` can be exercised locally without any real model.

```bash
python3 tools/dev/mock_ai_service.py --port 8000            # dim=4, no latency
python3 tools/dev/mock_ai_service.py --port 8000 --latency-ms 300
```

Every response is seeded by `sha256(model + input)`, so the same statement
returns the same value twice — safe to assert against.

### Endpoints

| provider | request | seekdb parses |
| --- | --- | --- |
| openai completion | `POST /v1/chat/completions` `{model, messages}` | `$.choices[0].message.content` |
| ollama completion | `POST /api/generate` `{model, prompt}` | `$.response` |
| openai embedding | `POST /v1/embeddings` `{model, input, dimensions?}` | `$.data[i].embedding`, `$.data[i].index` |
| ollama embedding | `POST /api/embed` `{model, input}` | `$.embeddings` |
| dashscope rerank | `POST /v1/rerank` `{model, input:{query, documents}}` | `$.output.results` |
| siliconflow rerank | `POST /v1/rerank` `{model, query, documents}` | `$.results` |

Rerank results are objects with an integer `index` and a double
`relevance_score`, sorted by score descending (stable for ties). Extra paths
(`/embeddings`, `/chat/completions`, `/api/chat`, `/rerank`, …) also resolve;
the shape is inferred from the body when the path is ambiguous.

### Inspecting and steering

```bash
curl -s localhost:8000/__health
curl -s localhost:8000/__control                                  # current fault config
curl -s -XPOST localhost:8000/__control -d '{"embed_shuffle":true}'
curl -s localhost:8000/__requests                                 # what the DB actually sent
curl -s -XPOST localhost:8000/__reset
```

`/__requests` records `path`, `model`, `n_inputs`, `n_documents`, `dim` … per
request. **This is how you prove a scoring point**: for the "one request carries
several inputs" requirement, assert that a scan of N rows produced one entry with
`n_inputs == N` instead of N entries with `n_inputs == 1`.

### Fault cookbook

| spec requirement | control call |
| --- | --- |
| batch responses come back out of order | `{"embed_shuffle":true}` |
| provider omits the `index` field | `{"embed_drop_index":true}` |
| returned vector length ≠ requested dimension | `{"embed_dim_override":8}` |
| non-numeric vector element | `{"embed_bad_element":true}` |
| result element is not an array | `{"embed_non_array_element":true}` |
| malformed payload (data is an object) | `{"embed_data_as_object":true}` |
| more vectors than inputs | `{"embed_extra_vector":true}` |
| 429 / 5xx, timeout | `{"fail_next":1,"fail_status":429}`, `{"latency_ms":3000}` |
| empty / missing completion | `{"complete_empty":true}`, `{"complete_missing_choice":true}` |
| rerank results not sorted by the provider | `{"rerank_reverse":true}` |
| rerank result missing `index` | `{"rerank_drop_index":true}` |
| rerank returns an out-of-range index | `{"rerank_extra_result":true}` |
| ollama streams by default (real behaviour) | `{"ollama_real_stream_default":true}` |

### Registering the mock as an endpoint

```sql
call DBMS_AI_SERVICE.CREATE_AI_MODEL('mock_complete', '{"type":"completion","model_name":"mock-complete"}');
call DBMS_AI_SERVICE.CREATE_AI_MODEL('mock_embed',    '{"type":"dense_embedding","model_name":"mock-embed"}');
call DBMS_AI_SERVICE.CREATE_AI_MODEL('mock_rerank',   '{"type":"rerank","model_name":"mock-rerank"}');

-- openai protocol
call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('ep_openai', '{
  "ai_model_name":"mock_complete","url":"http://127.0.0.1:8000/v1/chat/completions",
  "access_key":"mock-key","provider":"openai"}');
call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('ep_openai_embed', '{
  "ai_model_name":"mock_embed","url":"http://127.0.0.1:8000/v1/embeddings",
  "access_key":"mock-key","provider":"openai"}');

-- ollama protocol, note the empty access key on purpose
call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('ep_ollama', '{
  "ai_model_name":"mock_complete","url":"http://127.0.0.1:8000/api/generate",
  "access_key":"","provider":"ollama"}');
call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('ep_ollama_embed', '{
  "ai_model_name":"mock_embed","url":"http://127.0.0.1:8000/api/embed",
  "access_key":"","provider":"ollama"}');

call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('ep_dashscope_rerank', '{
  "ai_model_name":"mock_rerank","url":"http://127.0.0.1:8000/v1/rerank",
  "access_key":"mock-key","provider":"aliyun-dashscope"}');
call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('ep_siliconflow_rerank', '{
  "ai_model_name":"mock_rerank","url":"http://127.0.0.1:8000/v1/rerank",
  "access_key":"mock-key","provider":"siliconflow"}');
```

```sql
select AI_COMPLETE('mock_complete', 'hello');
select AI_EMBED('mock_embed', 'hello');
select AI_EMBED('mock_embed', 'hello', 8);                       -- explicit dimension
select AI_COMPLETE('mock_complete', AI_PROMPT('{0}+{1}=?', '1', '2'));
select AI_RERANK('mock_rerank', 'database', '["database search","cats"]');
select AI_RERANK('mock_rerank', 'database', json_array(json_object('text','database search')));
```
