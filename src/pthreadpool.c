/* Standard C headers */
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>

/* POSIX headers */
#include <pthread.h>
#include <unistd.h>

#include <fxdiv.h>

/* Library header */
#include <pthreadpool.h>

#define PTHREADPOOL_CACHELINE_SIZE 64
#define PTHREADPOOL_CACHELINE_ALIGNED __attribute__((__aligned__(PTHREADPOOL_CACHELINE_SIZE)))

#if defined(__clang__)
	#if __has_extension(c_static_assert) || __has_feature(c_static_assert)
		#define PTHREADPOOL_STATIC_ASSERT(predicate, message) _Static_assert((predicate), message)
	#else
		#define PTHREADPOOL_STATIC_ASSERT(predicate, message)
	#endif
#elif defined(__GNUC__) && ((__GNUC__ > 4) || (__GNUC__ == 4) && (__GNUC_MINOR__ >= 6))
	/* Static assert is supported by gcc >= 4.6 */
	#define PTHREADPOOL_STATIC_ASSERT(predicate, message) _Static_assert((predicate), message)
#else
	#define PTHREADPOOL_STATIC_ASSERT(predicate, message)
#endif

static inline size_t multiply_divide(size_t a, size_t b, size_t d) {
	#if defined(__SIZEOF_SIZE_T__) && (__SIZEOF_SIZE_T__ == 4)
		return (size_t) (((uint64_t) a) * ((uint64_t) b)) / ((uint64_t) d);
	#elif defined(__SIZEOF_SIZE_T__) && (__SIZEOF_SIZE_T__ == 8)
		return (size_t) (((__uint128_t) a) * ((__uint128_t) b)) / ((__uint128_t) d);
	#else
		#error "Unsupported platform"
	#endif
}

static inline size_t divide_round_up(size_t dividend, size_t divisor) {
	if (dividend % divisor == 0) {
		return dividend / divisor;
	} else {
		return dividend / divisor + 1;
	}
}

static inline size_t min(size_t a, size_t b) {
	return a < b ? a : b;
}

enum thread_state {
	thread_state_idle,
	thread_state_compute_1d,
	thread_state_shutdown,
};

struct PTHREADPOOL_CACHELINE_ALIGNED thread_info {
	/**
         * The next index of the item that the worker will work on.
	 */
	volatile size_t next_index;
	/**
         * The total number of items that need to be processed by all threads.
	 */
	volatile size_t items_count;
        /**
	 * Thread id in the 0..threads_count-1 range.
	 */
	size_t thread_id;
	/**
	 * The active state of the thread.
	 */
	volatile enum thread_state state;
	/**
	 * The pthread object corresponding to the thread.
	 */
	pthread_t thread_object;
	/**
	 * Condition variable used to wake up the thread.
	 * When the thread is idle, it waits on this condition variable.
	 */
	pthread_cond_t wakeup_condvar;
};

PTHREADPOOL_STATIC_ASSERT(sizeof(struct thread_info) % PTHREADPOOL_CACHELINE_SIZE == 0, "thread_info structure must occupy an integer number of cache lines (64 bytes)");

struct PTHREADPOOL_CACHELINE_ALIGNED pthreadpool {
	/**
	 * The number of threads that signalled completion of an operation.
	 */
	volatile size_t checkedin_threads;
	/**
	 * The function to call for each item.
	 */
	volatile void* function;
	/**
	 * The first argument to the item processing function.
	 */
	void *volatile argument;
	/**
	 * Serializes concurrent calls to @a pthreadpool_compute_* from different threads.
	 */
	pthread_mutex_t execution_mutex;
	/**
	 * Guards access to the @a checkedin_threads variable.
	 */
	pthread_mutex_t barrier_mutex;
	/**
	 * Condition variable to wait until all threads check in.
	 */
	pthread_cond_t barrier_condvar;
	/**
	 * Guards access to the @a state variables.
	 */
	pthread_mutex_t state_mutex;
	/**
	 * Condition variable to wait for change of @a state variable.
	 */
	pthread_cond_t state_condvar;
	/**
	 * The number of threads in the thread pool. Never changes after initialization.
	 */
	size_t threads_count;
	/**
	 * Thread information structures that immediately follow this structure.
	 */
	struct thread_info threads[];
};

