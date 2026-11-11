/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Upstream Tls_mbedtls.c that trusts the ESP-IDF certificate bundle, not a CA file.
 * Keep the rest identical to upstream, except the differences listed in port/README.md. */
#define LOG_CLASS "TLS_esp"
#include "../Include_i.h"

#include "esp_crt_bundle.h"

STATUS createTlsSession(PTlsSessionCallbacks pCallbacks, PTlsSession* ppTlsSession)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PTlsSession pTlsSession = NULL;

    CHK(ppTlsSession != NULL && pCallbacks != NULL && pCallbacks->outboundPacketFn != NULL, STATUS_NULL_ARG);

    pTlsSession = (PTlsSession) MEMCALLOC(1, SIZEOF(TlsSession));
    CHK(pTlsSession != NULL, STATUS_NOT_ENOUGH_MEMORY);

    CHK_STATUS(createIOBuffer(DEFAULT_MTU_SIZE_BYTES, &pTlsSession->pReadBuffer));
    pTlsSession->callbacks = *pCallbacks;
    pTlsSession->state = TLS_SESSION_STATE_NEW;

    // initialize mbedtls stuff with sane values
#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_entropy_init(&pTlsSession->entropy);
    mbedtls_ctr_drbg_init(&pTlsSession->ctrDrbg);
#endif
    mbedtls_x509_crt_init(&pTlsSession->cacert);
    mbedtls_ssl_config_init(&pTlsSession->sslCtxConfig);
    mbedtls_ssl_init(&pTlsSession->sslCtx);
#if MBEDTLS_VERSION_MAJOR < 4
    CHK(mbedtls_ctr_drbg_seed(&pTlsSession->ctrDrbg, mbedtls_entropy_func, &pTlsSession->entropy, NULL, 0) == 0, STATUS_CREATE_SSL_FAILED);
#endif

CleanUp:

    if (STATUS_FAILED(retStatus) && pTlsSession != NULL) {
        freeTlsSession(&pTlsSession);
    }

    if (ppTlsSession != NULL) {
        *ppTlsSession = pTlsSession;
    }

    LEAVES();
    return retStatus;
}

STATUS freeTlsSession(PTlsSession* ppTlsSession)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    PTlsSession pTlsSession = NULL;

    CHK(ppTlsSession != NULL, STATUS_NULL_ARG);

    pTlsSession = *ppTlsSession;
    CHK(pTlsSession != NULL, retStatus);

#if MBEDTLS_VERSION_MAJOR < 4
    mbedtls_entropy_free(&pTlsSession->entropy);
    mbedtls_ctr_drbg_free(&pTlsSession->ctrDrbg);
#endif
    mbedtls_x509_crt_free(&pTlsSession->cacert);
    mbedtls_ssl_config_free(&pTlsSession->sslCtxConfig);
    mbedtls_ssl_free(&pTlsSession->sslCtx);

    freeIOBuffer(&pTlsSession->pReadBuffer);
    retStatus = tlsSessionShutdown(pTlsSession);
    SAFE_MEMFREE(*ppTlsSession);

CleanUp:
    return retStatus;
}

INT32 tlsSessionSendCallback(PVOID customData, const unsigned char* buf, ULONG len)
{
    STATUS retStatus = STATUS_SUCCESS;
    PTlsSession pTlsSession = (PTlsSession) customData;

    CHK(pTlsSession != NULL, STATUS_NULL_ARG);

    retStatus = pTlsSession->callbacks.outboundPacketFn(pTlsSession->callbacks.outBoundPacketFnCustomData, (PBYTE) buf, len);
    if (STATUS_FAILED(retStatus)) {
        // Part of the record may already be on the wire, so the stream can't continue
        tlsSessionChangeState(pTlsSession, TLS_SESSION_STATE_CLOSED);
    }

CleanUp:

    return STATUS_FAILED(retStatus) ? -retStatus : len;
}

INT32 tlsSessionReceiveCallback(PVOID customData, unsigned char* buf, ULONG len)
{
    STATUS retStatus = STATUS_SUCCESS;
    PTlsSession pTlsSession = (PTlsSession) customData;
    PIOBuffer pBuffer;
    UINT32 readBytes = MBEDTLS_ERR_SSL_WANT_READ;

    CHK(pTlsSession != NULL, STATUS_NULL_ARG);

    pBuffer = pTlsSession->pReadBuffer;

    if (pBuffer->off < pBuffer->len) {
        retStatus = ioBufferRead(pBuffer, buf, len, &readBytes);
    }

CleanUp:

    return STATUS_FAILED(retStatus) ? -retStatus : readBytes;
}

