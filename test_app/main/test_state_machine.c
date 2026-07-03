/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "unity.h"
#include "state_machine.h"

/* Test states as bit flags */
#define STATE_IDLE       (1ULL << 0)  /* 0x01 */
#define STATE_CONNECTING (1ULL << 1)  /* 0x02 */
#define STATE_CONNECTED  (1ULL << 2)  /* 0x04 */

/* Track which state to move to next (controlled by test) */
static UINT64 s_next_state = STATE_IDLE;

static UINT64 s_time_counter = 0;

static UINT64 test_get_time(UINT64 customData)
{
    (void)customData;
    return s_time_counter++;
}

static STATUS test_get_next_state_idle(UINT64 customData, PUINT64 pNextState)
{
    (void)customData;
    *pNextState = s_next_state;
    return STATUS_SUCCESS;
}

static STATUS test_get_next_state_connecting(UINT64 customData, PUINT64 pNextState)
{
    (void)customData;
    *pNextState = s_next_state;
    return STATUS_SUCCESS;
}

static STATUS test_get_next_state_connected(UINT64 customData, PUINT64 pNextState)
{
    (void)customData;
    *pNextState = s_next_state;
    return STATUS_SUCCESS;
}

static STATUS test_execute_noop(UINT64 customData, UINT64 time)
{
    (void)customData;
    (void)time;
    return STATUS_SUCCESS;
}

/* 3-state machine: IDLE -> CONNECTING -> CONNECTED */
static StateMachineState s_test_states[] = {
    {
        .state = STATE_IDLE,
        .acceptStates = STATE_IDLE | STATE_CONNECTING,  /* can come from IDLE or stay */
        .getNextStateFn = test_get_next_state_idle,
        .executeStateFn = test_execute_noop,
        .stateTransitionHookFunc = NULL,
        .maxLocalStateRetryCount = INFINITE_RETRY_COUNT_SENTINEL,
        .status = STATUS_SUCCESS,
    },
    {
        .state = STATE_CONNECTING,
        .acceptStates = STATE_IDLE | STATE_CONNECTING,  /* accept from IDLE or self-retry */
        .getNextStateFn = test_get_next_state_connecting,
        .executeStateFn = test_execute_noop,
        .stateTransitionHookFunc = NULL,
        .maxLocalStateRetryCount = INFINITE_RETRY_COUNT_SENTINEL,
        .status = STATUS_SUCCESS,
    },
    {
        .state = STATE_CONNECTED,
        .acceptStates = STATE_CONNECTING | STATE_CONNECTED,  /* accept from CONNECTING or self */
        .getNextStateFn = test_get_next_state_connected,
        .executeStateFn = test_execute_noop,
        .stateTransitionHookFunc = NULL,
        .maxLocalStateRetryCount = INFINITE_RETRY_COUNT_SENTINEL,
        .status = STATUS_SUCCESS,
    },
};

#define TEST_STATE_COUNT (sizeof(s_test_states) / sizeof(s_test_states[0]))

TEST_CASE("state machine create and free", "[state_machine]")
{
    PStateMachine pSm = NULL;
    s_next_state = STATE_IDLE;
    s_time_counter = 0;

    STATUS ret = createStateMachine(s_test_states, TEST_STATE_COUNT, 0, test_get_time, 0, &pSm);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);
    TEST_ASSERT_NOT_NULL(pSm);

    /* Verify initial state is IDLE (first state) */
    PStateMachineState pState = NULL;
    ret = getStateMachineCurrentState(pSm, &pState);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);
    TEST_ASSERT_EQUAL(STATE_IDLE, pState->state);

    ret = freeStateMachine(pSm);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);
}

TEST_CASE("state machine transitions IDLE -> CONNECTING -> CONNECTED", "[state_machine]")
{
    PStateMachine pSm = NULL;
    s_time_counter = 0;
    s_next_state = STATE_IDLE;

    STATUS ret = createStateMachine(s_test_states, TEST_STATE_COUNT, 0, test_get_time, 0, &pSm);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);

    /* Transition to CONNECTING */
    s_next_state = STATE_CONNECTING;
    ret = stepStateMachine(pSm);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);

    PStateMachineState pState = NULL;
    ret = getStateMachineCurrentState(pSm, &pState);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);
    TEST_ASSERT_EQUAL(STATE_CONNECTING, pState->state);

    /* Transition to CONNECTED */
    s_next_state = STATE_CONNECTED;
    ret = stepStateMachine(pSm);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);

    ret = getStateMachineCurrentState(pSm, &pState);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);
    TEST_ASSERT_EQUAL(STATE_CONNECTED, pState->state);

    freeStateMachine(pSm);
}

TEST_CASE("state machine rejects invalid transition", "[state_machine]")
{
    PStateMachine pSm = NULL;
    s_time_counter = 0;
    s_next_state = STATE_IDLE;

    STATUS ret = createStateMachine(s_test_states, TEST_STATE_COUNT, 0, test_get_time, 0, &pSm);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);

    /* Try to jump directly from IDLE to CONNECTED (not in CONNECTED's acceptStates) */
    s_next_state = STATE_CONNECTED;
    ret = stepStateMachine(pSm);
    TEST_ASSERT_TRUE(STATUS_FAILED(ret));

    /* Verify we're still in IDLE */
    PStateMachineState pState = NULL;
    ret = getStateMachineCurrentState(pSm, &pState);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);
    TEST_ASSERT_EQUAL(STATE_IDLE, pState->state);

    freeStateMachine(pSm);
}