PTHREADPOOL_STATIC_ASSERT(sizeof(struct pthreadpool) % PTHREADPOOL_CACHELINE_SIZE == 0, "pthreadpool structure must occupy an integer number of cache lines (64 bytes)");

static void checkin_worker_thread(struct pthreadpool* threadpool) {
	pthread_mutex_lock(&threadpool->barrier_mutex);
	const size_t checkedin_threads = threadpool->checkedin_threads + 1;
	threadpool->checkedin_threads = checkedin_threads;
	if (checkedin_threads == threadpool->threads_count) {
		pthread_cond_signal(&threadpool->barrier_condvar);
	}
	pthread_mutex_unlock(&threadpool->barrier_mutex);
}

static void wait_worker_threads(struct pthreadpool* threadpool) {
	if (threadpool->checkedin_threads != threadpool->threads_count) {
		pthread_mutex_lock(&threadpool->barrier_mutex);
		while (threadpool->checkedin_threads != threadpool->threads_count) {
			pthread_cond_wait(&threadpool->barrier_condvar, &threadpool->barrier_mutex);
		};
		pthread_mutex_unlock(&threadpool->barrier_mutex);
	}
}

static void wakeup_worker_threads(struct pthreadpool* threadpool) {
	pthread_mutex_lock(&threadpool->state_mutex);
	threadpool->checkedin_threads = 0; /* Locking of barrier_mutex not needed: readers are sleeping */
	pthread_cond_broadcast(&threadpool->state_condvar);
	pthread_mutex_unlock(&threadpool->state_mutex); /* Do wake up */
}

#define FETCH_ADD(ptr, n) __sync_fetch_and_add(ptr, n)

static size_t steal(struct pthreadpool* threadpool) {
        size_t tid = 0, mintid = 0, nextitemindex = 0, minitemindex = LONG_MAX;
        for (; tid < threadpool->threads_count; ++tid) {
                if (threadpool->threads[tid].state != thread_state_idle && minitemindex > threadpool->threads[tid].next_index) {
                        minitemindex = threadpool->threads[tid].next_index;
                        mintid = tid;
                }
        }
        return FETCH_ADD(&(threadpool->threads[mintid].next_index), threadpool->threads_count);
}

static void thread_compute_1d(struct pthreadpool* threadpool, struct thread_info* thread) {
	const pthreadpool_function_1d_t function = (pthreadpool_function_1d_t) threadpool->function;
	void *const argument = threadpool->argument;
	/* Process thread's own range of items */
        const size_t threads_count = threadpool->threads_count;
        const size_t items_count = thread->items_count;
        
	volatile size_t* ptr_index = &(thread->next_index);

        for (size_t index = FETCH_ADD(ptr_index, threads_count); index < items_count; index = FETCH_ADD(ptr_index, threads_count)) {
                function(argument, index);
        }

        /* Steal work from other threads */
        for (size_t index = steal(threadpool); index < items_count; index = steal(threadpool)) {
                function(argument, index);
        }
}

static void* thread_main(void* arg) {
	struct thread_info* thread = (struct thread_info*) arg;
	struct pthreadpool* threadpool = ((struct pthreadpool*) (thread - thread->thread_id)) - 1;

	/* Check in */
	checkin_worker_thread(threadpool);

	/* Monitor the state changes and act accordingly */
	for (;;) {
		/* Lock the state mutex */
		pthread_mutex_lock(&threadpool->state_mutex);
		/* Read the state */
		enum thread_state state;
		while ((state = thread->state) == thread_state_idle) {
			/* Wait for state change */
			pthread_cond_wait(&threadpool->state_condvar, &threadpool->state_mutex);
		}
		/* Read non-idle state */
		pthread_mutex_unlock(&threadpool->state_mutex);
		switch (state) {
			case thread_state_compute_1d:
				thread_compute_1d(threadpool, thread);
				break;
			case thread_state_shutdown:
				return NULL;
			case thread_state_idle:
				/* To inhibit compiler warning */
				break;
		}
		/* Notify the master thread that we finished processing */
		thread->state = thread_state_idle;
		checkin_worker_thread(threadpool);
	};
}

