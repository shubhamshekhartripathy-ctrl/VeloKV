# Milestone 8 — Leader–Replica Replication: Implementation Plan

> Saved as: `d:\redis\milestone_8_implementation_plan.md`

---

## Overview

Milestone 8 adds **asynchronous, single-leader replication** to RapidKV.
One instance runs as **Leader** (accepts all writes) and one or more run as **Replicas** (maintain a live read-only copy).  This mirrors the core of Redis's built-in replication subsystem.

---

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                      LEADER (port 6379)                      │
│                                                              │
│   ┌──────────────┐    ┌──────────────────────────────────┐   │
│   │   Database   │◄───│  CommandProcessor                │   │
│   └──────┬───────┘    │   SET/DEL/EXPIRE → local apply   │   │
│          │            │   → repl_mgr.propagate(cmd)      │   │
│          │            └──────────────────────────────────┘   │
│          │                          │                        │
│          │            ┌─────────────▼───────────────────┐    │
│          └───────────►│  ReplicationManager (Leader)    │    │
│                       │   • add_replica(fd, db)         │    │
│                       │   • build_full_sync(db)         │    │
│                       │   • propagate(cmd)              │    │
│                       │   • flush_replica / reg_fds     │    │
│                       └────────────┬────────────────────┘    │
└────────────────────────────────────│────────────────────────┘
                                     │ TCP  (same port 6379)
              ┌──────────────────────┴────────────────────────┐
              │                 REPLICA (port 6380)            │
              │                                               │
              │   ┌──────────────┐   ┌─────────────────────┐  │
              │   │   Database   │◄──│ ReplicationManager  │  │
              │   │  (read-only) │   │  (Replica)          │  │
              │   └──────────────┘   │  • connect_to_leader│  │
              │                      │  • read_from_leader │  │
              │                      │  • try_reconnect    │  │
              │                      └─────────────────────┘  │
              └───────────────────────────────────────────────┘
```

---

## File Map

| File | Status | Purpose |
|---|---|---|
| `src/replication.hpp` | **NEW** | `NodeRole`, `ReplicaConnection`, `ReplicationManager` |
| `src/replication.cpp` | **NEW** | Full-sync, propagation, reconnect, apply-leader-line |
| `src/client.hpp` | MODIFY | Added `release()` — steal fd without closing socket |
| `src/client.cpp` | MODIFY | Implemented `release()` |
| `src/command.hpp` | MODIFY | Added `is_write_command()` declaration |
| `src/command.cpp` | MODIFY | Implemented `is_write_command()` |
| `src/server.hpp` | MODIFY | Added `role_`, `repl_mgr_`, `rdb_path_`; updated constructor |
| `src/server.cpp` | MODIFY | REPLICAOF detection, READONLY guard, propagate writes, repl FDs in select() |
| `src/main.cpp` | MODIFY | CLI arg parsing: `--role`, `--port`, `--rdb`, `--leader-host`, `--leader-port` |
| `Makefile` | MODIFY | Added `src/replication.cpp` to `SRCS` |

---

## Design Decisions

### 1. Single-threaded / no new threads

The entire replication I/O is integrated into the **existing select() event loop**.
Replica socket FDs are added to `fd_set` alongside client FDs.  No mutexes.

```
Why not a background thread?
  Thread → needs mutex around Database → lock contention on every command
  select() integration → zero mutexes, same event-loop thread, no races
```

### 2. Inline text replication protocol

```
Handshake (replica → leader):
  REPLICAOF self self\r\n

Full sync (leader → replica):
  +FULLSYNC\r\n
  REPLSET <enc_key> <enc_value> <expiry_ms>\r\n   (one per string key)
  REPLLIST <enc_key> <expiry_ms> <n> <e0> ...\r\n  (one per list key)
  +FULLSYNCDONE\r\n

Live propagation (leader → replica):
  SET <enc_key> <enc_value>\r\n
  DEL <enc_key>\r\n
  EXPIRE <enc_key> <seconds>\r\n
  LPUSH / RPUSH / LPOP / RPOP  (same pattern)
