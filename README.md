# ckvs — Concurrent Key-Value Server

A lightweight, in-memory key-value store exposed over HTTP, written in C.  
Designed around a thread-safe hash table, a fixed-size thread pool, and a minimal HTTP request dispatcher. No external dependencies beyond the POSIX standard library.

---

## Architecture

```
                         ┌─────────────────────────────────────────┐
                         │               server_functions.c         │
                         │  main() · start_server() · server_loop() │
                         └───────────────────┬─────────────────────┘
                                             │ accept()
                                             ▼
                         ┌─────────────────────────────────────────┐
                         │               threadPool.c               │
                         │   pool_submit() ──► TaskQueue ──► worker │
                         └───────────────────┬─────────────────────┘
                                             │ read() / write()
                                             ▼
                         ┌─────────────────────────────────────────┐
                         │              route_handler.c             │
                         │  handle_request() ──► /get /set /delete  │
                         └───────────────────┬─────────────────────┘
                                             │ ht_get / ht_set / ht_delete
                                             ▼
                         ┌─────────────────────────────────────────┐
                         │               hash_table.c               │
                         │  separate chaining · rwlock · djb2+seed  │
                         └─────────────────────────────────────────┘
```

### Modules

| File | Responsibility |
|---|---|
| `config.h` | Global macros (port, buffer sizes, timeouts) |
| `hash_table.{h,c}` | Thread-safe generic hash table |
| `route_handler.{h,c}` | HTTP request parsing and route dispatch |
| `server_functions.{h,c}` | Socket setup, main loop, signal handling, response formatting |
| `threadPool.{h,c}` | Fixed-size worker thread pool with FIFO task queue |

---

## Hash Table

The hash table (`hash_table.c`) is the core data structure. It stores arbitrary binary values under NUL-terminated string keys.

**Implementation details:**

- **Collision resolution:** separate chaining via singly-linked lists per bucket.
- **Concurrency:** a per-table `pthread_rwlock_t` serialises writes while allowing unlimited concurrent reads.
- **Resizing:** when `size + 1 >= capacity`, the bucket array grows to the next prime ≥ 2 × capacity. Entries are relinked in-place (no reallocation) using the cached raw hash stored in each `Entry` node, so no re-hashing is needed.
- **Hash function:** pluggable via function pointer (`hash_func`). The server wires in a seeded djb2 variant (`hash = hash * 33 + c`).
- **Seed:** generated from `/dev/urandom` at table creation (fallback: `time(NULL)`) to mitigate hash-flooding attacks.
- **Persistence:** `ht_destroy()` optionally serialises the entire table to a binary file; `ht_load()` reconstructs it. The on-disk format is a sequence of records `[key_len | key | val_size | value]`.

**Complexity:**

| Operation | Average | Worst case |
|---|---|---|
| `ht_get` | O(1) | O(n) |
| `ht_set` | O(1) amortised | O(n) |
| `ht_delete` | O(1) | O(n) |
| `ht_resize` | O(n) | O(n) |

---

## Thread Pool

`threadPool.c` implements a classic boss/worker pattern:

- A fixed number of POSIX threads are spawned at startup and sleep on a condition variable (`pthread_cond_t`) when the task queue is empty.
- `pool_submit()` wraps an accepted socket fd into a `Task` node, appends it to the FIFO queue, and signals exactly one worker (`pthread_cond_signal`).
- Each worker dequeues a task, handles the full HTTP exchange (including keep-alive loops), closes the socket, and frees the task node before returning to sleep.
- `pool_destroy()` sets a `shutdown` flag, broadcasts to all workers, then `pthread_join`s every thread to drain the queue before freeing resources.

---

## HTTP Request Handling

The server implements a minimal subset of HTTP/1.1:

- The first line of the request is parsed with `strtok_r` (re-entrant, thread-safe) to extract the URL path and query string.
- Routes are matched by prefix against a static `Route[]` array. The first match wins.
- Query parameters are extracted by `get_query_param()`, which scans for `name=value` pairs delimited by `&` or a space.
- Input is validated by `is_sanitized()`, which percent-decodes the value on the fly and rejects any non-printable character.
- Keep-alive is detected from the `Connection: keep-alive` header; the worker re-reads the socket in a loop until the client closes or the `SO_RCVTIMEO` timeout (5 s) fires.

### Endpoints

| Method | Path | Parameters | Success | Error |
|---|---|---|---|---|
| GET | `/get` | `key=<k>` | `200 {"value":"<v>"}` | `400` missing key · `404` not found |
| GET | `/set` | `key=<k>&val=<v>` | `200 stored` | `400` bad params · `500` internal |
| GET | `/delete` | `key=<k>` | `200 value deleted` | `400` missing key · `404` not found |

---

## Building

```bash
gcc -O2 -Wall -Wextra -pthread \
    server_functions.c hash_table.c route_handler.c threadPool.c \
    -o ckvs
```

---

## Running

```bash
# Start with an empty table
./ckvs

# Load from file on start, save to the same file on exit (Ctrl+C)
./ckvs data.bin

# Explicit load/save flags (can be different files)
./ckvs -l load.bin -s save.bin

# Load and save to the same file with combined flag
./ckvs -ls data.bin
```

The server listens on port **8080** by default (configurable via `PORT` in `config.h`).

---

## Usage Examples

```bash
# Store a value
curl "http://localhost:8080/set?key=name&val=alice"
# → stored

# Retrieve it
curl "http://localhost:8080/get?key=name"
# → {"value":"alice"}

# Delete it
curl "http://localhost:8080/delete?key=name"
# → value deleted

# Key not found
curl "http://localhost:8080/get?key=name"
# → key not exists  (HTTP 404)
```

---

## Configuration

All tuneable constants are centralised in `config.h`:

| Macro | Default | Description |
|---|---|---|
| `PORT` | `8080` | TCP port the server binds to |
| `BUFFER_SIZE` | `1024` | Read/write buffer per request |
| `URL_BUFFER_SIZE` | `1024` | Max URL length |
| `PARAM_KEY_SIZE` | `64` | Max query parameter key length |
| `PARAM_VALUE_SIZE` | `1024` | Max query parameter value length |
| `RESPONSE_BUFFER_SIZE` | `1280` | Response body buffer |
| `LISTEN_BACKLOG` | `1023` | `listen()` backlog queue depth |
| `KEEPALIVE_TIMEOUT` | `5` | `SO_RCVTIMEO` in seconds |

The thread count (default: **8**) is set at `pool_create()` call-site in `main()`.

---

## Shutdown & Persistence

Sending `SIGINT` (Ctrl+C) triggers a clean shutdown sequence:

1. The signal handler clears `keep_running`.
2. `server_loop()` exits the `accept()` loop and calls `pool_destroy()`, which drains all pending tasks and joins worker threads.
3. The listening socket is closed.
4. `ht_destroy()` is called; if a save path was provided, the entire table is serialised to disk before memory is freed.

---

## Limitations & Known Issues

- All three endpoints use the GET method; a production implementation would use POST/PUT/DELETE as appropriate.
- The `is_sanitized()` check rejects binary values; values are stored as NUL-terminated strings, so null bytes inside values are silently truncated by `strlen`.
- No TLS, no authentication, no rate limiting.
- The thread count is compile-time fixed in `main()`; consider exposing it as a CLI argument.
