#include "../base/ntos/ex/arm64/interlocked.h"
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

enum { THREADS = 8, ITERATIONS = 100000, TOTAL = THREADS * ITERATIONS };
static volatile int32_t tickets;
static volatile int64_t cas_counter;
static _Atomic unsigned seen[TOTAL];
static volatile int32_t ready;
static _Atomic uint64_t payload;

static void single_thread(void)
{
    struct { int32_t value; uint32_t guard; } a = {0, 0xabcdef01};
    int64_t b = 0;
    assert(InterlockedIncrement(&a.value) == 1);
    assert(InterlockedDecrement(&a.value) == 0);
    assert(InterlockedExchange(&a.value, 17) == 0);
    assert(InterlockedExchangeAdd(&a.value, -5) == 17 && a.value == 12);
    assert(InterlockedCompareExchange(&a.value, 44, 13) == 12 && a.value == 12);
    assert(InterlockedCompareExchange(&a.value, 44, 12) == 12 && a.value == 44);
    a.value = INT32_MAX;
    assert(InterlockedIncrement(&a.value) == INT32_MIN);
    assert(InterlockedDecrement(&a.value) == INT32_MAX);
    assert(a.guard == 0xabcdef01);
    assert(InterlockedIncrement64(&b) == 1);
    assert(InterlockedDecrement64(&b) == 0);
    assert(InterlockedExchange64(&b, INT64_C(0x123456789abcdef)) == 0);
    assert(InterlockedExchangeAdd64(&b, -7) == INT64_C(0x123456789abcdef));
    int64_t old = b;
    assert(InterlockedCompareExchange64(&b, 12, 3) == old && b == old);
    assert(InterlockedCompareExchange64(&b, 12, old) == old && b == 12);
    b = INT64_MAX;
    assert(InterlockedIncrement64(&b) == INT64_MIN);
    assert(InterlockedDecrement64(&b) == INT64_MAX);

    void *allocation = malloc(16);
    assert(allocation);
    void *volatile pointer = NULL;
    assert(InterlockedExchangePointer(&pointer, allocation) == NULL);
    assert(InterlockedCompareExchangePointer(&pointer, NULL, NULL) == allocation);
    assert(pointer == allocation);
    assert(InterlockedCompareExchangePointer(&pointer, NULL, allocation) == allocation);
    assert(pointer == NULL);
    free(allocation);
}
static void *contend(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < ITERATIONS; ++i) {
        int32_t index = InterlockedExchangeAdd(&tickets, 1);
        assert(index >= 0 && index < TOTAL);
        assert(atomic_fetch_add_explicit(&seen[index], 1, memory_order_relaxed) == 0);
        int64_t old = InterlockedCompareExchange64(&cas_counter, 0, 0);
        for (;;) {
            int64_t found = InterlockedCompareExchange64(&cas_counter, old + 1, old);
            if (found == old) break;
            old = found;
        }
    }
    return NULL;
}
static void *publish(void *unused)
{
    (void)unused;
    for (uint64_t i = 1; i <= ITERATIONS; ++i) {
        while (InterlockedCompareExchange(&ready, 0, 0) != 0) {}
        atomic_store_explicit(&payload, i, memory_order_relaxed);
        assert(InterlockedExchange(&ready, 1) == 0);
    }
    return NULL;
}
static void *consume(void *unused)
{
    (void)unused;
    for (uint64_t i = 1; i <= ITERATIONS; ++i) {
        while (InterlockedCompareExchange(&ready, 1, 1) != 1) {}
        assert(atomic_load_explicit(&payload, memory_order_relaxed) == i);
        assert(InterlockedExchange(&ready, 0) == 1);
    }
    return NULL;
}
int main(void)
{
    single_thread();
    pthread_t threads[THREADS];
    for (unsigned i = 0; i < THREADS; ++i)
        assert(pthread_create(&threads[i], NULL, contend, NULL) == 0);
    for (unsigned i = 0; i < THREADS; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    assert(tickets == TOTAL && cas_counter == TOTAL);
    for (unsigned i = 0; i < TOTAL; ++i)
        assert(atomic_load_explicit(&seen[i], memory_order_relaxed) == 1);
    assert(pthread_create(&threads[0], NULL, publish, NULL) == 0);
    assert(pthread_create(&threads[1], NULL, consume, NULL) == 0);
    assert(pthread_join(threads[0], NULL) == 0);
    assert(pthread_join(threads[1], NULL) == 0);
    puts("PASS: ARM64 return values, 32/64-bit wrap, pointer width, 800000 contended updates and 100000 publications");
    return 0;
}