```

All tokens are **percent-encoded** (`%20` = space, `%0A` = newline, `%25` = %).

### 3. Single-port handshake detection

Replicas connect to the **same TCP port as clients**.  Their first message `REPLICAOF self self\r\n` is detected in `handle_client_read()`.  The connection is **promoted** (fd stolen from Client via `release()`) and handed to ReplicationManager.

### 4. FD promotion pattern

```cpp
// In Server::handle_client_read():
socket_t rfd = client.release(); // sets client.fd_ = INVALID — no socket close
clients_.erase(it);              // ~Client() runs but fd_ is INVALID → safe
repl_mgr_->add_replica(rfd, db_);
return;
```

### 5. Propagation ordering guarantee

Live commands are appended to `output_buffer` AFTER the full-sync payload.
TCP delivers the buffer in order → replica always sees: full-sync → live commands.
No race condition regardless of when live writes arrive during sync delivery.

### 6. READONLY enforcement

All write commands (`SET`, `DEL`, `EXPIRE`, `LPUSH`, `RPUSH`, `LPOP`, `RPOP`) are detected via `is_write_command()`.  On a replica, they return:
```
-READONLY You can't write against a read only replica.\r\n
```

---

## Request Flow (Leader write)

```
1. Client → TCP → handle_client_read()
2. RESPParser::parse() → Command = {"SET", "key", "value"}
3. is_write_command("SET") → true
4. READONLY check → skipped (this is the leader)
5. processor_.execute(cmd, db_) → db_.set("key","value") → "+OK"
6. repl_mgr_->propagate(cmd)
   → encode_command(cmd) → "SET key value\r\n"
   → appended to each ReplicaConnection::output_buffer
7. Response sent to client
8. Next select() iteration: FD writable → flush_replica() drains buffer to replica
```

## Replication Flow (Full Sync)

```
1. Replica process starts → connect_to_leader("127.0.0.1", 6379)
2. Sends: "REPLICAOF self self\r\n"
3. Leader: handle_client_read() detects "REPLICAOF"
   a. client.release() → steals fd
   b. clients_.erase(it) → safe (fd is INVALID)
   c. repl_mgr_->add_replica(rfd, db_)
      → build_full_sync(db_) → serialise all live keys
      → queue into ReplicaConnection::output_buffer
4. Leader event loop: select() sees rfd writable → flush_replica(rfd)
5. Replica: read_from_leader() → apply_leader_line() for each line:
   +FULLSYNC        → full_sync_in_progress_ = true
   REPLSET key val  → db_.set(key, val) [+ expire_at if TTL]
   REPLLIST key...  → db_.del(key); rpush each element
   +FULLSYNCDONE    → full_sync_in_progress_ = false
