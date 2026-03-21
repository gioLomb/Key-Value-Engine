# Key-Value Engine — Thread Pool

A lightweight in-memory key-value store exposed over HTTP, written in C. This branch implements a **multi-threaded architecture** built on a fixed-size thread pool and a FIFO task queue.

> Looking for the single-threaded event-driven version? See the [`master`](../../tree/master) branch.

---

## Architecture

The main thread accepts connections in a blocking loop and immediately hands each new socket off to the thread pool via `pool_submit()`. Worker threads sleep on a condition variable when the queue is empty and wake up one at a time as tasks arrive.

```
              main thread
                  │
             accept() loop
                  │
            pool_submit(fd)
                  │
     ┌────────────▼────────────┐
     │        TaskQueue        │   ← mutex-protected FIFO
     │  head → [T] → [T] → [T]│
     └────────────┬────────────┘
                  │  pthread_cond_signal()
       ┌──────────┼──────────┐
       ▼          ▼          ▼
   worker_0   worker_1  ... worker_N
       │
  handle_client_task()
       │
  handle_request()       ← route_handler
       │
  send_response()
       │
  close(socketFd)
```

Keepalive is handled entirely inside the worker: after sending a response, if the client requested `Connection: keep-alive` the worker loops on the same socket — reading the next request — without re-enqueuing it. The socket is closed and the task node freed only when the client disconnects or the keepalive loop ends.

Shutdown is cooperative: `pool_destroy()` sets a `shutdown` flag, broadcasts to all sleeping workers, and joins every thread. Workers drain the queue completely before exiting.

---

## Components

| File | Responsibility |
|---|---|
| `config.h` | Global constants and `#include` aggregation |
| `hash_table.c/h` | Thread-safe hash table with chaining, auto-resize, and binary persistence |
| `route_handler.c/h` | HTTP request parsing and dispatch to `/get`, `/set`, `/delete` |
| `threadPool.c/h` | Thread pool, task queue, worker lifecycle |
| `server_functions.c/h` | TCP socket setup, accept loop, signal handling, `main()` |

### Thread Pool

- **Fixed size**: the number of worker threads is set at creation time (`pool_create(threadCount, db)`) and never changes; this branch spawns **8 workers** by default
- **FIFO task queue** (`TaskQueue`): a singly-linked list with a `head` pointer for dequeuing and a `tail` pointer for O(1) enqueue; protected by a single `pthread_mutex_t`
- **Condition variable** (`pthread_cond_t`): workers block on it when the queue is empty; `pool_submit()` calls `pthread_cond_signal()` to wake exactly one worker per submitted task
- **Graceful shutdown**: `pool_destroy()` sets `shutdown = 1`, calls `pthread_cond_broadcast()` to unblock all waiting workers, then joins every thread; the queue is fully drained before any thread exits
- **Task ownership**: once a socket fd is submitted to the pool, the worker that picks up the task owns it — it is responsible for closing the fd and freeing the `Task` node

### Hash Table

- **String keys, generic values** — keys are NUL-terminated C strings; values are opaque byte buffers (`void *` + `size_t`)
- **Separate chaining** for collision resolution
- **Readers-writer lock** (`pthread_rwlock_t`): multiple concurrent reads, serialised writes — safe for use across all worker threads simultaneously
- **Pluggable hash function** via function pointer; per-instance random seed from `/dev/urandom` to mitigate hash-flooding attacks
- **Auto-resize** to the next prime ≥ 2× capacity when load factor reaches 1
- **Binary persistence**: `ht_destroy()` serialises all entries to file; `ht_load()` restores them on startup

### HTTP Interface

Three routes are supported via query parameters:

| Route | Parameters | Success | Error |
|---|---|---|---|
| `/get` | `key=<k>` | `200 {"value":"..."}` | `404` key not found / `400` missing key |
| `/set` | `key=<k>&val=<v>` | `200 stored` | `400` bad params / `500` internal error |
| `/delete` | `key=<k>` | `200 value deleted` | `404` key not found / `400` missing key |

All keys and values are sanitised: percent-encoded sequences are decoded and every character must be printable before the request reaches the hash table.

---

## Build

```bash
gcc -O2 -Wall -Wextra -o kvengine \
    server_functions.c hash_table.c route_handler.c threadPool.c \
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

Shut down cleanly with **Ctrl+C**: the SIGINT handler clears `keep_running`, the accept loop exits, `pool_destroy()` drains the queue and joins all workers, and the table is persisted if a save path was provided.

---

## Configuration

All tunables live in `config.h`:

| Constant | Default | Description |
|---|---|---|
| `PORT` | `8080` | Listening port |
| `KEEPALIVE_TIMEOUT` | `5` | Per-socket receive timeout (seconds) via `SO_RCVTIMEO` |
| `BUFFER_SIZE` | `1024` | Per-request read buffer (bytes) |
| `LISTEN_BACKLOG` | `1023` | TCP listen backlog |

The thread count (default **8**) is passed directly to `pool_create()` in `main()`.

---

## Branch Overview

| Branch | Concurrency model |
|---|---|
| [`master`](../../tree/master) | Single-threaded, epoll, event-driven |
| `feature/threadPool` ← you are here | Multi-threaded, fixed thread pool + FIFO task queue |
