#ifndef EIGRP_TEST_FRR_PTHREAD_H
#define EIGRP_TEST_FRR_PTHREAD_H

#include <pthread.h>

int frr_pthread_non_controlled_startup(pthread_t thread, const char *name,
				       const char *os_name);
void frr_pthread_non_controlled_shutdown(pthread_t thread);

#endif /* EIGRP_TEST_FRR_PTHREAD_H */
