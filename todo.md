parallelizzare alcune istruzioni ????

# Bug & Miglioramenti — C HTTP Server

[x] 4. Ottimizzazione della Hash Table
La tua tabella ha una capacità iniziale molto bassa (5). Anche se hai ht_resize, le collisioni saranno frequenti all'inizio.

Miglioramento:

Usa una dimensione iniziale più grande (es. 101 o 1024).

Importante: Assicurati che ht_resize utilizzi numeri primi per la capacità, riduce drasticamente le collisioni.

## 🔴 Bug Critici

- [x] **1. `ht_destroy` — use-after-free**
  - `free(table)` viene chiamato **prima** di `pthread_rwlock_destroy(&table->lock)`
  - File: `hash_table.c`
  - Fix: invertire l'ordine delle due chiamate

- [x] **2. Macro `get` inutilizzabile**
  - La macro `get(table, key, T)` fa `*(T*)ht_get(...)` ma `ht_get` restituisce `int`, non un puntatore
  - File: `hash_table.h`
  - Fix: rimuovere la macro o ridisegnarla in modo coerente con l'API

- [x] **3. `strtok` non thread-safe in `handle_request`**
  - `strtok` è rientrante: con più thread concorrenti che parsano richieste contemporaneamente c'è race condition
  - File: `route_handler.c`
  - Fix: sostituire con `strtok_r`

- [x] **4. `write` senza controllo del valore di ritorno in `send_response`**
  - Se la write fallisce parzialmente (es. client disconnesso) non viene rilevato nulla
  - File: `server_functions.c`
  - Fix: controllare il return value e gestire l'errore

---

## 🟠 Problemi di Sicurezza / Robustezza

- [x] **5. `ht_resize` chiamato con lock già acquisito — invariante non documentata**
  - `ht_set` tiene il write lock e chiama `ht_resize` che non prende lock: corretto **solo** perché è privato
  - Se qualcuno chiamasse `ht_resize` dall'esterno avrebbe un deadlock
  - File: `hash_table.c`, `hash_table.h`
  - Fix: rendere `ht_resize` statico o documentare esplicitamente il vincolo

- [x] **6. Nessuna validazione dell'input nei parametri URL**
  - Chiavi e valori arrivano dalla rete senza sanitizzazione (caratteri speciali, binari, encoding)
  - File: `route_handler.c`
  - Fix: aggiungere validazione (es. solo caratteri stampabili, lunghezza minima > 0)

- [x] **7. `BUFFER_SIZE` condiviso tra request e response**
  - Request e response usano lo stesso `BUFFER_SIZE` (1KB): per valori vicini a `MAX_VALUE_SIZE` (1MB) la risposta viene troncata silenziosamente
  - File: `config.h`, `server_functions.c`, `route_handler.c`
  - Fix: introdurre una costante separata `RESPONSE_BUFFER_SIZE` più grande

---

## 🟡 Problemi di Design / Qualità

- [x] **8. Label `success` irraggiungibile in `ht_delete`**
  - Non c'è nessun `goto success` esplicito: il flusso ci cade per caso, il codice è confuso e fragile
  - File: `hash_table.c`
  - Fix: eliminare i goto e riscrivere con early return

- [x] **9. Response non è JSON valido**
  - La risposta del `/get` produce `{hello}` invece di un JSON ben formato
  - File: `route_handler.c`
  - Fix: formattare come `{"value":"hello"}` o simile

- [x] **10. `analyze_args` non gestisce duplicati o ordini insoliti**
  - `./server -l a.db -l b.db` sovrascrive `idxLoad` silenziosamente
  - File: `server_functions.c`
  - Fix: aggiungere controllo duplicati e stampare un errore

---

## 🟢 Miglioramenti Minori / Stile

- [x ] **11. `int addrLen` invece di `socklen_t` in `server_loop`**
  - Mismatch di tipo con `accept()`
  - File: `server_functions.c`
  - Fix: dichiarare `socklen_t addrLen`

- [x] **12. Magic number `3` in `listen()`**
  - `listen(server_fd, 3)` — il backlog dovrebbe essere una costante nominata
  - File: `server_functions.c`, `config.h`
  - Fix: aggiungere `#define LISTEN_BACKLOG 3` in `config.h`

- [ ] **13. Nessun logging strutturato**
  - I messaggi vanno su stdout/stderr senza timestamp o livello (INFO/ERROR)
  - Fix: aggiungere una semplice macro di log con timestamp

- [x ] **14. Nessun test**
  - Aggiungere test basilari (es. script `curl` o piccolo test in C) migliorerebbe la manutenibilità
  - Fix: creare un `test.sh` con i casi principali (get/set/delete, chiavi mancanti, ecc.)