/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Host (IDF linux target) only: usrsctp's pthreads start with the FreeRTOS tick
 * (SIGALRM) blocked, since a tick on a plain pthread parks it and freezes the
 * scheduler. usrsctp's sources call this in place of pthread_create (see the
 * CMakeLists), which leaves the usrsctp fork untouched.
 */

#undef pthread_create
#include <pthread.h>
#include <signal.h>

int esp_usrsctp_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg)
{
    sigset_t tick, saved;
    sigemptyset(&tick);
    sigaddset(&tick, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &tick, &saved);
    int ret = pthread_create(thread, attr, start_routine, arg);
    pthread_sigmask(SIG_SETMASK, &saved, NULL);
    return ret;
}
