# Key-Value Engine

A lightweight, high-performance in-memory key-value store exposed over HTTP, written in C. The server is built on a custom generic hash table and a **single-threaded, event-driven architecture** using Linux `epoll`.

> Looking for the multi-threaded version? See the [`feature/threadPool`](../../tree/feature/threadPool) branch.

---

## Architecture

Concurrency is handled entirely through non-blocking I/O: a single `epoll` event loop multiplexes thousands of simultaneous connections without spawning any threads.

```
                      ┌──────────────────────────────┐
                      │          epoll_wait()         │
                      └──────────┬───────────────────┘
                                 │
            ┌────────────────────┼──────────────────────────┐
            │                    │                           │
     server fd (NULL)     sock_ev (client)          timerEv (keepalive)
            │                    │                           │
   accept_connections()  handle_socket_event()      handle_timerEvent()
            │                    │
     setup_client()       rate_limit_check()
            │                    │
    client_pool_alloc()   handle_request()
                                 │
                         ┌───────┴────────┐
                         │  route_handler │
                         └───────┬────────┘
                                 │
                   ┌─────────────┴──────────────┐
                   │                            │
              Hash_Table (db)          Hash_Table (rateLimitTable)
              key-value store          per-IP rate limiting
```

Each accepted connection is represented by a `ClientCtx` struct that embeds two `ConnectionEvent` descriptors — one for the socket fd and one for a `timerfd` — both registered directly in epoll. When an event fires, `data.ptr` resolves type and owner with zero extra lookups and zero extra allocations.

All live `ClientCtx` nodes are linked in a doubly-linked list anchored in `server_loop()`, enabling O(n) graceful shutdown on SIGINT without any auxiliary data structure.

A dedicated `timerfd` registered in the same epoll instance fires every 60 seconds to evaluate the snapshot trigger rule. When the rule is met, a child process is forked to write the snapshot without blocking the event loop.

---

## Components

| File | Responsibility |
|---|---|
| `config.h` | Global constants and `#include` aggregation |
| `hash_table.c/h` | Thread-safe generic hash table with chaining, auto-resize, and binary persistence |
| `client_pool.c/h` | Slab allocator for `ClientCtx` objects; fixed-size chunks, O(1) alloc/release |
| `route_handler.c/h` | HTTP request parsing and dispatch to `/get`, `/set`, `/delete`, `/stats` |
| `server_functions.c/h` | epoll event loop, connection lifecycle, signal handling, snapshot scheduling, `main()` |

### Hash Table

- **Generic binary keys and values** — keys and values are opaque byte buffers (`void *` + `size_t`), not restricted to strings
- **Separate chaining** for collision resolution
- **Readers-writer lock** (`pthread_rwlock_t`): multiple concurrent reads, serialised writes
- **Pluggable hash function** via function pointer; per-instance random seed from `/dev/urandom` to mitigate hash-flooding attacks
- **Auto-resize** to the next prime ≥ 2× capacity when load factor reaches 1; existing entries are relinked using the cached hash, no recomputation
- **Binary persistence**: `ht_snapshot()` serialises all entries to file without destroying the table; `ht_load()` restores them on startup; `ht_destroy()` calls `ht_snapshot()` internally if a path is provided

### Client Pool

- **Slab allocation** — `ClientCtx` objects are stored in fixed-size `MemoryChunk` blocks of 64 slots each, allocated contiguously to minimise heap fragmentation under high connection rates
- **O(1) alloc and release** — each chunk maintains a local free list threaded through `ClientCtx.next`; popping and pushing slots requires no global search
- **O(1) chunk lookup on release** — every slot holds a `parentChunk` back-pointer set at chunk creation and never mutated, so `client_pool_release()` locates the owning chunk without traversing the list
- **Automatic shrink** — when a chunk's reference count drops to zero it is unlinked and freed immediately, keeping resident memory proportional to peak concurrency; the last chunk is always retained to avoid churn under low load
- **Transparent to callers** — `setup_client()` calls `client_pool_alloc()` and `close_client()` calls `client_pool_release()`; the rest of the server is unaware of the underlying slab structure

### HTTP Interface

Five routes are supported via query parameters:

