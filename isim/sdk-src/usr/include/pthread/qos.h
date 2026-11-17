#pragma once
#include <sys/qos.h>
#include <pthread.h>
__BEGIN_DECLS
int pthread_set_qos_class_self_np(qos_class_t qos_class, int relative_priority);
int pthread_get_qos_class_np(pthread_t thread, qos_class_t *qos_class, int *relative_priority);
typedef struct pthread_override_s *pthread_override_t;
pthread_override_t pthread_override_qos_class_start_np(pthread_t thread, qos_class_t qos_class, int relative_priority);
int pthread_override_qos_class_end_np(pthread_override_t override);
__END_DECLS
