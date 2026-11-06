.. SPDX-License-Identifier: GPL-2.0

======================
Cache Aware Scheduling
======================

1. Introduction
---------------

Cache Aware Scheduling gives the fair scheduler's load balancer the ability
to keep the threads of a process together in one Last Level Cache (LLC)
domain. Modern processors often split their LLC into several independent
slices: one per chiplet, per core complex, or per socket. Threads that share
data but are spread over different LLCs pay for each cache line they
exchange with a cross-die or cross-socket transfer, and each LLC holds its
own copy of the shared working set.

Cache aware scheduling watches where the threads of each process actually
run, picks a 'preferred LLC' for the process, and biases load balancing to
move the threads toward that LLC when this can be done without overloading
it. When the system is busy, or when the process is too large to fit in one
LLC, it steps aside and the generic load balancer decides alone.

This document gives an introduction to how cache aware scheduling works,
what the main design decisions behind it are, and how it can be tuned.

Before going any further, please note that::

   /!\ Cache aware scheduling is active only if at least one sched domain
       partition spans more than one LLC /!\

On platforms with a single LLC (for example a monolithic die where all cores
share one L3), there is nothing to aggregate. The feature then stays
disabled at run time, and its hooks are patched out by static keys.


2. Background and Terminology
-----------------------------

The generic load balancer spreads runnable load evenly and uses idle CPUs.
It knows the cache topology (sched domains carry the SD_SHARE_LLC flag), but
it does not know which tasks share data. Thus, a process with eight threads
on a machine with eight LLCs can easily end up with one thread in each LLC.
Each lock handoff, producer/consumer exchange or shared data update then
becomes a cross-LLC coherence transaction.

Cache aware scheduling supplies the missing information: these tasks belong
together, and this is the LLC where they already do most of their work.

The following terms are used in the rest of this document:

 - cache group: the unit of aggregation (struct sched_cache_group). Each
   address space (mm) has one cache group. All tasks that share the mm,
   that is all threads of a process, belong to it. A new group is attached
   to a task on exec(). Single-threaded processes are ignored, because
   there is nothing to co-locate.

 - occupancy: the share of a CPU's recent fair-class run time that was
   consumed by the tasks of one cache group.

 - epoch: the period at which the run time counters decay (by default
   HZ/100 jiffies, about 10 ms). Also the minimum interval between two
   scans of the same cache group.

 - preferred LLC: the LLC that the cache group currently wants its tasks to
   run on. It is identified by one CPU of that LLC (grp->cpu), and is -1 if
   there is no preference. Each task caches the id of its preferred LLC in
   p->preferred_llc.

Cache aware scheduling does not change the wake-up path. The wake-up path
already tends to place a task back on the LLC it last ran on, where its data
is cache hot. The load balancer is where threads get spread apart, so this
is where the bias is applied.


3. Topology information
-----------------------

When the sched domains are built or rebuilt, the scheduler checks whether
any partition has an LLC domain with a parent domain that spans more CPUs
(see sd_in_multi_llcs()). If so, the static key sched_cache_present is
enabled. A second static key, sched_cache_active, is enabled when
sched_cache_present is set and the user has not disabled the feature (see
Section 7.1). All hooks of cache aware scheduling are gated by
sched_cache_active.

An exclusive cpuset that is confined to a single LLC does not, by itself,
enable the feature. Because the static keys are global, the feature is on
for the whole system as soon as one partition spans several LLCs.

For each CPU, the bottom sched domain carries three additional fields:

 - llc_counts[]: a histogram indexed by LLC id. llc_counts[n] is the number
   of tasks queued on this CPU that prefer LLC n.
 - llc_max: the size of the llc_counts[] array.
 - llc_bytes: the size of the LLC in bytes, as seen by this CPU.

The LLC size comes from the generic cacheinfo framework. If the physical
LLC is shared by more CPUs than the sched domain spans (for example, when a
cpuset partition splits an LLC), the size is prorated by the ratio of the
domain's CPU count to the number of CPUs sharing the physical cache.
cacheinfo can be populated after the sched domains are first built. In this
case, sched_update_llc_bytes() fills in llc_bytes later, when CPUs come
online.