STATUS tlsSessionStartWithHostname(PTlsSession pTlsSession, BOOL isServer, PCHAR hostname)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    INT32 sslRet;

    CHK(pTlsSession != NULL, STATUS_NULL_ARG);
    CHK(pTlsSession->state == TLS_SESSION_STATE_NEW, retStatus);

    CHK(mbedtls_ssl_config_defaults(&pTlsSession->sslCtxConfig, isServer ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) == 0,
        STATUS_CREATE_SSL_FAILED);

    // Trust ESP-IDF's CA bundle, not a CA file
    CHK(esp_crt_bundle_attach(&pTlsSession->sslCtxConfig) == ESP_OK, STATUS_CREATE_SSL_FAILED);

    // For mbedTLS 3.x, use appropriate verification mode
    if (hostname != NULL) {
        // If a hostname is provided, use strict verification
        mbedtls_ssl_conf_authmode(&pTlsSession->sslCtxConfig, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        // Otherwise, use optional verification for IP-based connections
        mbedtls_ssl_conf_authmode(&pTlsSession->sslCtxConfig, MBEDTLS_SSL_VERIFY_OPTIONAL);
    }

#if !MBEDTLS_V4_OR_LATER
    /* mbedTLS 4 removes mbedtls_ssl_conf_rng() — PSA Crypto provides the RNG
     * internally once psa_crypto_init() has run, so no explicit binding is needed. */
    mbedtls_ssl_conf_rng(&pTlsSession->sslCtxConfig, mbedtls_ctr_drbg_random, &pTlsSession->ctrDrbg);
#endif
    CHK(mbedtls_ssl_setup(&pTlsSession->sslCtx, &pTlsSession->sslCtxConfig) == 0, STATUS_SSL_CTX_CREATION_FAILED);

    // Set the hostname for certificate verification (for mbedTLS 3.x compatibility)
    if (!isServer && hostname != NULL) {
        // Use the provided hostname for certificate verification
        CHK(mbedtls_ssl_set_hostname(&pTlsSession->sslCtx, hostname) == 0, STATUS_SSL_CTX_CREATION_FAILED);
    }

    mbedtls_ssl_set_bio(&pTlsSession->sslCtx, pTlsSession, (mbedtls_ssl_send_t*) (void*) tlsSessionSendCallback,
                        (mbedtls_ssl_recv_t*) (void*) tlsSessionReceiveCallback, NULL);

    /* init and send handshake */
    tlsSessionChangeState(pTlsSession, TLS_SESSION_STATE_CONNECTING);
    sslRet = mbedtls_ssl_handshake(&pTlsSession->sslCtx);
    CHK(sslRet == MBEDTLS_ERR_SSL_WANT_READ || sslRet == MBEDTLS_ERR_SSL_WANT_WRITE, STATUS_SSL_CTX_CREATION_FAILED);
    LOG_MBEDTLS_ERROR("mbedtls_ssl_handshake", sslRet);

CleanUp:

    CHK_LOG_ERR(retStatus);

    LEAVES();
    return retStatus;
}

STATUS tlsSessionStart(PTlsSession pTlsSession, BOOL isServer)
{
    return tlsSessionStartWithHostname(pTlsSession, isServer, NULL);
}

