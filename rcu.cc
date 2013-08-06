#include <unistd.h>
#include <time.h>
#include <string.h>
#include <numa.h>
#include <sched.h>
#include <iostream>
#include <thread>

#include "rcu.h"
#include "macros.h"
#include "util.h"
#include "thread.h"
#include "counter.h"
#include "lockguard.h"

using namespace std;
using namespace util;

rcu rcu::s_instance;

static event_counter evt_rcu_deletes("rcu_deletes");
static event_counter evt_rcu_frees("rcu_frees");
static event_counter evt_rcu_local_reaps("rcu_local_reaps");
static event_counter evt_rcu_incomplete_local_reaps("rcu_incomplete_local_reaps");
static event_counter evt_rcu_loop_reaps("rcu_loop_reaps");
static event_counter evt_allocator_arena_allocation("allocator_arena_allocation");
static event_counter evt_allocator_large_allocation("allocator_large_allocation");

static event_avg_counter evt_avg_gc_reaper_queue_len("avg_gc_reaper_queue_len");
static event_avg_counter evt_avg_rcu_delete_queue_len("avg_rcu_delete_queue_len");
static event_avg_counter evt_avg_rcu_local_delete_queue_len("avg_rcu_local_delete_queue_len");
static event_avg_counter evt_avg_rcu_sync_try_release("avg_rcu_sync_try_release");

void *
rcu::sync::alloc(size_t sz)
{
  if (pin_cpu_ == -1)
    // fallback to regular allocator
    return malloc(sz);
  auto sizes = ::allocator::ArenaSize(sz);
  auto arena = sizes.second;
  if (arena >= ::allocator::MAX_ARENAS) {
    // fallback to regular allocator
    ++evt_allocator_large_allocation;
    return malloc(sz);
  }
  ensure_arena(arena);
  void *p = arenas_[arena];
  ALWAYS_ASSERT(p);
  arenas_[arena] = *reinterpret_cast<void **>(p);
  ++evt_allocator_arena_allocation;
  return p;
}

void *
rcu::sync::alloc_static(size_t sz)
{
  if (pin_cpu_ == -1)
    return malloc(sz);
  // round up to hugepagesize
  static const size_t hugepgsize = ::allocator::GetHugepageSize();
  sz = slow_round_up(sz, hugepgsize);
  INVARIANT((sz % hugepgsize) == 0);
  return ::allocator::AllocateUnmanaged(pin_cpu_, sz / hugepgsize);
}

void
rcu::sync::dealloc(void *p, size_t sz)
{
  if (!::allocator::ManagesPointer(p)) {
    ::free(p);
    return;
  }
  auto sizes = ::allocator::ArenaSize(sz);
  auto arena = sizes.second;
  ALWAYS_ASSERT(arena < ::allocator::MAX_ARENAS);
  *reinterpret_cast<void **>(p) = arenas_[arena];
  arenas_[arena] = p;
  deallocs_[arena]++;
}

void
rcu::sync::try_release()
{
  // XXX: tune
  static const size_t threshold = 10000;
  // only release if there are > threshold segments to release (over all arenas)
  size_t acc = 0;
  for (size_t i = 0; i < ::allocator::MAX_ARENAS; i++)
    acc += deallocs_[i];
  if (acc > threshold) {
    do_release();
    evt_avg_rcu_sync_try_release.offer(acc);
  }
}

void
rcu::sync::do_release()
{
  ::allocator::ReleaseArenas(arenas_);
  NDB_MEMSET(arenas_, 0, sizeof(arenas_));
  NDB_MEMSET(deallocs_, 0, sizeof(deallocs_));
}

void
rcu::free_with_fn(void *p, deleter_t fn)
{
  uint64_t rcu_tick = 0;
  const bool in_region = in_rcu_region(rcu_tick);
  if (!in_region)
    INVARIANT(false);
  // already locked by the scoped region
  sync &s = mysync();
  s.queue_.enqueue(delete_entry(p, fn), rcu_tick);
  ++evt_rcu_frees;
}

void
rcu::pin_current_thread(size_t cpu)
{
  sync &s = mysync();
  s.set_pin_cpu(cpu);
  auto node = numa_node_of_cpu(cpu);
  // pin to node
  ALWAYS_ASSERT(!numa_run_on_node(node));
  // is numa_run_on_node() guaranteed to take effect immediately?
  ALWAYS_ASSERT(!sched_yield());
  // release current thread-local cache back to allocator
  s.do_release();
}

void
rcu::fault_region()
{
  sync &s = mysync();
  if (s.get_pin_cpu() == -1)
    return;
  ::allocator::FaultRegion(s.get_pin_cpu());
}

rcu::rcu()
  : syncs_([this](sync &s) { s.impl_ = this; })
{
}

scoped_rcu_region::~scoped_rcu_region()
{
  INVARIANT(sync_->depth_);
  const unsigned new_depth = --sync_->depth_;
  guard()->ticker::guard::~guard();
  if (new_depth)
    return;
  // out of RCU region now, check if we need to run cleaner

  // compute cleaner epoch
  const uint64_t clean_tick_exclusive = sync_->impl_->cleaning_rcu_tick_exclusive();
  if (!clean_tick_exclusive)
    return;
  const uint64_t clean_tick = clean_tick_exclusive - 1;

  INVARIANT(sync_->last_reaped_epoch_ <= clean_tick);
  INVARIANT(sync_->scratch_.empty());
  if (sync_->last_reaped_epoch_ == clean_tick)
    return;
  sync_->last_reaped_epoch_ = clean_tick;

  sync_->scratch_.empty_accept_from(sync_->queue_, clean_tick);
  sync_->scratch_.transfer_freelist(sync_->queue_);
  rcu::px_queue &q = sync_->scratch_;
  if (q.empty())
    return;
  size_t n = 0;
  for (auto it = q.begin(); it != q.end(); ++it, ++n) {
    try {
      it->second(it->first);
    } catch (...) {
      cerr << "rcu::region_end: uncaught exception in free routine" << endl;
    }
  }
  q.clear();
  evt_rcu_deletes += n;
  evt_avg_rcu_local_delete_queue_len.offer(n);

  // try to release memory from allocator slabs back
  sync_->impl_->try_release();
}