Each runqueue also counts the tasks that prefer some LLC (nr_llc_running),
and the runnable tasks that are queued on their preferred LLC
(nr_pref_llc_running).

Example 1.
    Let us consider a platform with 16 CPUs in one NUMA node, split in 2
    LLCs of 8 CPUs each::

	          CPUs:   0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15
	          LLCs:  |-----llc0------|---------llc1--------|
	          MC:    |-----sd mc-----|--------sd mc--------|
	          PKG:   |---------------sd pkg----------------|

    The MC domain has SD_SHARE_LLC set, and its parent (PKG) spans more
    CPUs, so sched_cache_present is enabled. Cache aware balancing is done
    at the PKG level, between the two LLC groups.

    If userspace instead creates two exclusive cpusets, one per LLC, each
    root domain spans a single LLC. sched_cache_present is then disabled
    and cache aware scheduling costs essentially nothing.


4. Choosing the preferred LLC
-----------------------------

4.1 - Measuring LLC occupancy
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Each time a fair-class task accrues run time, account_mm_sched() adds the
run time to two counters:

 - the cache group's per-CPU counter (pcpu_sched->runtime), and
 - the CPU's total counter (rq->cpu_runtime).

Both counters are halved at each epoch boundary. The occupancy of a CPU by
a cache group is then::

	                            group runtime on CPU
	occupancy = NICE_0_LOAD * -----------------------
	                           total runtime on CPU

Recent behaviour dominates, and old history fades out within a few epochs.

4.2 - Scanning
^^^^^^^^^^^^^^

From the scheduler tick, task_tick_cache() queues task_cache_work() as task
work on the current task, at most once per epoch per cache group. When the
work runs, only one thread of the group does the scan. A cmpxchg() on
grp->next_scan makes sure of this. The scan:

 1. Selects the candidate CPUs. With NUMA balancing active, the scan is
    limited to the task's preferred NUMA node, plus the node of the current
    preferred LLC and the node the task is running on. Without NUMA
    balancing, or if the task has no preferred node, all online CPUs are
    scanned.

 2. Adds up the occupancy of the group over all CPUs of each LLC. The sum,
    and not the average per CPU, is compared. Thus, a larger LLC that holds
    more of the group's threads wins over a smaller LLC that holds fewer
    threads, even if the smaller one is more dense.

 3. Selects the LLC with the highest total occupancy. grp->cpu is switched
    to it only if its occupancy is more than twice the occupancy of the
    current preferred LLC. This hysteresis keeps the preference stable and
    prevents ping-pong migrations. If there is no current preference, any
    LLC with non-zero occupancy is taken.

 4. Updates nr_running_avg, a moving average of the number of the group's
    threads that were running on the scanned CPUs at the time of the scan.

Example 2.
    Using the platform of Example 1, consider a process whose preferred
    LLC is currently llc1. The scan finds these per-CPU occupancies, given
    as a percentage of NICE_0_LOAD::

	          CPU:     0    1    2    3   4-7  |  8    9   10-15
	          occ:   100% 100% 100%  0%   0%   | 100% 50%   0%

	          llc0 total = 300%
	          llc1 total = 150%

    llc0 has the highest total, but 300% is not more than 2 * 150%, so the
    preferred LLC stays llc1. If the thread on CPU 9 later sleeps and the
    total for llc1 drops to 100%, the next scan moves the preference to
    llc0 (300% > 200%).

4.3 - NUMA balancing takes precedence
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Each task derives its preferred LLC from grp->cpu. If NUMA balancing is
active and the preferred LLC is on a different node than the task's
preferred NUMA node, the task gets no preferred LLC. NUMA placement always
takes precedence over LLC placement.


5. When the preference is withdrawn
-----------------------------------