STATUS tlsSessionProcessPacket(PTlsSession pTlsSession, PBYTE pData, UINT32 bufferLen, PUINT32 pDataLen)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    INT32 sslRet, readBytes = 0;
    BOOL iterate = TRUE;
    PIOBuffer pReadBuffer;

    CHK(pTlsSession != NULL && pData != NULL && pDataLen != NULL, STATUS_NULL_ARG);
    CHK(pTlsSession->state != TLS_SESSION_STATE_NEW, STATUS_SOCKET_CONNECTION_NOT_READY_TO_SEND);
    CHK(pTlsSession->state != TLS_SESSION_STATE_CLOSED, STATUS_SOCKET_CONNECTION_CLOSED_ALREADY);

    pReadBuffer = pTlsSession->pReadBuffer;
    CHK_STATUS(ioBufferWrite(pReadBuffer, pData, *pDataLen));

    // read application data
    while (iterate && pReadBuffer->off < pReadBuffer->len && bufferLen > 0) {
        sslRet = mbedtls_ssl_read(&pTlsSession->sslCtx, pData + readBytes, bufferLen);
        if (sslRet > 0) {
            readBytes += sslRet;
            bufferLen -= sslRet;
        } else if (sslRet == 0 || sslRet == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            // if sslRet is 0, the connection is closed already.
            // if sslRet is MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY, the client notified us that the connection is going to be closed.
            // In either case, we'll make sure that the state will change to CLOSED. If it's already closed, it'll be just a noop.
            DLOGD("Detected TLS close_notify alert");
            CHK_STATUS(tlsSessionShutdown(pTlsSession));
            iterate = FALSE;
        } else if (sslRet == MBEDTLS_ERR_SSL_WANT_READ || sslRet == MBEDTLS_ERR_SSL_WANT_WRITE) {
            iterate = FALSE;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
        } else if (sslRet == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            // A TLS 1.3 session ticket, not an error: keep reading
#endif
        } else {
            LOG_MBEDTLS_ERROR("mbedtls_ssl_read", sslRet);
            readBytes = 0;
            retStatus = STATUS_INTERNAL_ERROR;
            iterate = FALSE;
            // Any other error is final: failed certificate check, alert, bad record
            tlsSessionChangeState(pTlsSession, TLS_SESSION_STATE_CLOSED);
        }
    }
#if MBEDTLS_BEFORE_V3
    if (pTlsSession->sslCtx.state == MBEDTLS_SSL_HANDSHAKE_OVER) {
#elif MBEDTLS_VERSION_NUMBER < 0x03040000
    if (pTlsSession->sslCtx.MBEDTLS_PRIVATE(state) == MBEDTLS_SSL_HANDSHAKE_OVER) {
#else
    if (mbedtls_ssl_is_handshake_over(&pTlsSession->sslCtx) != 0) {
#endif
        if (pTlsSession->state == TLS_SESSION_STATE_CONNECTING) {
            tlsSessionChangeState(pTlsSession, TLS_SESSION_STATE_CONNECTED);
        }
    }

CleanUp:
    if (pDataLen != NULL) {
        *pDataLen = readBytes;
    }

    // CHK_LOG_ERR might be too verbose
    if (STATUS_FAILED(retStatus)) {
        DLOGD("Warning: reading socket data failed with 0x%08x", retStatus);
    }

    LEAVES();
    return retStatus;
}

STATUS tlsSessionPutApplicationData(PTlsSession pTlsSession, PBYTE pData, UINT32 dataLen)
{
    ENTERS();
    STATUS retStatus = STATUS_SUCCESS;
    UINT32 writtenBytes = 0;
    BOOL iterate = TRUE;
    INT32 sslRet;

    CHK(pTlsSession != NULL, STATUS_NULL_ARG);

    while (iterate && writtenBytes < dataLen) {
        sslRet = mbedtls_ssl_write(&pTlsSession->sslCtx, pData + writtenBytes, dataLen - writtenBytes);
        if (sslRet > 0) {
            writtenBytes += sslRet;
        } else if (sslRet == MBEDTLS_ERR_SSL_WANT_READ || sslRet == MBEDTLS_ERR_SSL_WANT_WRITE) {
            iterate = FALSE;
        } else {
            LOG_MBEDTLS_ERROR("mbedtls_ssl_write", sslRet);
            writtenBytes = 0;
            retStatus = STATUS_INTERNAL_ERROR;
            iterate = FALSE;
        }
    }

CleanUp:
    LEAVES();
    return retStatus;
}

STATUS tlsSessionShutdown(PTlsSession pTlsSession)
{
    STATUS retStatus = STATUS_SUCCESS;

    CHK(pTlsSession != NULL, STATUS_NULL_ARG);
    CHK(pTlsSession->state != TLS_SESSION_STATE_CLOSED, retStatus);

    while (mbedtls_ssl_close_notify(&pTlsSession->sslCtx) == MBEDTLS_ERR_SSL_WANT_WRITE) {
        // keep flushing outgoing buffer until nothing left
    }
    CHK_STATUS(tlsSessionChangeState(pTlsSession, TLS_SESSION_STATE_CLOSED));

CleanUp:

    CHK_LOG_ERR(retStatus);

    return retStatus;
}
