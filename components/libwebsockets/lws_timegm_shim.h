/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Prototype shim for picolibc timegm().
 *
 * picolibc ships the timegm() symbol in libc.a but gates its <time.h>
 * declaration behind BSD/GNU visibility, which ESP's forced POSIX feature
 * macros disable. Force-included into the libwebsockets build so date.c's
 * lws_http_date_parse_unix() has a correct (64-bit time_t) prototype instead
 * of an implicit int return. timegm() interprets a struct tm as UTC, which is
 * what an HTTP "GMT" Date header needs (mktime() would apply the local TZ).
 */
#pragma once
#include <time.h>

#ifndef __ESP_LWS_TIMEGM_DECLARED
#define __ESP_LWS_TIMEGM_DECLARED
time_t timegm(struct tm *__tp);
#endif