The preferred LLC of a cache group is cleared (grp->cpu = -1) if any of the
following conditions holds. The tasks of the group are then balanced like
any other task.

  a. The group has not been scanned for more than epoch_affinity_timeout
     epochs (by default 5 epochs, about 50 ms). The data is stale, because
     the group became idle or its behaviour changed.

  b. Too many threads run at the same time. The check is::

	fits_capacity(nr_running_avg * SMT_threads, scale * llc_cpus)

     where llc_cpus is the number of CPUs in the LLC, SMT_threads is the
     number of hardware threads per core, and scale is derived from
     aggr_tolerance (see Section 7.2). fits_capacity() keeps a ~20% margin.
     For an LLC of 16 CPUs with SMT-2 and the default aggr_tolerance, at
     most 6 threads can run at the same time.

  c. The working set would not fit in one LLC. The check is::

	footprint * PAGE_SIZE > llc_bytes * (1 + (aggr_tolerance - 1) * 256)

     The footprint of a group is the sum of the NUMA balancing memory fault
     statistics of its tasks. It is subtracted again when a task exits.
     This estimate is only available when NUMA balancing is enabled.
     Without NUMA balancing, this check never triggers.

The thread and footprint checks are done during accounting, during the
scan, and again when the load balancer considers a cache aware migration.
A single-threaded process always fails the thread check.


6. Cache aware load balancing
-----------------------------

6.1 - LLC utilization
^^^^^^^^^^^^^^^^^^^^^

When the load balancer runs on a domain whose child is the LLC domain, it
records the utilization (the sum of cpu_util_cfs()) and the capacity of each
LLC group in the shared LLC data (struct sched_domain_shared). These values
are not updated by newly idle balancing. They are used by the migration
policy below.

6.2 - Migration policy
^^^^^^^^^^^^^^^^^^^^^^

Whether a task can move between two LLCs depends on the utilization of both
LLCs after the move. Two rules are used, both expressed as a percentage of
the LLC's capacity:

 - An LLC is 'busy' when its utilization is not below overaggr_pct (by
   default 50%). The 50% threshold prevents SMT-2 cores from saturating.
   On systems without SMT, the threshold is raised by 1.5x (75% by
   default).

 - An LLC is 'noticeably busier' than another one when its utilization is
   more than imb_pct (by default 20%) higher. 20% is slightly above the
   imbalance_pct of the LLC domain (17%), so that the two balancers do not
   fight each other.

Then, can_migrate_llc() returns one of three decisions:

 - G (mig_unrestricted): both LLCs are busy. Cache aware balancing does
   not interfere and the generic load balancer decides.

 - N (mig_forbid): the move is refused, because:

   * when moving toward the preferred LLC: the preferred LLC would become
     busy and noticeably busier than the source LLC, or
   * when moving out of the preferred LLC: the preferred LLC is not busy,
     or it is not noticeably busier than the destination LLC.

 - Y (mig_llc): the move is allowed for LLC locality.

The effect of imb_pct is that the preferred LLC can be up to imb_pct busier
than the other LLC before a task is moved out. Moving a task out of its
preferred LLC thus has a higher bar than moving it in.

Example 3.
    The tables below show the decisions with the default values
    (overaggr_pct = 50, imb_pct = 20, SMT system). The values are the LLC
    utilizations after the move, as a percentage of LLC capacity.

    Moving toward the preferred LLC (dst is the preferred LLC)::

	  src \ dst      30%  45%  50%  60%
	  30%            Y    Y    N    N
	  45%            Y    Y    Y    N
	  50%            Y    Y    G    G
	  60%            Y    Y    G    G

    Moving out of the preferred LLC (src is the preferred LLC)::

	  src \ dst      30%  45%  50%  60%
	  30%            N    N    N    N
	  45%            N    N    N    N
	  50%            Y    N    G    G
	  60%            Y    Y    G    G

    A task can move from a 45% LLC into its preferred LLC that becomes 50%
    busy, because 50% is within 20% of 45%. But a task in a 50% busy
    preferred LLC is not moved out to a 45% busy LLC, for the same reason.
    If it was, the next balance would move it back.

