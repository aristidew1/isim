/* isim: C versions of the Observation library's runtime hooks (upstream Locking.cpp / ThreadLocal.cpp use
 * Swift's internal threading headers): a pthread mutex for ObservationRegistrar and a thread-local
 * pointer for withObservationTracking. Same symbols and Swift calling convention. */
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#define HOOK __attribute__((swiftcall, visibility("hidden")))

HOOK size_t _swift_observation_lock_size(void) { return sizeof(pthread_mutex_t); }
HOOK void _swift_observation_lock_init(pthread_mutex_t *lock) { pthread_mutex_init(lock, NULL); }
HOOK void _swift_observation_lock_lock(pthread_mutex_t *lock) { pthread_mutex_lock(lock); }
HOOK void _swift_observation_lock_unlock(pthread_mutex_t *lock) { pthread_mutex_unlock(lock); }

static pthread_key_t tls_key;
static pthread_once_t tls_once = PTHREAD_ONCE_INIT;
static void tls_make(void) { if (pthread_key_create(&tls_key, NULL)) { fprintf(stderr, "Observation: no thread-local key\n"); abort(); } }
HOOK void *_swift_observation_tls_get(void) { pthread_once(&tls_once, tls_make); return pthread_getspecific(tls_key); }
HOOK void _swift_observation_tls_set(void *value) { pthread_once(&tls_once, tls_make); pthread_setspecific(tls_key, value); }

/* isim (SwiftUI): sets the thread's withObservationTracking access list and returns the one it replaces. A view's body
 * is tracked on its own: its reads are not added to the tracking of the view that contains it (SwiftUI re-evaluates
 * only the bodies that read what changed). Exported, not part of Apple's API. */
__attribute__((swiftcall)) void *_isim_observation_tls_swap(void *value) {
    pthread_once(&tls_once, tls_make);
    void *old = pthread_getspecific(tls_key);
    pthread_setspecific(tls_key, value);
    return old;
}
