.. SPDX-License-Identifier: GPL-2.0

=======================
Cache Aware Scheduling
=======================

Overview
========

On a machine with several last level caches (LLCs) - typically one per
socket or per CPU cluster - it can pay off to keep a set of cooperating
tasks on CPUs that share one LLC, so that the data they pass between each
other stays cache hot instead of bouncing across the interconnect.

The scheduler tracks, per group of tasks, how much runtime the group spends
on each LLC and nudges the group's members towards the LLC where it is most
active, as long as that LLC is not already overcommitted. This is enabled by
CONFIG_SCHED_CACHE.

The unit that is aggregated is a *cache group* (struct sched_cache_group).
By default every address space (mm) gets its own cache group, so the threads
of a process are aggregated together and nothing else is. That default is a
good fit for a classic multi-threaded process, but not for every workload:

  - A workload split across cooperating *processes* rather than threads - a
    database with a process per connection, a browser with a renderer per
    site, a server and its worker helpers - shares data through shared memory
    or pipes but never shares an mm, so the default never aggregates it.

  - A process whose threads do not actually share data is aggregated anyway,
    just because the threads live in one address space.

To cover those cases a process can manage cache group membership explicitly
through prctl(2).

The prctl() interface
=====================

::

    int prctl(int option, unsigned long subop, unsigned long pid,
              unsigned long arg4, unsigned long pid_type);

with ``option`` set to ``PR_SCHED_CACHE``. The remaining arguments are:

``subop``
    Which operation to perform (see below).

``pid``
    The task the operation applies to. ``0`` means the calling task.

``arg4``
    Only used by ``PR_SCHED_CACHE_GET`` (output pointer) and
    ``PR_SCHED_CACHE_SHARE_FROM`` (source pid). Must be ``0`` for every
    other sub-operation.

``pid_type``
    One of ``PIDTYPE_PID``, ``PIDTYPE_TGID`` or ``PIDTYPE_PGID`` (0, 1, 2).
    It selects whether the operation affects just the named thread, its whole
    thread group, or its process group. ``PR_SCHED_CACHE_GET`` requires
    ``PIDTYPE_PID``.

The cache group itself is a kernel object. User space never invents or
passes a group id; it only names tasks by pid and asks the kernel to create
a group or to copy one task's group onto another. This follows core
scheduling, where the cookie is a kernel object and the *_GET operation only
returns an obfuscated identifier.

Sub-operations
--------------

``PR_SCHED_CACHE_GET``
    Read back the calling-convention cookie id of ``pid``. ``arg4`` is a
    ``__u64 __user *`` that must be 8-byte aligned; the kernel writes an
    obfuscated hash of the task's cache group there (0 if the task currently
    has no group). ``pid_type`` must be ``PIDTYPE_PID``.

``PR_SCHED_CACHE_CREATE``
    Allocate a fresh cache group and install it on the target task(s). This
    operation is *not* idempotent: each call creates a new group. A caller
    that wants several tasks in one group should ``CREATE`` once and then
    ``SHARE_FROM`` for the rest.

``PR_SCHED_CACHE_SHARE_FROM``
    Copy the cache group of the task named by ``arg4`` (the source) onto the
    task(s) named by ``pid``. This is how a task joins an existing group.

``PR_SCHED_CACHE_DISABLE`` / ``PR_SCHED_CACHE_ENABLE``
    Opt the target's cache group out of / back into LLC aggregation. Because
    the flag lives on the (shared) group, changing it for one member changes
    it for every task in the group.

Permissions
-----------

The caller must be able to ``ptrace_may_access(PTRACE_MODE_READ_REALCREDS)``
every task it touches. For the ``PIDTYPE_TGID`` and ``PIDTYPE_PGID`` scopes
the access of *all* tasks in the group is checked before *any* task is
changed, so the operation either applies to the whole group or fails with
-EPERM without touching anyone. ``PR_SCHED_CACHE_SHARE_FROM`` additionally
requires access to the source task.

Kernel threads cannot be targeted.

Return value
------------

Returns 0 on success. On error one of:

``-EINVAL``
    Unknown sub-operation, out-of-range ``pid``/``pid_type``, ``arg4`` given
    for a sub-operation that does not take one, misaligned ``GET`` pointer,
    ``GET`` with a ``pid_type`` other than ``PIDTYPE_PID``, or the target is
    a kernel thread.

``-ESRCH``
    The target (or, for ``SHARE_FROM``, the source) task does not exist.

``-EPERM``
    The caller is not allowed to access the target (or source) task.

``-ENOENT``
    The source task (``SHARE_FROM``) or target task (``DISABLE``/``ENABLE``)
    has no cache group.

``-ENOMEM``
    ``CREATE`` could not allocate a new group.

``-EFAULT``
    ``GET`` could not write to the ``arg4`` pointer.

Interaction with the system-wide policy
=======================================

A system-wide policy composes with the per-task hint the same way THP does.
It is set through debugfs::

    /sys/kernel/debug/sched/llc_balancing/enabled

and takes one of three modes:

``always``
    Always aggregate, ignoring the per-group disable flag.

``advise``
    Honour the per-group flag set through ``PR_SCHED_CACHE_DISABLE`` /
    ``PR_SCHED_CACHE_ENABLE`` (the default).

``never``
    Disable cache aware scheduling entirely.

For backward compatibility the knob still accepts the old boolean spelling on
write (``1``/``y``/``on`` map to ``always``, ``0``/``n``/``off`` to
``never``); reads show ``[always] advise never`` with the active mode in
brackets.

Example
=======

Put a helper process into the same cache group as its parent::

    /* In the parent, once per instance. */
    prctl(PR_SCHED_CACHE, PR_SCHED_CACHE_CREATE, 0, 0, PIDTYPE_TGID);

    /* In (or on behalf of) each helper. */
    prctl(PR_SCHED_CACHE, PR_SCHED_CACHE_SHARE_FROM,
          helper_pid, parent_tgid, PIDTYPE_PID);