6.3 - Pulling tasks to their preferred LLC
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

When a CPU balances a domain that spans several LLCs, update_sg_lb_stats()
counts, for each remote group, how many of its tasks prefer the LLC of the
destination CPU (using llc_counts[]). If there are such tasks, and the
policy above returns Y for a move from that group to the destination, the
group is classified as group_llc_balance.

group_llc_balance ranks above all other group types except
group_overloaded, so a truly overloaded group is always handled first. For
a group_llc_balance busiest group, the balancer:

 - uses the migration type migrate_llc_task with an imbalance of one task,
 - picks the runqueue with the most tasks that prefer the destination LLC,
 - detaches only a task whose preferred LLC is the destination LLC,
 - if needed, kicks active balancing with the same restriction.

6.4 - Vetoing harmful migrations
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

During ordinary load balancing, migrate_degrades_llc() refuses to move a
task out of its preferred LLC when the policy returns N. Such refusals are
expected behaviour, so they set LBF_LLC_PINNED and do not increase
nr_balance_failed.

Active balancing does not evict a task from a CPU in its preferred LLC
(alb_break_llc()) when all runnable tasks on the source CPU are on their
preferred LLC, and either:

 - the source CPU runs only one task, or
 - moving the current task out would be refused by the policy above.

6.5 - Safety valves
^^^^^^^^^^^^^^^^^^^

The LLC bias gives way to other scheduler goals in these cases:

 - Repeated failures: when nr_balance_failed of a domain reaches
   cache_nice_tries + 1, all LLC constraints are dropped for that domain
   until balancing succeeds.

 - Asymmetric CPU capacity: misfit migration takes precedence. A group
   with a misfit task is not classified group_llc_balance. Cache aware
   balancing never pulls a task onto a CPU it does not fit, unless the task
   does not fit its source CPU either. Active balancing of misfit tasks is
   not blocked.

 - Asymmetric packing: moving tasks to idle, higher priority CPUs
   (SD_ASYM_PACKING) takes precedence.

 - Core scheduling: tasks with a core scheduling cookie are exempt from
   the migrate_degrades_llc() veto.

 - Moves inside one LLC are never restricted.


7. Tunables
-----------

The tunables are in debugfs, in /sys/kernel/debug/sched/llc_balancing/.
Like other files in /sys/kernel/debug/sched/, they are meant for
experimentation and are not a stable ABI.

7.1 - enabled
^^^^^^^^^^^^^

Default: always.

Selects how the per-group choice made with PR_SCHED_CACHE_ENABLE and
PR_SCHED_CACHE_DISABLE (Section 9) is applied, like the THP ``enabled``
modes and ``MADV_HUGEPAGE`` / ``MADV_NOHUGEPAGE``. It takes one of three
modes:

``always``
    Aggregate every group, except groups that opted out with
    ``PR_SCHED_CACHE_DISABLE`` (the default).

``advise``
    Aggregate only the groups that opted in with ``PR_SCHED_CACHE_ENABLE``.

``never``
    Disable cache aware scheduling entirely.

For backward compatibility the knob still accepts the old boolean spelling on
write (``1``/``y``/``on`` map to ``always``, ``0``/``n``/``off`` to
``never``); reads show ``[always] advise never`` with the active mode in
brackets.

The same three modes can be selected at boot time, before any process has
had a chance to use the prctl, with the kernel command line parameter::

    sched_cache={always|advise|never}

The debugfs knob can still be written afterwards to change the mode at
runtime.

The feature is actually active only if the platform also supports it
(Section 3). When sched_verbose is set (with the boot parameter, or with
/sys/kernel/debug/sched/verbose), the kernel logs the resulting state each
time it is evaluated, that is on writes to this file and on sched domain
rebuilds::

	# echo never > /sys/kernel/debug/sched/llc_balancing/enabled
	# dmesg | grep 'cache aware scheduling'

