#ifndef SORTUTIL_H
#define SORTUTIL_H

#include <algorithm>
#include <cstddef>
#include <thread>
#include <vector>

// Sort [first, first + n) with `cmp`, using up to `nthreads` threads: each
// thread std::sort()s one chunk, then neighbouring chunks are merged pairwise
// (in parallel) until one run is left. Falls back to a plain std::sort for small
// inputs or a single thread.
template <class T, class Cmp>
void parallel_sort(T *first, size_t n, Cmp cmp, unsigned nthreads) {
	const size_t MIN_PER_THREAD = 1 << 16;
	size_t maxThreads = n / MIN_PER_THREAD;
	if (nthreads > maxThreads) nthreads = (unsigned)maxThreads;
	if (nthreads <= 1) {
		std::sort(first, first + n, cmp);
		return;
	}

	// chunk boundaries
	std::vector<size_t> bounds(nthreads + 1);
	for (unsigned i = 0; i <= nthreads; i++)
		bounds[i] = n / nthreads * i + (i == nthreads ? n % nthreads : 0);

	std::vector<std::thread> pool;
	for (unsigned i = 0; i < nthreads; i++)
		pool.emplace_back([=, &bounds]() { std::sort(first + bounds[i], first + bounds[i + 1], cmp); });
	for (auto &t : pool) t.join();

	// pairwise merge of adjacent runs
	while (bounds.size() > 2) {
		std::vector<size_t> next;
		std::vector<std::thread> mergers;
		size_t runs = bounds.size() - 1;
		for (size_t r = 0; r < runs; r += 2) {
			next.push_back(bounds[r]);
			if (r + 1 < runs) {
				size_t lo = bounds[r], mid = bounds[r + 1], hi = bounds[r + 2];
				mergers.emplace_back([=]() { std::inplace_merge(first + lo, first + mid, first + hi, cmp); });
			}
		}
		next.push_back(n);
		for (auto &t : mergers) t.join();
		bounds.swap(next);
	}
}

#endif
