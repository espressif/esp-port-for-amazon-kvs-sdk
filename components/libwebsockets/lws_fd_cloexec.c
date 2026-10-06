/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* lws defines its FreeRTOS lws_plat_apply_FD_CLOEXEC() only with LWS_WITH_FILE_OPS, which
 * this component disables, yet its socket code calls it. Weak, so lws's own wins once it has one. */
__attribute__((weak)) int lws_plat_apply_FD_CLOEXEC(int n)
{
    (void) n;
    return 0;
}