7.2 - aggr_tolerance
^^^^^^^^^^^^^^^^^^^^

Default: 1.

Sets how large a cache group can be and still be aggregated. It scales the
limits of Section 5 (b) and (c):

  ===============  ============================  ===========================
  aggr_tolerance   thread limit                  footprint limit
  ===============  ============================  ===========================
  0                zero: never aggregate         zero: never aggregate
  1                ~80% of the LLC's CPUs        LLC size
  N (2 to 99)      N times the value for 1       (1 + (N - 1) * 256) times
                                                 the LLC size
  100 or more      no limit                      no limit
  ===============  ============================  ===========================

For example, with a 32 MB LLC, aggr_tolerance = 2 allows a footprint of
up to 257 * 32 MB (about 8 GB). Values from 2 upward thus make the
footprint check mostly irrelevant, and the thread limit becomes the main
constraint.

Raise aggr_tolerance when a group has a few more active threads than one
LLC can hold, or a large memory footprint but a small shared hot data set.
This matters most on platforms with small LLCs. Before you use large values
in production, measure memory bandwidth bound phases of the workload,
because one LLC gives less memory bandwidth than the whole node.

7.3 - overaggr_pct
^^^^^^^^^^^^^^^^^^

Default: 50 (percent of LLC capacity).

The utilization at which an LLC is 'busy' (Section 6.2). A busy LLC stops
attracting tasks. Higher values pack tasks more aggressively. Lower values
(for example 30 to 40) reduce packing if run queue latency or throughput
gets worse. On systems without SMT, the value is multiplied by 1.5.

7.4 - imb_pct
^^^^^^^^^^^^^

Default: 20 (percent).

The hysteresis margin between the preferred and the non-preferred LLC
(Section 6.2). Higher values make tasks stay longer in their preferred LLC
and reduce bouncing between LLCs. Lower values react faster to load
changes.

7.5 - epoch_period
^^^^^^^^^^^^^^^^^^

Default: HZ/100 (in jiffies).

The decay period of the occupancy counters, and the minimum interval
between two scans of a cache group. Note that the unit is jiffies, so the
default is 10 ms with HZ=1000 and HZ=100, but 8 ms with HZ=250. A value of
0 is treated as 1. Longer values give smoother but slower preferences, and
less scan overhead.

7.6 - epoch_affinity_timeout
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Default: 5 (epochs).

How many epochs a preferred LLC survives without a new scan (Section 5
(a)). Raise it for bursty workloads that become idle for short periods and
lose their preference too often.

7.7 - Tuning guidance
^^^^^^^^^^^^^^^^^^^^^

 1. Start with the default values, and compare against enabled = never. Keep
    everything else (cpusets, NUMA balancing, cpufreq governor) the same.
 2. If the threads of a process are not aggregated because the process is
    slightly too large, try aggr_tolerance = 2.
 3. If packing causes queueing or lower throughput, lower overaggr_pct.
 4. If tasks bounce between LLCs, raise imb_pct and/or epoch_period.
 5. If the preference is lost for bursty workloads, raise
    epoch_affinity_timeout.
 6. Where the application lets you set the size of its thread pool,
    matching it to the number of cores per LLC often gives more than any
    kernel tunable.

To see what the kernel uses as LLCs, look at the shared_cpu_list of the
highest cache index of each CPU::

	# grep . /sys/devices/system/cpu/cpu*/cache/index3/shared_cpu_list


8. Dependencies and requirements
--------------------------------

8.1 - Kernel configuration
^^^^^^^^^^^^^^^^^^^^^^^^^^

Cache aware scheduling is built with CONFIG_SCHED_CACHE=y (default y,
depends on SMP). The tunables need CONFIG_DEBUG_FS. CONFIG_NUMA_BALANCING
is recommended (see Section 8.4).

8.2 - Multiple LLCs
^^^^^^^^^^^^^^^^^^^

