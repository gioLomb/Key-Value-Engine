# Key-Value Engine  --- TO BE UPDATED

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
            ┌────────────────────┼────────────────────┐
            │                    │                     │
     server fd (NULL)     sock_ev (client)      timer_ev (keepalive)
            │                    │                     │
   accept_connections()  handle_socket_event()  handle_timer_event()
            │                    │
     setup_client()       handle_request()
                                 │
                         ┌───────┴────────┐
                         │  route_handler │
                         └───────┬────────┘
                                 │
                            Hash_Table
```

Each accepted connection is represented by a `ClientCtx` struct that embeds two `ConnectionEvent` descriptors — one for the socket fd and one for a `timerfd` — both registered directly in epoll. When an event fires, `data.ptr` resolves type and owner with zero extra lookups and zero extra allocations.

All live `ClientCtx` nodes are linked in a doubly-linked list anchored in `server_loop()`, enabling O(n) graceful shutdown on SIGINT without any auxiliary data structure.

---

## Components

| File | Responsibility |
|---|---|
| `config.h` | Global constants and `#include` aggregation |
| `hash_table.c/h` | Thread-safe generic hash table with chaining, auto-resize, and binary persistence |
| `route_handler.c/h` | HTTP request parsing and dispatch to `/get`, `/set`, `/delete` |
| `server_functions.c/h` | epoll event loop, connection lifecycle, signal handling, `main()` |

### Hash Table

- **Generic binary keys and values** — keys and values are opaque byte buffers (`void *` + `size_t`), not restricted to strings
- **Separate chaining** for collision resolution
- **Readers-writer lock** (`pthread_rwlock_t`): multiple concurrent reads, serialised writes
- **Pluggable hash function** via function pointer; per-instance random seed from `/dev/urandom` to mitigate hash-flooding attacks
- **Auto-resize** to the next prime ≥ 2× capacity when load factor reaches 1; existing entries are relinked using the cached hash, no recomputation
- **Binary persistence**: `ht_destroy()` serialises all entries to file; `ht_load()` restores them on startup

### HTTP Interface

Three routes are supported via query parameters:

| Route | Parameters | Success | Error |
|---|---|---|---|
| `/get` | `key=<k>` | `200 {"value":"..."}` | `404` key not found / `400` missing key |
| `/set` | `key=<k>&val=<v>` | `200 stored` | `400` bad params / `500` internal error |
| `/delete` | `key=<k>` | `200 value deleted` | `404` key not found / `400` missing key |

All keys and values are sanitised: percent-encoded sequences are decoded and every character must be printable before the request reaches the hash table.

### Keepalive & Timers

Each client gets a `CLOCK_MONOTONIC` timerfd armed to `KEEPALIVE_TIMEOUT` seconds (default 5). On every successful request the timer is re-armed. When it fires, the connection is closed immediately. The `Connection: keep-alive` header in the request controls whether the server resets the timer or closes after the response.

---

## Build

```bash
gcc -O2 -Wall -Wextra -o kvengine \
    server_functions.c hash_table.c route_handler.c \
    -lpthread
```

---

## Usage

```bash
# Start with an empty table
./kvengine

# Load from file on startup, save to the same file on shutdown
./kvengine data.bin

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
```

Shut down cleanly with **Ctrl+C**: the SIGINT handler clears `keep_running`, the event loop drains, all open connections are closed, and the table is persisted if a save path was provided.

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

---

## Branch Overview

| Branch | Concurrency model |
|---|---|
| `master` ← you are here | Single-threaded, epoll, event-driven |
| [`feature/threadPool`](../../tree/feature/threadPool) | Multi-threaded, fixed thread pool + FIFO task queue |