/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "unity.h"
#include "unity_test_runner.h"

void app_main(void)
{
    printf(" _  ____     ______   __        __   _    ____ _____ ____   \n");
    printf("| |/ /\\ \\   / / ___| \\ \\      / /__| |__|  _ \\_   _/ ___| \n");
    printf("| ' /  \\ \\ / /\\___ \\  \\ \\ /\\ / / _ \\ '_ \\| |_) || || |    \n");
    printf("| . \\   \\ V /  ___) |  \\ V  V /  __/ |_) |  _ < | || |___ \n");
    printf("|_|\\_\\   \\_/  |____/    \\_/\\_/ \\___|_.__/|_| \\_\\|_| \\____|\n");
    printf("                          Unit Tests                       \n\n");

    unity_run_menu();
}