As mentioned in the introduction, at least one sched domain partition must
span more than one LLC. Platforms that benefit include:

 - chiplet processors with an LLC per die or per core complex,
 - processors that expose several LLC domains per socket,
 - multi-socket systems with memory interleaved over the sockets. Such a
   system is seen as one NUMA node with several LLCs, so NUMA balancing
   cannot help there,
 - platforms whose cacheinfo reports cluster-level LLCs.

On ordinary multi-socket systems with one LLC per NUMA node, NUMA
balancing already handles the placement between nodes, and it takes
precedence.

8.3 - Cache size information
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The LLC size is read from the generic cacheinfo framework. Architectures
that do not populate it (currently PowerPC, which has its own cacheinfo
implementation) report an LLC size of 0. When NUMA balancing is active,
any non-zero footprint then fails the check of Section 5 (c), so cache
aware scheduling is effectively disabled on these platforms.

8.4 - NUMA balancing
^^^^^^^^^^^^^^^^^^^^

The footprint estimate of Section 5 (c) comes from NUMA balancing fault
statistics. Without NUMA balancing, the footprint check is skipped, and
groups with a working set much larger than one LLC can still be aggregated.
With NUMA balancing, the scan is also limited to the relevant nodes, which
reduces its cost on large systems.

8.5 - Multithreading (SMT)
^^^^^^^^^^^^^^^^^^^^^^^^^^

The thread limit of Section 5 (b) multiplies the number of running threads
by the number of hardware threads per core. Thus, it effectively counts
cores. On systems with many threads per core and small LLCs (for example
SMT-4 with 4 CPUs per LLC), almost every multi-threaded process fails this
check unless aggr_tolerance is raised.

9. Per-process control with prctl()
-----------------------------------

9.1 - Cache groups
^^^^^^^^^^^^^^^^^^

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

9.2 - The interface
^^^^^^^^^^^^^^^^^^^

::

    int prctl(int option, unsigned long subop, unsigned long pid,
              unsigned long arg4, unsigned long pid_type);

with ``option`` set to ``PR_SCHED_CACHE``. The remaining arguments are:

``subop``
    Which operation to perform (see below).

``pid``
    The task the operation applies to. ``0`` means the calling task.

``arg4``
    Only used by ``PR_SCHED_CACHE_GET`` and ``PR_SCHED_CACHE_GET_ENABLED``
    (output pointer) and ``PR_SCHED_CACHE_SHARE_FROM`` (source pid). Must be
    ``0`` for every other sub-operation.

``pid_type``
    One of ``PR_SCHED_CACHE_SCOPE_THREAD``,
    ``PR_SCHED_CACHE_SCOPE_THREAD_GROUP`` or
    ``PR_SCHED_CACHE_SCOPE_PROCESS_GROUP``. It selects whether the operation
    affects just the named thread, its whole thread group, or its process
    group. ``PR_SCHED_CACHE_GET``, ``PR_SCHED_CACHE_GET_ENABLED``,
    ``PR_SCHED_CACHE_DISABLE`` and ``PR_SCHED_CACHE_ENABLE`` require
    ``PR_SCHED_CACHE_SCOPE_THREAD``.

The cache group itself is a kernel object. User space never invents or
passes a group id; it only names tasks by pid and asks the kernel to create
a group or to copy one task's group onto another. This follows core
scheduling, where the cookie is a kernel object and only an obfuscated
identifier is returned by ``PR_SCHED_CACHE_GET``.

9.3 - Sub-operations
^^^^^^^^^^^^^^^^^^^^

``PR_SCHED_CACHE_GET``
    Read back the cache group id of ``pid``. ``arg4`` is a
    ``__u64 __user *`` that must be non-NULL and 8-byte aligned; the kernel
    writes an obfuscated hash of the task's cache group there (0 if the task
    currently has no group). ``pid_type`` must be ``PR_SCHED_CACHE_SCOPE_THREAD``.

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
    it for every task in the group, so ``pid_type`` must be
    ``PR_SCHED_CACHE_SCOPE_THREAD``. A newly created group, including the
    default per-mm group, has made no choice yet. How the choice is applied
    depends on the system-wide mode (Section 7.1).

