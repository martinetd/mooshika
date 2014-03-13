#define ERROR_LOG(fmt, args...) fprintf(stderr, "ERROR: %s (%d), %s: " fmt "\n", __FILE__, __LINE__, __func__, ##args)
//#define ERROR_LOG(fmt, args...)
#define INFO_LOG(debug, fmt, args...) 	if (debug) fprintf(stderr, "INFO:  %s (%d), %s: " fmt "\n", __FILE__, __LINE__, __func__, ##args)
//#define INFO_LOG(fmt, args...)

#define TEST_Z(x)  do { int retval; if ( (retval=x)) { ERROR_LOG("error: " #x " failed (returned %d: %s).", retval, strerror(retval) ); exit(retval); } } while (0)
#define TEST_NZ(x) do { if (!(x)) { ERROR_LOG("error: " #x " failed (returned zero/null)."); exit(-1); }} while (0)

#include "atomics.h"

#define set_size(val, unit) do {                                 \
	switch(unit[0]) {                                        \
		case 'k':                                        \
		case 'K':                                        \
			val *= 1024;                             \
			break;                                   \
		case 'm':                                        \
		case 'M':                                        \
			val *= 1024 * 1024;                      \
			break;                                   \
		case 'g':                                        \
		case 'G':                                        \
			val *= 1024 * 1024 * 1024;               \
			break;                                   \
		default:                                         \
			ERROR_LOG("unknown unit '%c'", unit[0]); \
			val = 0;                                 \
	}                                                        \
} while (0)


#define NSEC_IN_SEC 1000000000

static inline void sub_timespec(uint64_t *new, struct timespec *x, struct timespec *y) {
	if (y->tv_nsec < x->tv_nsec) {
		*new = (y->tv_sec - x->tv_sec - 1) * NSEC_IN_SEC +
			y->tv_nsec + NSEC_IN_SEC - x->tv_nsec;
	} else {
		*new = (y->tv_sec - x->tv_sec) * NSEC_IN_SEC +
			y->tv_nsec - x->tv_nsec;
	}
}
