#undef GCC_SYNC_FUNCTIONS
#undef GCC_ATOMIC_FUNCTIONS

#ifndef __GNUC__
#error Please edit abstract_atomic.h and implement support for  \
        non-GNU compilers.
#else                           /* __GNUC__ */
#define ATOMIC_GCC_VERSION (__GNUC__ * 10000                           \
                            + __GNUC_MINOR__ * 100                     \
                            + __GNUC_PATCHLEVEL__)

#if ((ATOMIC_GCC_VERSION) >= 40700)
#define GCC_ATOMIC_FUNCTIONS 1
#elif ((ATOMIC_GCC_VERSION) >= 40100)
#define GCC_SYNC_FUNCTIONS 1
#else
#error This verison of GCC does not support atomics.
#endif                          /* Version check */
#endif                          /* __GNUC__ */


#define atomic_postinc(x) __sync_fetch_and_add(&x, 1)
#define atomic_postdec(x) __sync_fetch_and_sub(&x, 1)
#define atomic_postadd(x,i) __sync_fetch_and_add(&x, i)
#define atomic_postsub(x,i) __sync_fetch_and_sub(&x, i)
#define atomic_postmask(x,i) __sync_fetch_and_and(&x, i)
#define atomic_inc(x) __sync_add_and_fetch(&x, 1)
#define atomic_dec(x) __sync_sub_and_fetch(&x, 1)
#define atomic_add(x,i) __sync_add_and_fetch(&x, i)
#define atomic_sub(x,i) __sync_sub_and_fetch(&x, i)
#define atomic_mask(x,i) __sync_and_and_fetch(&x, i)
#define atomic_bool_compare_and_swap __sync_bool_compare_and_swap

#ifdef GCC_ATOMIC_FUNCTIONS
#define atomic_store(x, n) __atomic_store_n(x, n, __ATOMIC_RELEASE)
#elif defined(GCC_SYNC_FUNCTIONS)
#define atomic_store(x, n) do { *(x) = n; __sync_synchronize(); } while (0)
#endif