``PR_SCHED_CACHE_GET_ENABLED``
    Read back the flag set by the two sub-operations above. ``arg4`` is a
    ``__u64 __user *`` that must be non-NULL and 8-byte aligned; the kernel
    writes 1 if the target's cache group was opted in with
    ``PR_SCHED_CACHE_ENABLE`` and 0 otherwise. ``pid_type`` must be
    ``PR_SCHED_CACHE_SCOPE_THREAD``.

    Note that this reports the per-group choice only, not whether
    aggregation is actually in effect: a group that made no choice is
    aggregated in the ``always`` mode, and no group is aggregated in the
    ``never`` mode (Section 7.1).

9.4 - Permissions
^^^^^^^^^^^^^^^^^

The caller must be able to ``ptrace_may_access(PTRACE_MODE_READ_REALCREDS)``
every task it touches. For the ``PR_SCHED_CACHE_SCOPE_THREAD_GROUP`` and
``PR_SCHED_CACHE_SCOPE_PROCESS_GROUP`` scopes the access of *all* tasks in
the group is checked before *any* task is changed, so the operation either
applies to the whole group or fails with -EPERM without touching anyone. ``PR_SCHED_CACHE_SHARE_FROM`` additionally
requires access to the source task.

Kernel threads cannot be targeted.

9.5 - Inheritance
^^^^^^^^^^^^^^^^^

Cache group membership follows these rules:

  - A thread, or any other child created with ``CLONE_VM``, joins the
    cache group of its parent.
  - A child created without ``CLONE_VM`` (a plain ``fork()``) gets a new
    address space and the new default group of that address space. It does
    *not* inherit the parent's group. To keep forked helpers in one group,
    use ``PR_SCHED_CACHE_SHARE_FROM`` for each child.
  - ``execve()`` moves the task to the default group of its new address
    space. A group set through the prctl is lost.

9.6 - Return value
^^^^^^^^^^^^^^^^^^

Returns 0 on success. On error one of:

``-EINVAL``
    Unknown sub-operation, out-of-range ``pid``/``pid_type``, ``arg4`` given
    for a sub-operation that does not take one, misaligned ``GET`` or
    ``GET_ENABLED`` pointer, ``GET``, ``GET_ENABLED``, ``DISABLE`` or
    ``ENABLE`` with a ``pid_type`` other than ``PR_SCHED_CACHE_SCOPE_THREAD``, or the target
    (or source) is a kernel thread.

``-ESRCH``
    The target (or, for ``SHARE_FROM``, the source) task does not exist.

``-EPERM``
    The caller is not allowed to access the target (or source) task.

``-ENOENT``
    The source task (``SHARE_FROM``) or target task
    (``DISABLE``/``ENABLE``/``GET_ENABLED``) has no cache group.

``-ENOMEM``
    ``CREATE`` could not allocate a new group.

``-EFAULT``
    ``GET`` or ``GET_ENABLED`` could not write to the ``arg4`` pointer,
    including when it is NULL.

9.7 - Example
^^^^^^^^^^^^^

Put a helper process into the same cache group as its parent::

    /* In the parent, once per instance. */
    prctl(PR_SCHED_CACHE, PR_SCHED_CACHE_CREATE, 0, 0,
          PR_SCHED_CACHE_SCOPE_THREAD_GROUP);

    /* In (or on behalf of) each helper. */
    prctl(PR_SCHED_CACHE, PR_SCHED_CACHE_SHARE_FROM,
          helper_pid, parent_tgid, PR_SCHED_CACHE_SCOPE_THREAD);

    /* In the advise mode, opt the group in. */
    prctl(PR_SCHED_CACHE, PR_SCHED_CACHE_ENABLE, parent_tgid, 0,
          PR_SCHED_CACHE_SCOPE_THREAD);