6. Live commands continue flowing
```

## Reconnect Flow

```
1. Leader crashes / network loss
2. Replica: read_from_leader() → recv() returns 0
3. close_socket(leader_fd_); leader_connected_ = false; last_reconnect_ms_ = now
4. Every select() iteration: try_reconnect() checks elapsed > 5000ms
5. After 5s: connect_to_leader() → REPLICAOF handshake → new full sync
```

---

## Consistency Guarantees

| Property | This Implementation | Real Redis |
|---|---|---|
| Model | Eventual consistency | Eventual (async replication) |
| Write ACK | Before replicas confirm | Same (WAIT can force sync ACK) |
| Read-your-writes (cross-node) | Not guaranteed | Not guaranteed |
| Command ordering | TCP guarantees in-order | Same |
| Partial resync | ❌ Full re-sync only | ✅ PSYNC + backlog |
| Failover | Manual | Automatic (Sentinel / Cluster) |
| TTL drift | Slight (relative EXPIRE) | None (absolute EXPIREAT) |

---

## Trade-offs vs Real Redis

| Aspect | RapidKV M8 | Real Redis |
|---|---|---|
| Sync format | Inline text (human-readable) | RDB binary (compact, fast) |
| Propagation wire format | Inline text | RESP arrays |
| Reconnect strategy | Full sync | Partial resync (PSYNC) |
| Replica ID | None | 40-char random ID for PSYNC |
| Replication offset | None | Monotonic counter for tracking lag |
| Multi-replica | ✅ Supported | ✅ Supported |
| Replica-of-replica | ❌ | ✅ |
| Read scaling | ✅ (reads served from replica) | ✅ |
| Write isolation | ✅ (READONLY enforced) | ✅ |

---

## Interview Questions & Answers

**Q1: What is the difference between synchronous and asynchronous replication?**

A: In **synchronous** replication, the leader waits for at least one replica to confirm receipt of a write before acknowledging the client.  In **async** replication (what we implement, and what Redis uses by default), the leader acks immediately and propagates in the background.  Async is faster but may lose the most recent writes if the leader crashes before replicas catch up.  Redis's `WAIT numreplicas timeout` command can force synchronous acknowledgement.

**Q2: How does Redis implement partial resync (PSYNC)?**

A: Every Redis leader maintains a circular **replication backlog** (fixed-size ring buffer of recent write commands) and a monotonically increasing **replication offset**.  Each replica tracks its own offset.  On reconnect, if the replica's offset falls within the backlog range, the leader sends only the missing commands (partial sync).  If the offset has fallen off the ring, a full sync is triggered.  We don't implement this — every reconnect triggers a full sync.

**Q3: Why does the leader propagate EXPIRE using relative seconds but real Redis uses EXPIREAT?**

A: When the leader propagates `EXPIRE key 30`, the replica re-starts the 30-second timer from ITS current clock.  Due to network latency, the replica's effective TTL will be slightly shorter.  Real Redis propagates `EXPIREAT key <unix_ms_timestamp>` (absolute deadline) so all nodes agree on the exact expiry time regardless of when the command arrives.

**Q4: What is the FD promotion pattern and why is it needed?**

A: When a replica connects, it starts as a regular client (a `Client` object in `clients_`).  After we detect the REPLICAOF handshake, we need to hand the socket FD to `ReplicationManager`.  Simply erasing the `Client` would close the socket (via `~Client()`), breaking the connection.  `Client::release()` sets `fd_ = INVALID_SOCKET_VAL` so the destructor skips `close_socket()`, and returns the original FD for the caller to use — a pattern equivalent to `unique_ptr::release()`.

**Q5: Why use the same port for clients and replicas?**

A: Real Redis uses a separate replication port (leader_port + 10000 by convention in some setups, or Redis Sentinel uses a separate channel).  We use the same port with a magic handshake keyword to avoid managing two listening sockets, simplifying the Server implementation.  The trade-off is that a misbehaving client that sends "REPLICAOF" would accidentally be promoted — acceptable for an educational clone.

**Q6: How does the leader handle a slow replica?**

A: In our implementation, the leader simply queues data in `output_buffer` and flushes it as fast as the socket allows via the select() loop.  There is no buffer limit.  Real Redis enforces `client-output-buffer-limit replica`: if the queue exceeds a threshold, the connection is forcibly closed and the replica must re-sync.

**Q7: Can a replica serve stale reads?**

A: Yes — this is the fundamental trade-off of async replication.  If a client writes `SET x 1` to the leader and immediately reads `GET x` from a replica, it may see the old value (or nil) until the write is propagated.  Applications requiring read-your-own-writes should always read from the leader, or use `WAIT 1 0` to ensure at least one replica is in sync.

**Q8: What happens to replicas when the leader crashes?**

A: In our implementation, replicas detect the disconnect, enter a 5-second reconnect backoff loop, and keep serving reads from their local database.  They will NOT automatically become the new leader.  In production Redis, **Redis Sentinel** monitors leaders and replicas; when the leader fails it promotes the most up-to-date replica automatically (via a leader election protocol).  Redis Cluster has built-in automatic failover.

**Q9: Why is the replication backlog a ring buffer, not a queue?**

A: A queue would grow unboundedly for slow or disconnected replicas, consuming memory.  A fixed-size ring buffer has O(1) write and O(1) eviction.  When a replica's offset falls off the ring (it has been disconnected too long), it performs a full re-sync — a deliberate trade-off between memory and re-sync cost.

**Q10: How does RapidKV ensure the full-sync and live commands arrive in the correct order?**

A: By appending live propagated commands to `output_buffer` AFTER the full-sync payload.  Since TCP is a byte stream that delivers data in order, the replica always processes: `+FULLSYNC ... REPLSET ... +FULLSYNCDONE ... SET key value ...` in that exact sequence, regardless of when live writes arrive at the leader while the sync data is still being drained.
