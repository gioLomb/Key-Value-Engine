# ckvs — In-Memory Key-Value Server

A lightweight, in-memory key-value store exposed over HTTP, written in C.
Built around a single-threaded epoll event loop, a thread-safe hash table, and a minimal HTTP request dispatcher. No external dependencies beyond the POSIX standard library.

---

## Architecture

```
                         ┌─────────────────────────────────────────┐
                         │             server_functions.c           │
                         │  main() · start_server() · server_loop() │
                         │  accept_connections() · setup_client()   │
                         └───────────────────┬─────────────────────┘
                                             │ epoll_wait()
                                             ▼
                         ┌─────────────────────────────────────────┐
                         │         handle_socket_event()            │
                         │         handle_timer_event()             │
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
| `server_functions.{h,c}` | Socket setup, epoll event loop, signal handling, response formatting |

---

## Event Loop

The server is **single-threaded and event-driven**. `server_loop()` calls `epoll_wait()` in a tight loop and dispatches each ready fd to one of three handlers:

- **`accept_connections()`** — fires when a new TCP connection arrives on the listening socket. Drains the accept queue completely (edge-triggered), calling `setup_client()` for each fd.
- **`handle_socket_event()`** — fires when data is available on a client socket. Reads the HTTP request, dispatches to the route handler, sends the response, and resets the keepalive timer.
- **`handle_timer_event()`** — fires when a client's keepalive timerfd expires. Closes the connection immediately.

### fd → ClientCtx resolution

Each accepted client is represented by a `ClientCtx` that embeds two `ConnectionEvent` structs — one for the socket fd, one for the timerfd. Both are registered in epoll with `data.ptr` pointing to their respective `ConnectionEvent`. When an event fires, the loop casts `data.ptr` to `ConnectionEvent*`, reads the `type` field, and follows `parent` to reach the owning `ClientCtx` — **zero hash-table lookups, zero extra allocations**.

The listening socket fd is registered with `data.ptr = NULL`, used as a sentinel to distinguish it from client events.

### Live-client list

All active `ClientCtx` are linked in a **doubly-linked list** anchored in `server_loop()`. This replaces the two auxiliary hash tables used in earlier versions and enables O(n) graceful shutdown by simply walking the list.

---

## Hash Table

The hash table (`hash_table.c`) stores arbitrary binary values under binary keys.

**Implementation details:**

- **Collision resolution:** separate chaining via singly-linked lists per bucket.
- **Concurrency:** a per-table `pthread_rwlock_t` serialises writes while allowing unlimited concurrent reads.
- **Resizing:** when `size + 1 >= capacity`, the bucket array grows to the next prime ≥ 2 × capacity. Entries are relinked in-place using the cached raw hash stored in each `Entry` — no recomputation needed.
- **Hash function:** pluggable via function pointer (`hash_func`). The server uses a seeded djb2 variant (`hash = hash * 33 + c`).
- **Seed:** generated from `/dev/urandom` at table creation (fallback: `time(NULL)`) to mitigate hash-flooding attacks.
- **Persistence:** `ht_destroy()` optionally serialises the entire table to a binary file; `ht_load()` reconstructs it on startup. On-disk format per record: `[key_len | key | val_size | value]`.
- **Initial capacity:** pre-allocated to 16384 buckets at startup to avoid resize pauses under load.

**Complexity:**

| Operation | Average | Worst case |
|---|---|---|
| `ht_get` | O(1) | O(n) |
| `ht_set` | O(1) amortised | O(n) |
| `ht_delete` | O(1) | O(n) |
| `ht_resize` | O(n) | O(n) |

---

## HTTP Request Handling

The server implements a minimal subset of HTTP/1.1:

- The request is parsed on a **local stack copy** of the receive buffer so `strtok_r` never modifies the original `ClientCtx.buffer`.
- Routes are matched by prefix against a static `Route[]` array. The first match wins.
- Query parameters are extracted by `get_query_param()`, scanning for `name=value` pairs delimited by `&` or a space.
- Input is validated by `is_sanitized()`, which percent-decodes on the fly and rejects any non-printable character.
- Keep-alive is detected from the `Connection: keep-alive` header; the keepalive timerfd is reset after each successful response and fires after `KEEPALIVE_TIMEOUT` seconds of inactivity.

### Endpoints

| Path | Parameters | Success | Error |
|---|---|---|---|
| `/get` | `key=<k>` | `200 {"value":"<v>"}` | `400` missing key · `404` not found |
| `/set` | `key=<k>&val=<v>` | `200 stored` | `400` bad params · `500` internal |
| `/delete` | `key=<k>` | `200 value deleted` | `400` missing key · `404` not found |

---

## Building

```bash
gcc -O2 -Wall -Wextra -pthread \
    server_functions.c hash_table.c route_handler.c \
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

All tuneable constants are in `config.h`:

| Macro | Default | Description |
|---|---|---|
| `PORT` | `8080` | TCP port the server binds to |
| `KEEPALIVE_TIMEOUT` | `5` | Seconds of inactivity before a connection is closed |
| `MAX_CLIENTS` | `16384` | Maximum simultaneous connections |
| `MAX_EVENTS` | `4096` | Maximum events returned per `epoll_wait()` call |
| `BUFFER_SIZE` | `1024` | Per-connection receive buffer size |
| `URL_BUFFER_SIZE` | `1024` | Maximum URL length |
| `PARAM_KEY_SIZE` | `64` | Maximum query parameter key length |
| `PARAM_VALUE_SIZE` | `1024` | Maximum query parameter value length |
| `RESPONSE_BUFFER_SIZE` | `1280` | Response body buffer |
| `LISTEN_BACKLOG` | `65535` | `listen()` backlog queue depth |

---

## Shutdown & Persistence

Sending `SIGINT` (Ctrl+C) triggers a clean shutdown:

1. The signal handler clears `keep_running` using only async-signal-safe calls.
2. `server_loop()` exits the `epoll_wait()` loop.
3. The live-client list is walked and every open connection is closed.
4. The epoll instance and listening socket are closed.
5. `ht_destroy()` is called; if a save path was provided, the entire table is serialised to disk before memory is freed.

---

## Limitations & Known Issues

- All three endpoints use the GET method; a production implementation would use POST/PUT/DELETE as appropriate.
- `is_sanitized()` rejects binary values — keys and values are treated as printable strings.
- No TLS, no authentication, no rate limiting.
- Single-threaded: one core is used. Scaling to multiple cores would require `SO_REUSEPORT` with multiple processes or a thread pool with careful lock partitioning on the hash table.