TEST_CASE("state machine set current state", "[state_machine]")
{
    PStateMachine pSm = NULL;
    s_time_counter = 0;
    s_next_state = STATE_IDLE;

    STATUS ret = createStateMachine(s_test_states, TEST_STATE_COUNT, 0, test_get_time, 0, &pSm);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);

    /* Force set to CONNECTED */
    ret = setStateMachineCurrentState(pSm, STATE_CONNECTED);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);

    PStateMachineState pState = NULL;
    ret = getStateMachineCurrentState(pSm, &pState);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);
    TEST_ASSERT_EQUAL(STATE_CONNECTED, pState->state);

    /* Set to non-existent state should fail */
    ret = setStateMachineCurrentState(pSm, (1ULL << 10));
    TEST_ASSERT_TRUE(STATUS_FAILED(ret));

    freeStateMachine(pSm);
}

TEST_CASE("state machine retry count enforcement", "[state_machine]")
{
    /* Create states with limited retries on CONNECTING */
    StateMachineState retry_states[] = {
        {
            .state = STATE_IDLE,
            .acceptStates = STATE_IDLE | STATE_CONNECTING,
            .getNextStateFn = test_get_next_state_idle,
            .executeStateFn = test_execute_noop,
            .stateTransitionHookFunc = NULL,
            .maxLocalStateRetryCount = INFINITE_RETRY_COUNT_SENTINEL,
            .status = STATUS_SUCCESS,
        },
        {
            .state = STATE_CONNECTING,
            .acceptStates = STATE_IDLE | STATE_CONNECTING,
            .getNextStateFn = test_get_next_state_connecting,
            .executeStateFn = test_execute_noop,
            .stateTransitionHookFunc = NULL,
            .maxLocalStateRetryCount = 2,  /* Allow max 2 retries */
            .status = STATUS_INVALID_ARG,  /* Error to return when max retries exceeded */
        },
        {
            .state = STATE_CONNECTED,
            .acceptStates = STATE_CONNECTING | STATE_CONNECTED,
            .getNextStateFn = test_get_next_state_connected,
            .executeStateFn = test_execute_noop,
            .stateTransitionHookFunc = NULL,
            .maxLocalStateRetryCount = INFINITE_RETRY_COUNT_SENTINEL,
            .status = STATUS_SUCCESS,
        },
    };

    PStateMachine pSm = NULL;
    s_time_counter = 0;

    STATUS ret = createStateMachine(retry_states, 3, 0, test_get_time, 0, &pSm);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);

    /* Move to CONNECTING */
    s_next_state = STATE_CONNECTING;
    ret = stepStateMachine(pSm);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);

    /* Retry in CONNECTING (retry 1) */
    s_next_state = STATE_CONNECTING;
    ret = stepStateMachine(pSm);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);

    /* Retry in CONNECTING (retry 2) */
    ret = stepStateMachine(pSm);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);

    /* Retry 3 should fail (exceeded max of 2) */
    ret = stepStateMachine(pSm);
    TEST_ASSERT_TRUE(STATUS_FAILED(ret));

    freeStateMachine(pSm);
}

TEST_CASE("state machine reset retry count", "[state_machine]")
{
    PStateMachine pSm = NULL;
    s_time_counter = 0;
    s_next_state = STATE_IDLE;

    STATUS ret = createStateMachine(s_test_states, TEST_STATE_COUNT, 0, test_get_time, 0, &pSm);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);

    /* Transition to self a few times */
    ret = stepStateMachine(pSm);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);
    ret = stepStateMachine(pSm);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);

    /* Reset retry count */
    ret = resetStateMachineRetryCount(pSm);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);

    /* Should still be able to step */
    ret = stepStateMachine(pSm);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);

    freeStateMachine(pSm);
}

TEST_CASE("state machine NULL argument checks", "[state_machine]")
{
    STATUS ret = createStateMachine(NULL, 0, 0, test_get_time, 0, NULL);
    TEST_ASSERT_TRUE(STATUS_FAILED(ret));

    ret = freeStateMachine(NULL);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret); /* freeStateMachine is idempotent */

    ret = stepStateMachine(NULL);
    TEST_ASSERT_TRUE(STATUS_FAILED(ret));

    ret = getStateMachineCurrentState(NULL, NULL);
    TEST_ASSERT_TRUE(STATUS_FAILED(ret));

    ret = resetStateMachineRetryCount(NULL);
    TEST_ASSERT_TRUE(STATUS_FAILED(ret));
}

TEST_CASE("state machine with name", "[state_machine]")
{
    PStateMachine pSm = NULL;
    s_time_counter = 0;
    s_next_state = STATE_IDLE;

    STATUS ret = createStateMachineWithName(s_test_states, TEST_STATE_COUNT, 0, test_get_time, 0, "TestSM", &pSm);
    TEST_ASSERT_EQUAL(STATUS_SUCCESS, ret);
    TEST_ASSERT_NOT_NULL(pSm);

    PCHAR name = getStateMachineName(pSm);
    TEST_ASSERT_NOT_NULL(name);
    TEST_ASSERT_EQUAL_STRING("TestSM", name);

    freeStateMachine(pSm);
}