| Route | Parameters | Success | Error |
|---|---|---|---|
| `/get` | `key=<k>` | `200 {"value":"..."}` | `404` key not found / `400` missing key |
| `/set` | `key=<k>&val=<v>` | `200 stored` | `400` bad params / `500` internal error |
| `/delete` | `key=<k>` | `200 value deleted` | `404` key not found / `400` missing key |
| `/stats` | — | `200 {"uptime_seconds":...,"totalRequests":...,"totalConnections":...,"total_keys":...}` | — |
| Any other path | — | — | `404` route does not exist |

All keys and values are sanitised: percent-encoded sequences are decoded and every character must be printable before the request reaches the hash table. Values returned by `/get` are JSON-escaped before insertion into the response body to prevent injection.

### Keepalive & Timers

Each client gets a `CLOCK_MONOTONIC` timerfd armed to `KEEPALIVE_TIMEOUT` seconds (default 5). On every successful request the timer is re-armed. When it fires, the connection is closed immediately. The `Connection: keep-alive` header in the request controls whether the server resets the timer or closes after the response.

### Snapshot Persistence

Persistence is optional and controlled by the `-s` flag. When enabled:

- A `timerfd` fires every 60 seconds and evaluates the snapshot rule: **at least 300 seconds elapsed AND at least 100 keys modified** since the last snapshot
- When the rule is met, the server `fork()`s a child process that calls `ht_snapshot()` and exits; the parent continues serving requests without any pause
- The child closes the inherited server and epoll file descriptors before writing, so it does not hold the listening port open
- `SIGCHLD` is set to `SIG_IGN` so terminated child processes are reaped automatically without leaving zombies
- On clean shutdown (SIGINT), `ht_destroy()` always writes a final snapshot regardless of the trigger rule

### Rate Limiting

Each incoming request is checked against a per-IP sliding window counter before being dispatched to the route handler. The algorithm is an approximation of a sliding window using two fixed-window counters:

```
estimated_rate = countPrev * (1 - elapsed / window) + countCurr
```

If the estimated rate exceeds `RATE_LIMIT_RPS` (default 100 req/s), the server responds immediately with `429 Too Many Requests` and closes the connection. The rate table is a second `Hash_Table` instance keyed by IP string, entirely separate from the key-value store.

---

## Build

```bash
gcc -O2 -Wall -Wextra -o kvengine \
    server_functions.c hash_table.c route_handler.c client_pool.c \
    -lpthread
```

---

## Usage

```bash
# Start with an empty table, no persistence
./kvengine

# Load from file on startup, save to the same file on shutdown + periodic snapshots
./kvengine -ls data.bin

# Load and save to the same file (explicit)
./kvengine -ls data.bin

# Load from one file, save to another
./kvengine -l load.bin -s save.bin
```

The server listens on port **8080** by default (configurable via `PORT` in `config.h`).

```bash
# Store a value
curl "http://localhost:8080/set?key=hello&val=world"

# Retrieve it
curl "http://localhost:8080/get?key=hello"

# Delete it
curl "http://localhost:8080/delete?key=hello"

# Check server stats
curl "http://localhost:8080/stats"
```

Shut down cleanly with **Ctrl+C**: the SIGINT handler clears `keepRunning`, the event loop drains, all open connections are closed, and the table is persisted if a save path was provided.

---

## Configuration

All tunables live in `config.h`:

| Constant | Default | Description |
|---|---|---|
| `PORT` | `8080` | Listening port |
| `KEEPALIVE_TIMEOUT` | `5` | Idle connection timeout (seconds) |
| `MAX_EVENTS` | `4096` | Max events per `epoll_wait()` call |
| `MAX_CLIENTS` | `16384` | Hard cap on simultaneous connections |
| `BUFFER_SIZE` | `1024` | Per-connection read buffer (bytes) |
| `LISTEN_BACKLOG` | `65535` | TCP listen backlog |
| `RATE_LIMIT_RPS` | `100` | Max requests per second per IP |

---

## Runtime Statistics

`/stats` returns a JSON object with live server metrics:

| Field | Description |
|---|---|
| `uptime_seconds` | Seconds since server start |
| `totalRequests` | Total HTTP requests dispatched |
| `totalConnections` | Total TCP connections accepted |
| `total_keys` | Current number of keys in the store |

---

## Branch Overview

| Branch | Concurrency model |
|---|---|
| `master` ← you are here | Single-threaded, epoll, event-driven |
| [`feature/threadPool`](../../tree/feature/threadPool) | Multi-threaded, fixed thread pool + FIFO task queue |