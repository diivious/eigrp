# OW-04: OpenWrt runtime and scheduler

Copyright (C) 2026 Donnie V. Savage

## Implementation

The OpenWrt-owned scheduler is implemented in:

- `openwrt/code/eigrp_owrt_runtime.c`
- `openwrt/code/eigrp_owrt_runtime.h`

The module implements the scheduler, event, timer, time, and work-queue
functions from `eigrp/code/eigrp_sys.h`. The required portable API functions
remain named **`eigrp_sys_*`**. Every private shim function, type, and source
file uses the short **`eigrp_owrt_`** prefix.

Host-neutral POSIX primitives are used so the scheduler can also be compiled
and tested on macOS and Debian before an OpenWrt cross-build. It is not a
packet I/O or RIB adapter and does not claim that a runnable daemon exists.

## Event architecture

```text
OpenEIGRP protocol / host control                      OpenWrt event thread
--------------------------------                      --------------------
eigrp_sys_event_add() ---+                          +-- poll() readiness
                        |                          |   or timer expiry
 eigrp_sys_timer_add() --+                          |
                        v                          v
                 +------------------------------+
                 | scheduler mutex              |
                 | event list (id + owner)      |
                 | per-instance fd bindings     |
                 +---------------+--------------+
                                 |
                       nonblocking wake pipe
                                 |
                                 v
                 +------------------------------+
                 | acquire protocol lock        |
                 | take exactly one ready event |
                 | clear event owner handle     |
                 | release scheduler mutex      |
                 | execute callback             |
                 +------------------------------+
```

Each readiness registration is one-shot. The core re-registers the descriptor
when needed. Timers use `CLOCK_MONOTONIC`; timestamps use `CLOCK_REALTIME`.
Each event carries a monotonically advancing identity so a polled event that
was canceled and reallocated is not accidentally mistaken for its predecessor.

## Thread safety and ordering

```text
                        [protocol lock]
                              |
                              v
                       [scheduler lock]

              NEVER acquire in the reverse order.
```

- Callbacks execute on one event thread holding the protocol lock.
- Host code that mutates portable objects while callbacks may run wraps the
  mutation in `eigrp_owrt_runtime_enter()` / `eigrp_owrt_runtime_leave()`.
- The scheduler mutex protects event owners, queued events and FD bindings.
- Event callbacks are invoked *outside* the scheduler mutex.
- A caller must not destroy objects referenced by an in-flight callback
  without first synchronizing through the protocol lock.
- Do **not** call `eigrp_sys_runtime_finish()` while holding the protocol
  lock, or from the callback/event thread. The finish operation joins the
  event thread and would otherwise deadlock.

## FD readiness contract

```text
OW-06 packet socket (future)
       |
       v
 eigrp_owrt_runtime_fd_set(instance, fd)
       |
       +--> eigrp_sys_read_add() / eigrp_sys_write_add()
       |                       |
       |                       v
       |                  poll(fd)
       |                       |
       |                       v
       |                  one-shot callback
       |
       +--> eigrp_owrt_runtime_fd_clear(instance)
                       |
                       v
              cancel pending FD events
              for THAT instance only
```

Descriptor ownership remains with the packet adapter. The runtime does not
open, close, or duplicate protocol sockets. Rebinding a different descriptor
for the same instance removes its old readiness registrations. Closing an FD
requires first removing its binding and synchronizing with callbacks.

## Work queues

```text
 enqueue(data)
     |
     v
 immediate scheduler event
     |
     +--> SUCCESS  : remove item
     +--> REQUEUE  : move item to tail
     +--> BLOCKED  : retain head until a subsequent enqueue/clear
```

Queues use the protocol lock for operations that race with callbacks. The
optional delete callback is invoked for unprocessed data during queue clear
or free. Queue callbacks must not free the queue currently executing them.

## Lifecycle

```text
host calls eigrp_sys_runtime_init()
      |
      +-- create nonblocking wake pipe
      +-- start scheduler thread
      |
      v
portable eigrp_init() / configuration / receive worker (future daemon)
      |
      v
host stops new ingress; core eigrp_terminate()
      |
      +-- core cancels owners / destroys instances
      +-- core calls eigrp_sys_runtime_finish()
      |         |
      |         +-- stop scheduler and wake poll
      |         +-- join scheduler thread
      |         +-- cancel remaining events and clear owner pointers
      |         +-- release FD bindings and close wake pipe
      v
restart is supported through a new runtime_init()
```

Initialization and finish must be orchestrated from the host's control thread.
Repeated init/finish calls are idempotent. A stopped scheduler does not accept
new callback registrations. Shutdown cancels queued but not already completed
callbacks.

## Validation

```sh
bash openwrt/test/runtime-test.sh
bash openwrt/test/recipe-test.sh
```

The tests validate native compilation, events, timers, cancellation, FD
replacement and independent instance bindings, callback serialization, queue
semantics, shutdown, restart, and the package recipe. A full OpenWrt target
cross-build remains an independent acceptance gate performed on Aurora using:

```sh
./tools/openwrt.sh --build
```

The next modules are Linux interface inventory (OW-05) and IP protocol 88
packet sockets (OW-06). Neither belongs in the event scheduler.
