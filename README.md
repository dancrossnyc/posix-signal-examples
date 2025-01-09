# Demonstrate Using POSIX Signal Handlers With Thread Condition Variables

These programs demonstrate how to safely handle POSIX signals,
translating asynchronous signal delivery into events on POSIX
threads, specifically broadcasting on a condition variables.

## Self-Pipe Trick Demonstration: selfpipe.c

The "self-pipe trick" (https://cr.yp.to/docs/selfpipe.html)
writes to a "self-pipe" (that is, a pipe that is not shared
between processes in the usual way) in the signal handler, which
has the effect of turing asynchronous signal delivery into an IO
operation.

In our demonstration, we set up threads for each signal of
itnreest that block on reads from a per-signal pipe, and then
`pthread_cond_broadcast` on a per-signal condition variable
whenever those reads return.

## Synchronous sigsetjmp/siglongjmp for signal handlers

Demonstrates a technique where we direct all signals to a single
thread for handling.  On delivery, the signal handler for any
given signal will run on that thread, and we use siglongjmp to
transfer control to the top of the signal handling loop, where
it can switch and dispatch based on the received signal number.

Because all signals are handled on a single thread, and we know
that thread does not run non-async-signal-safe code except when
signals are blocked, we know that we can safely signal a
condition variable in this context.

## Demonstrate using sigwait and threads

This program demonstrates how to use POSIX `sigwait` to reflect
asynchronous signal delivery into a return from a blocking
synchronous call.  Again, we direct all signals to a single
thread that blocks signals.  When `sigwait` indicates a signal
was pending for the thread, we `pthread_cond_broadcast` on a
per-signal condition variable.
