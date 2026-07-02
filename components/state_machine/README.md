# state_machine

A small generic state-machine utility used by the KVS signaling/WebRTC lifecycle (connection,
ICE, session states). Components declare a table of `StateMachineState` entries — each with its
state value, accepted predecessor states, a handler, and a transition function — and the engine
drives transitions.

## Key APIs

- `createStateMachine()` / `createStateMachineWithName()` — build a machine from a state table.
- `stepStateMachine()` — evaluate the current state and transition if its condition is met.
- `setStateMachineCurrentState()` / `getStateMachineState()` — inspect/force state.
- `freeStateMachine()`.

The engine is single-executor by design (the caller drives `stepStateMachine`); it does not add
its own locking.

## Origin

Ported from the [Amazon Kinesis Video Streams PIC](https://github.com/awslabs/amazon-kinesis-video-streams-pic) and adapted for ESP-IDF.
