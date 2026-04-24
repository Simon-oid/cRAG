# The Linux CFS Scheduler

The Completely Fair Scheduler (CFS) was introduced in Linux 2.6.23 in 2007
by Ingo Molnar, replacing the older O(1) scheduler. It is the default
scheduler for normal (SCHED_NORMAL) tasks on Linux. In kernel 6.6 CFS was
superseded by EEVDF (Earliest Eligible Virtual Deadline First), but CFS
remains the foundation most programmers still refer to.

## Core Idea

CFS does not use fixed time slices. Instead it tracks, for every runnable
task, a quantity called vruntime: the amount of virtual runtime the task
has consumed. The scheduler always picks the runnable task with the
smallest vruntime next. Tasks that run consume vruntime; tasks that sleep
do not. Over time, every runnable task receives a fair share of the CPU
proportional to its weight.

## Red-Black Tree

Runnable tasks are stored in a per-CPU red-black tree keyed by vruntime.
The leftmost node in the tree is the task with the smallest vruntime and
is therefore next to run. Insertion, deletion, and leftmost-node lookup
are all O(log n). This replaced the array-of-priority-queues used by the
O(1) scheduler.

## Weights and Nice Values

Each task has a nice value from -20 (highest priority) to +19 (lowest).
Nice values map to weights via a lookup table such that every unit of nice
corresponds to roughly a 10 percent change in CPU share. Vruntime is
advanced by (delta_exec * NICE_0_LOAD) / task_weight, so heavier tasks
accumulate vruntime more slowly and get more wall-clock CPU.

## Scheduling Classes

CFS is one of several scheduling classes in the kernel. The class hierarchy
from highest to lowest priority is: stop, deadline (SCHED_DEADLINE), real
time (SCHED_FIFO and SCHED_RR), fair (CFS / EEVDF), and idle. The scheduler
core asks each class in order for a runnable task.

## Tunables

The main sysctl knobs historically exposed by CFS were
kernel.sched_latency_ns, kernel.sched_min_granularity_ns, and
kernel.sched_wakeup_granularity_ns. They controlled the target latency
window and the minimum time a task runs before it can be preempted. Most
were removed or hidden behind debugfs in recent kernels as EEVDF replaced
the internals.

## Why It Matters

Understanding CFS helps you reason about latency spikes, CPU-bound vs
I/O-bound task behavior, and why nice sometimes does not feel like it is
doing what you expect. The fair share is computed over a sliding window,
not instantaneously.
