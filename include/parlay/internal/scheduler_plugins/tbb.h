#ifndef PARLAY_INTERNAL_SCHEDULER_PLUGINS_TBB_HPP_
#define PARLAY_INTERNAL_SCHEDULER_PLUGINS_TBB_HPP_

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_invoke.h>
#include <tbb/task_arena.h>

namespace parlay {

inline int num_workers() { return tbb::this_task_arena::max_concurrency(); }
inline int worker_id() { return tbb::task_arena::current_thread_index(); }

template <class F>
inline void parallel_for(long start, long end, F f, long granularity, bool) {
  if (granularity == 0) {
    tbb::parallel_for(tbb::blocked_range<size_t>(start, end), [&](const tbb::blocked_range<size_t>& r) {
      for (auto i = r.begin(); i != r.end(); ++i) {
        f(i);
      }
    });
  }
  else {
    long n_blocks = (end - start + granularity - 1) / granularity;
    long block_size = (end - start + n_blocks - 1) / n_blocks;
    tbb::parallel_for(0L, n_blocks, [&](long b) {
      for (long i = b * block_size + start; i < (b + 1) * block_size + start && i < end; i++) {
        f(i);
      }
    });
  }
}

template <typename Lf, typename Rf>
inline void par_do(Lf left, Rf right, bool) {
  tbb::parallel_invoke(left, right);
}

}  // namespace parlay

#endif  // PARLAY_INTERNAL_SCHEDULER_PLUGINS_TBB_HPP_

