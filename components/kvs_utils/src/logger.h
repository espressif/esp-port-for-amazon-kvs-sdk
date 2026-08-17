/*
 * Copyright 2021 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */
#ifndef __AWS_KVS_WEBRTC_LOGGER_INCLUDE__
#define __AWS_KVS_WEBRTC_LOGGER_INCLUDE__

#ifdef __cplusplus
extern "C" {
#endif

#include "common_defs.h"
#include "error.h"

/* Max log *format* length (args expand later in esp_log_writev, so a 2 KB "%s"
 * payload is fine). Sits on the caller's stack, and takes the format from every
 * DLOG site linked into the image, not just this component: the longest is 213
 * (SessionDescription.c, "Only %u of %u local transceivers ..."), then 168 there
 * and 157 in IceAgent.c. addLogMetadata() prepends a timestamp + level prefix
 * (~33) once that path is enabled, and PRIu64 expands at compile time, so size
 * for 213 + prefix + slack rather than the 135 this component alone would need.
 * Overflow is a silent SNPRINTF truncation, possibly mid-specifier. */
#define MAX_LOG_FORMAT_LENGTH 320

// Set the global log level
#define SET_LOGGER_LOG_LEVEL(l) loggerSetLogLevel((l))

// Get the global log level
#define GET_LOGGER_LOG_LEVEL() loggerGetLogLevel()

/*
 * Set log level
 * @UINT32 - IN - target log level
 */
VOID loggerSetLogLevel(UINT32);

/**
 * Get current log level
 * @return - UINT32 - current log level
 */
UINT32 loggerGetLogLevel();

/**
 * Prepend log message with timestamp and thread id.
 * @PCHAR - IN - buffer holding the log
 * @UINT32 - IN - buffer length
 * @PCHAR - IN - log format string
 * @UINT32 - IN - log level
 * @return - VOID
 */
VOID addLogMetadata(PCHAR, UINT32, PCHAR, UINT32);

#ifdef __cplusplus
}
#endif
#endif /* __AWS_KVS_WEBRTC_LOGGER_INCLUDE__ */