struct pthreadpool* pthreadpool_create(size_t threads_count) {
	if (threads_count == 0) {
		threads_count = (size_t) sysconf(_SC_NPROCESSORS_ONLN);
	}
#if !defined(__ANDROID__)
	struct pthreadpool* threadpool = NULL;
	if (posix_memalign((void**) &threadpool, 64, sizeof(struct pthreadpool) + threads_count * sizeof(struct thread_info)) != 0) {
#else
	/*
	 * Android didn't get posix_memalign until API level 17 (Android 4.2).
	 * Use (otherwise obsolete) memalign function on Android platform.
	 */
	struct pthreadpool* threadpool = memalign(64, sizeof(struct pthreadpool) + threads_count * sizeof(struct thread_info));
	if (threadpool == NULL) {
#endif
		return NULL;
	}
	memset(threadpool, 0, sizeof(struct pthreadpool) + threads_count * sizeof(struct thread_info));
	threadpool->threads_count = threads_count;
	pthread_mutex_init(&threadpool->execution_mutex, NULL);
	pthread_mutex_init(&threadpool->barrier_mutex, NULL);
	pthread_cond_init(&threadpool->barrier_condvar, NULL);
	pthread_mutex_init(&threadpool->state_mutex, NULL);
	pthread_cond_init(&threadpool->state_condvar, NULL);

	for (size_t tid = 0; tid < threads_count; tid++) {
		threadpool->threads[tid].thread_id = tid;
		threadpool->threads[tid].next_index = tid;
		pthread_create(&threadpool->threads[tid].thread_object, NULL, &thread_main, &threadpool->threads[tid]);
	}

	/* Wait until all threads initialize */
	wait_worker_threads(threadpool);
	return threadpool;
}

size_t pthreadpool_get_threads_count(struct pthreadpool* threadpool) {
	return threadpool->threads_count;
}

void pthreadpool_compute_1d(
	struct pthreadpool* threadpool,
	pthreadpool_function_1d_t function,
	void* argument,
	size_t range)
{
	if (threadpool == NULL) {
		/* No thread pool provided: execute function sequentially on the calling thread */
		for (size_t i = 0; i < range; i++) {
			function(argument, i);
		}
	} else {
		/* Protect the global threadpool structures */
		pthread_mutex_lock(&threadpool->execution_mutex);

		/* Lock the state variables to ensure that threads don't start processing before they observe complete state */
		pthread_mutex_lock(&threadpool->state_mutex);

		/* Setup global arguments */
		threadpool->function = (void *)function;
		threadpool->argument = argument;

		/* Spread the work between threads */
		for (size_t tid = 0; tid < threadpool->threads_count; tid++) {
			struct thread_info* thread = &threadpool->threads[tid];
                        thread->items_count = range;
                        thread->next_index = tid;
			thread->state = thread_state_compute_1d;
		}

		/* Unlock the state variables before waking up the threads for better performance */
		pthread_mutex_unlock(&threadpool->state_mutex);

		/* Wake up the threads */
		wakeup_worker_threads(threadpool);

		/* Wait until the threads finish computation */
		wait_worker_threads(threadpool);

		/* Unprotect the global threadpool structures */
		pthread_mutex_unlock(&threadpool->execution_mutex);
	}
}

struct compute_1d_tiled_context {
	pthreadpool_function_1d_tiled_t function;
	void* argument;
	size_t range;
	size_t tile;
};

static void compute_1d_tiled(const struct compute_1d_tiled_context* context, size_t linear_index) {
	const size_t tile_index = linear_index;
	const size_t index = tile_index * context->tile;
	const size_t tile = min(context->tile, context->range - index);
	context->function(context->argument, index, tile);
}

void pthreadpool_compute_1d_tiled(
	pthreadpool_t threadpool,
	pthreadpool_function_1d_tiled_t function,
	void* argument,
	size_t range,
	size_t tile)
{
	if (threadpool == NULL) {
		/* No thread pool provided: execute function sequentially on the calling thread */
		for (size_t i = 0; i < range; i += tile) {
			function(argument, i, min(range - i, tile));
		}
	} else {
		/* Execute in parallel on the thread pool using linearized index */
		const size_t tile_range = divide_round_up(range, tile);
		struct compute_1d_tiled_context context = {
			.function = function,
			.argument = argument,
			.range = range,
			.tile = tile
		};
		pthreadpool_compute_1d(threadpool, (pthreadpool_function_1d_t) compute_1d_tiled, &context, tile_range);
	}
}

struct compute_2d_context {
	pthreadpool_function_2d_t function;
	void* argument;
	struct fxdiv_divisor_size_t range_j;
};

static void compute_2d(const struct compute_2d_context* context, size_t linear_index) {
	const struct fxdiv_divisor_size_t range_j = context->range_j;
	const struct fxdiv_result_size_t index = fxdiv_divide_size_t(linear_index, range_j);
	context->function(context->argument, index.quotient, index.remainder);
}

void pthreadpool_compute_2d(
	struct pthreadpool* threadpool,
	pthreadpool_function_2d_t function,
	void* argument,
	size_t range_i,
	size_t range_j)
{
	if (threadpool == NULL) {
		/* No thread pool provided: execute function sequentially on the calling thread */
		for (size_t i = 0; i < range_i; i++) {
			for (size_t j = 0; j < range_j; j++) {
				function(argument, i, j);
			}
		}
	} else {
		/* Execute in parallel on the thread pool using linearized index */
		struct compute_2d_context context = {
			.function = function,
			.argument = argument,
			.range_j = fxdiv_init_size_t(range_j)
		};
		pthreadpool_compute_1d(threadpool, (pthreadpool_function_1d_t) compute_2d, &context, range_i * range_j);
	}
}

struct compute_2d_tiled_context {
	pthreadpool_function_2d_tiled_t function;
	void* argument;
	struct fxdiv_divisor_size_t tile_range_j;
	size_t range_i;
	size_t range_j;
	size_t tile_i;
	size_t tile_j;
};

static void compute_2d_tiled(const struct compute_2d_tiled_context* context, size_t linear_index) {
	const struct fxdiv_divisor_size_t tile_range_j = context->tile_range_j;
	const struct fxdiv_result_size_t tile_index = fxdiv_divide_size_t(linear_index, tile_range_j);
	const size_t max_tile_i = context->tile_i;
	const size_t max_tile_j = context->tile_j;
	const size_t index_i = tile_index.quotient * max_tile_i;
	const size_t index_j = tile_index.remainder * max_tile_j;
	const size_t tile_i = min(max_tile_i, context->range_i - index_i);
	const size_t tile_j = min(max_tile_j, context->range_j - index_j);
	context->function(context->argument, index_i, index_j, tile_i, tile_j);
}

void pthreadpool_compute_2d_tiled(
	pthreadpool_t threadpool,
	pthreadpool_function_2d_tiled_t function,
	void* argument,
	size_t range_i,
	size_t range_j,
	size_t tile_i,
	size_t tile_j)
{
	if (threadpool == NULL) {
		/* No thread pool provided: execute function sequentially on the calling thread */
		for (size_t i = 0; i < range_i; i += tile_i) {
			for (size_t j = 0; j < range_j; j += tile_j) {
				function(argument, i, j, min(range_i - i, tile_i), min(range_j - j, tile_j));
			}
		}
	} else {
		/* Execute in parallel on the thread pool using linearized index */
		const size_t tile_range_i = divide_round_up(range_i, tile_i);
		const size_t tile_range_j = divide_round_up(range_j, tile_j);
		struct compute_2d_tiled_context context = {
			.function = function,
			.argument = argument,
			.tile_range_j = fxdiv_init_size_t(tile_range_j),
			.range_i = range_i,
			.range_j = range_j,
			.tile_i = tile_i,
			.tile_j = tile_j
		};
		pthreadpool_compute_1d(threadpool, (pthreadpool_function_1d_t) compute_2d_tiled, &context, tile_range_i * tile_range_j);
	}
}

void pthreadpool_destroy(struct pthreadpool* threadpool) {
	/* Update threads' states */
	for (size_t tid = 0; tid < threadpool->threads_count; tid++) {
		threadpool->threads[tid].state = thread_state_shutdown;
	}

	/* Wake up the threads */
	wakeup_worker_threads(threadpool);

	/* Wait until all threads return */
	for (size_t tid = 0; tid < threadpool->threads_count; tid++) {
		pthread_join(threadpool->threads[tid].thread_object, NULL);
	}

	/* Release resources */
	pthread_mutex_destroy(&threadpool->execution_mutex);
	pthread_mutex_destroy(&threadpool->barrier_mutex);
	pthread_cond_destroy(&threadpool->barrier_condvar);
	pthread_mutex_destroy(&threadpool->state_mutex);
	pthread_cond_destroy(&threadpool->state_condvar);
	free(threadpool);
}
