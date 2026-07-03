/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* UTC timegm() for the libwebsockets build.
 *
 * picolibc declares timegm() but its definition is not linkable in the ESP
 * picolibc configuration (undefined reference at link). lws_http_date_parse_unix()
 * needs timegm() to interpret an HTTP "GMT" Date header as UTC; the only other
 * option lws compiles is mktime(), which applies the local TZ and corrupts the
 * server time by the TZ offset (breaks SigV4 clock-skew handling on the ws-off
 * signaling path). Provide a dependency-free UTC conversion so lws links and
 * parses Date headers correctly regardless of the configured local timezone.
 */
#include <time.h>

static int is_leap(int y)
{
    return ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
}

time_t timegm(struct tm *tm)
{
    static const int mdays_cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};

    int year = tm->tm_year + 1900;
    int month = tm->tm_mon;
    if (month < 0) {
        year += (month - 11) / 12;
        month = 12 + (month % 12);
    } else if (month > 11) {
        year += month / 12;
        month %= 12;
    }

    long long days = 0;
    if (year >= 1970)
        for (int y = 1970; y < year; ++y) days += 365 + is_leap(y);
    else
        for (int y = year; y < 1970; ++y) days -= 365 + is_leap(y);

    days += mdays_cum[month];
    if (month > 1 && is_leap(year)) days += 1;
    days += (tm->tm_mday - 1);

    return (time_t)(days * 86400LL
                    + (long long)tm->tm_hour * 3600
                    + (long long)tm->tm_min * 60
                    + (long long)tm->tm_sec);
}
