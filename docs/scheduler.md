# Scheduler API

Include `<threadforge/scheduler.hpp>` and link `threadforge::threadforge`.

## Construction

```cpp
threadforge::Scheduler automatic;
threadforge::Scheduler four_workers(4);
threadforge::Scheduler configured(threadforge::Config{4});
```

The default configuration uses the reported hardware concurrency with a one-worker
fallback. Zero workers throws `std::invalid_argument`. Thread/resource allocation
failures propagate, with already-created workers shut down before construction
unwinds. Scheduler objects are neither copyable nor movable.

`worker_count()` returns the configured count, including after shutdown.

## Submission and futures

`submit(callable, args...)` returns `std::future<Result>`, including `void` and
reference result types. Callable and argument storage follows decayed value
semantics. The callable and arguments are moved for their single invocation;
explicit `std::ref` arguments preserve references. Callables must be invocable
with the stored argument types and their intended move/reference semantics.

```cpp
auto value = scheduler.submit([](int a, int b) { return a + b; }, 10, 20);
auto moved = scheduler.submit(
    [](std::unique_ptr<int> p) { return *p; }, std::make_unique<int>(42));
```

Keep captured pointers, reference arguments, and reference results valid for their
required lifetime. Submission is safe from multiple external threads and from
worker threads while the scheduler accepts work. Nested tasks use the global
queue in Phase 1. A task submitted during graceful draining is rejected.

An allocation failure during submission throws without accepting that task.
Submission after closure throws `std::runtime_error`. Exceptions thrown by an
accepted callable propagate through `future.get()`. Ignoring or destroying a
future neither blocks the submitting thread nor cancels queued work.

## Graceful shutdown

`shutdown()` closes submissions and waits for all accepted work and workers to
finish. External callers may invoke it concurrently or repeatedly. A worker of
the same scheduler receives `std::logic_error` if it invokes this method.

`is_shutdown()` means the submission boundary has closed; it can become true
before all running tasks or an external `shutdown()` call have returned. It does
not provide a separate termination barrier. Calling `shutdown()` does.

The destructor drains and joins automatically. The owner must destroy the object
outside its own workers, after other API users have stopped using it. Calling an
API concurrently with destruction violates the object's lifetime contract.

No immediate shutdown, cancellation, priority, or dependency API exists in Phase 1.
