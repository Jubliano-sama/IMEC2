## 1. Design reasoning
This document is more of a proposal than gospel. Treat it as a possible way to implement stuff at a high level.

The firmware contains several operations that last over time. A packet may wait for a route, a connection may wait for its next radio window, an accepted click may temporarily take over channel 5, and the gateway BLE link may be blocked while UWB traffic continues.

Putting all of this in one device-wide state machine would create many combined states. Using only flags and callbacks would avoid that large diagram, but retry, timeout, and cleanup rules would become spread across the code.

A useful middle ground is to give each long-lived operation a small state machine. Plain functions remain better for calculations and one-step work. A component probably needs state when it:

- waits for something that will arrive later;
- can be interrupted, retried, or timed out;
- owns data that must survive those interruptions.

This leads to the following starting structure:

```mermaid
flowchart LR
    Inputs["Button, timers, BLE, received frames"] --> Events["Event messages"]
    Events --> Dispatch["One-at-a-time dispatcher"]

    Dispatch --> Click["Click operation"]
    Dispatch --> AnchorClick["Anchor click service"]
    Dispatch --> Route["Route and connection state"]
    Dispatch --> Delivery["Packet delivery state"]
    Dispatch --> Control["Enumeration and survey"]

    Click --> Effects["Requested work"]
    AnchorClick --> Effects
    Route --> Effects
    Delivery --> Effects
    Control --> Effects

    Effects --> Radio["Radio Manager"]
    Effects --> Timers["Timer service"]
    Effects --> BLE["BLE service"]
    Effects --> Queues["Packet queues"]
    Effects --> LED["Indicator service"]

    Radio --> Events
    Timers --> Events
    BLE --> Events
```

Most boundaries in this diagram are soft. Two components may share one source file, and a simple component may use a flat enum instead of a hierarchical state machine.

Two boundaries should remain strict:

1. The Radio Manager owns the DWM3000 because channel 5 and channel 9 use one physical radio.
2. One logical packet has one custody owner because ACK handling must never leave both nodes believing the other owns it.

## 2. Events, states, and effects

An **event message** is a small record saying that something has happened. It is input to a state machine.

Examples are `BUTTON_RELEASED`, `TIMER_EXPIRED`, `RADIO_RX_COMPLETE`, `WAKE_CLAIM_RECEIVED`, and `GATEWAY_ACK_RECEIVED`.

| Term | Meaning | Example |
|---|---|---|
| State | What an operation is currently doing or waiting for | `WAIT_DISCOVERY` |
| Event message | A fact that has just arrived | `DISCOVERY_TIMEOUT` |
| Transition | The response to an event in the current state | Move to `RETRY_WAIT` |
| Effect request | Work outside the state machine | Start a timer or submit a radio job |

A complete transition can be written as:

```text
WAIT_DISCOVERY + DISCOVERY_TIMEOUT
    -> RETRY_WAIT
    -> request START_RETRY_TIMER
```

A typical event contains:

```text
type, source, timestamp, operation_id, generation, payload
```

The generation changes when an operation is restarted. A late timer or radio result from an older generation can then be ignored.

Callbacks and interrupt handlers should only create event messages. The dispatcher handles one event at a time. A state machine updates its own context, changes state, and requests work. The service that performs that work later returns a result event.

```mermaid
sequenceDiagram
    participant C as Callback or ISR
    participant Q as Event queue
    participant S as State machine
    participant W as Radio, timer, or BLE service

    C->>Q: Add event message
    Q->>S: Handle one event
    S->>W: Request work
    W->>Q: Add result event
    Q->>S: Continue operation
```

Hierarchy is only useful when several child states share behavior. For example, all states inside `CLICK_ACTIVE` can share the 15-second deadline, cancellation handling, radio-failure handling, and cleanup. A flat state machine is simpler when no such shared behavior exists.

## 3. Radio Manager

The DWM3000 can run only one job at a time and must retune between channels. A single Radio Manager makes this hardware limit visible to every protocol without forcing protocol code into the driver.

A radio request contains the owner, operation ID, channel, RX or TX mode, earliest start, deadline, maximum duration, and priority class.

```mermaid
stateDiagram-v2
    [*] --> Off
    Off --> Starting: first request
    Starting --> Ready5: initialization succeeds
    Starting --> Recovering: initialization fails

    Ready5 --> Busy5: channel-5 job
    Busy5 --> Ready5: complete
    Ready5 --> Retune9: channel-9 job selected
    Retune9 --> Ready9: complete

    Ready9 --> Busy9: channel-9 job
    Busy9 --> Ready9: complete
    Ready9 --> Retune5: channel-5 job selected
    Retune5 --> Ready5: complete

    Busy5 --> Recovering: driver error
    Busy9 --> Recovering: driver error
    Recovering --> Starting: bounded retry
    Recovering --> Off: recovery fails
```

Suggested scheduling policy:

| Work | Behavior |
|---|---|
| Accepted click or gateway control on channel 5 | May cancel lower-priority channel-9 work. |
| Rejected or invalid wake claim | Changes nothing. Validation happens before pre-emption. |
| Route-request wake | Uses a permitted channel-5 window without destroying an active rhythm. |
| Channel-9 receive turn | Keep when possible because it checks peer liveness. |
| Channel-9 transmit turn | May be clipped, skipped, or retried for required channel-5 work. |
| Gateway BLE work | Remains queued while UWB has priority. |

The manager reports completion, timeout, cancellation, or failure to the request owner. It does not decide whether the protocol operation succeeded.

## 4. Clicker state

Button timing and UWB ranging are separate concerns. Keeping the gesture state separate avoids mixing debounce and long-press logic into the ranging protocol. They may still live in the same clicker module.

### 4.1 Button gesture

```mermaid
stateDiagram-v2
    [*] --> Idle
    Idle --> Debounce: button pressed
    Debounce --> Idle: released before 50 ms
    Debounce --> Pressed: stable for 50 ms

    Pressed --> NormalClick: released before 1.5 s
    Pressed --> WaitLongRelease: held for 1.5 s

    WaitLongRelease --> SelfTestArmed: released
    SelfTestArmed --> SelfTest: short press within 3 s
    SelfTestArmed --> Idle: 3 s expires

    NormalClick --> Idle: request accepted
    SelfTest --> Idle: request accepted
```

### 4.2 Normal click

```mermaid
stateDiagram-v2
    [*] --> ClickActive

    state ClickActive {
        [*] --> Create
        Create --> Politeness: create event ID and nonce

        Politeness --> WaitPeer: higher-priority traffic found
        WaitPeer --> Politeness: peer wait ends
        Politeness --> Wake: channel is clear

        Wake --> Discover: wake train ends
        Discover --> Retry: no replies
        Discover --> Release: 1 or 2 replies
        Discover --> Schedule: at least 3 replies

        Release --> Retry: release sent
        Schedule --> Range: schedule sent
        Range --> Evaluate: full burst ends

        Evaluate --> Success: at least 3 unique ranges
        Evaluate --> Retry: fewer than 3 ranges
        Retry --> Politeness: time and attempts remain
        Retry --> Failure: limit reached

        Success --> [*]
        Failure --> [*]
    }

    note right of ClickActive
        The parent handles the 15 s deadline,
        cancellation, fatal radio errors,
        and cleanup for every child state.
    end note

    ClickActive --> [*]
```
## 6. Mesh route, connection, and packet delivery

These states answer different questions:

- **Route:** Which parent should carry traffic?
- **Connection:** When can this anchor and that parent use channel 9 together?
- **Delivery:** Who owns this exact packet, and has its transfer finished?

They are worth keeping conceptually separate because one can fail while the others remain useful. For example, an accepted click may remove connection timing while the route remains valid and the packet remains owned.

```mermaid
flowchart LR
    P["Packet waiting"] --> R{Route ready?}
    R -- No --> Find["Direct probe, then route discovery"]
    Find --> R
    R -- Yes --> C{Connection ready?}
    C -- No --> Negotiate["Negotiate channel-9 timing"]
    Negotiate --> C
    C -- Yes --> TX["Send in TX window"]
    TX --> ACK{ACK received?}
    ACK -- Yes --> Done["Custody transferred or delivery complete"]
    ACK -- No --> Retry["Keep custody and retry"]
    Retry --> R
```

### 6.2 Connection state

Each anchor may have one upstream and one downstream instance. Each instance owns its own phase and event counter.

```mermaid
stateDiagram-v2
    [*] --> Empty
    Empty --> Negotiating: route needs timing
    Negotiating --> Active: PROPOSE and ACCEPT
    Negotiating --> Empty: timeout or rejection

    Active --> Active: event completes
    Active --> Stale: 8 attempted-transfer failures
    Active --> Stale: 5 min peer supervision timeout
    Active --> Empty: accepted click removes timing
    Active --> Closing: route closes

    Stale --> Negotiating: repair
    Stale --> Empty: route removed
    Closing --> Empty: close ends
```

The successful `PROPOSE` defines the phase. Direction reverses after each event of that connection. Upstream and downstream counters advance independently. Empty transmit turns may be skipped, while receive turns remain useful liveness checks.

### 6.3 Delivery state

Each logical packet receives a small delivery record.

```mermaid
stateDiagram-v2
    [*] --> Owned
    Owned --> WaitRoute: no route
    Owned --> WaitConnection: route ready, timing missing
    Owned --> WaitTx: path ready

    WaitRoute --> WaitConnection: route found
    WaitConnection --> WaitTx: timing ready
    WaitTx --> WaitTx: deferred before RF
    WaitTx --> WaitAck: RF starts

    WaitAck --> Transferred: hop ACK
    WaitAck --> Delivered: gateway ACK
    WaitAck --> Retry: timeout

    Retry --> WaitTx: path still usable
    Retry --> WaitRoute: path invalidated
    Retry --> Failed: retry budget ends

    Transferred --> [*]
    Delivered --> [*]
    Failed --> [*]
```

A relay may ACK only after it has accepted custody:

```text
validate -> deduplicate -> reserve storage -> copy packet -> ACK
```

Pre-RF delay consumes no attempt. RF start does. Missing entries from a gateway batch ACK remain owned. After the fourth gateway-ACK failure, the selected parent is invalidated and held down for 60 seconds.

## 7. Gateway and host link

Gateway UWB and BLE have independent flow control. UWB packets may arrive while BLE notifications are blocked, and BLE commands may arrive while the radio is busy. Keeping their state separate prevents one interface from blocking the other, although both may remain in one gateway module.

The UWB root can use the simple flow `LISTEN_9 -> RECEIVE_BATCH -> ACCEPT_BATCH -> SEND_ACK -> LISTEN_9`. It does not use the normal anchor-to-anchor cadence. Accepted packets enter a bounded gateway-to-host queue.

The visible host sequence remains:

```mermaid
sequenceDiagram
    participant GUI as PC GUI
    participant GW as Gateway
    participant A as Anchors

    GUI->>GW: Start Here-I-Am
    GW-->>A: Wake flood and Here-I-Am frames
    GW->>GUI: Gateway radio work finished

    GUI->>GW: Send operation command
    GW-->>A: Wake flood and command frames
    GW->>GUI: Gateway radio work finished

    A-->>GW: Results through reliable delivery
    GW-->>GUI: Results over BLE
```


## 8. Enumeration and survey

### 8.1 Enumeration

```mermaid
sequenceDiagram
    participant GW as Gateway
    participant A as Anchors

    GW-->>A: CLAIM wake and frames
    A-->>GW: Reliable identity RESPONSES
    GW->>GW: Freeze immutable slot table
    GW-->>A: TABLE wake and frames
    A->>A: Validate and store complete table
```

Each response stays under normal packet custody until gateway ACK. An anchor replaces its stored table only after validating the complete new table. Repeated CLAIM and TABLE frames should be harmless.

### 8.2 Survey coordinator

```mermaid
stateDiagram-v2
    [*] --> Idle
    Idle --> SendConfig: host starts survey
    SendConfig --> Discovery
    Discovery --> CollectReports: rounds end
    CollectReports --> BuildGraph: report window ends

    BuildGraph --> SelectPairs
    SelectPairs --> ArmPairs: useful pairs remain
    SelectPairs --> Publish: no useful pairs remain

    ArmPairs --> WaitResults
    WaitResults --> UpdateGraph: result or timeout
    UpdateGraph --> SelectPairs: work remains

    Publish --> Complete: graph complete
    Publish --> Partial: graph incomplete
    Complete --> Idle
    Partial --> Idle
```

Pair selection can remain a normal function that receives the partial graph and pending jobs. It may choose pairs in parallel only when their endpoints and known neighbour sets do not overlap.

The gateway's durable survey generation is also the explicit restart-repair boundary. When an anchor accepts a strictly newer generation, it aborts the older producer, abandons any exact older discovery or pair-result communication handles, and only then releases their RAM custody. The gateway retains the discovery START delivery and redrives that same generation at 10, 20, 30, and 40 seconds before the shared 90-second execution instant, so an anchor that spent an earlier wave draining obsolete custody still gets another bounded admission opportunity. Same-generation redrives remain idempotent, lower generations remain stale, and this cancellation never masquerades as a gateway acknowledgement.

Pair arming follows the fixed order:

```mermaid
sequenceDiagram
    participant GW as Gateway
    participant I as Initiator
    participant R as Responder

    GW->>I: PREPARE initiator
    GW->>R: PREPARE responder
    GW->>R: START responder with future delay
    GW->>I: START initiator with future delay
    Note over I,R: Subtract packet age and calculate one start time
    I->>R: Five DS-TWR exchanges
    R-->>GW: Five reliable raw sample results
    GW-->>GUI: Host-retained sample records
    GUI->>GUI: Calculate median
```

The anchor-side pair flow can remain `IDLE -> PREPARED -> ARMED -> WAIT_START -> RANGE -> COMPLETE/RESULT_OWNED`. A late command returns `MISSED_DEADLINE` instead of starting immediately. A failed pair repeats the complete PREPARE and START sequence, with at most two reruns. An incomplete graph is published as partial.

## 9. Implementation and verification

No state-machine framework is required. A plain enum, context struct, and handler are enough.

The handler should return quickly. It should not sleep, poll hardware, allocate dynamic memory, or directly edit another component's context.

Useful invariants for native and integration tests are:

- only one DWM3000 job is active;
- channel 5 and channel 9 are never active together;
- a rejected claim changes no mesh state;
- a stale event cannot affect a newer generation;
- a click attempt is counted only when RF starts;
- a relay never ACKs a packet it could not store;
- one packet has one custody owner;
- a partial survey is never reported as complete.

A small transition trace helps hardware debugging:

```text
timestamp, machine, instance, old_state, event, new_state, reason
```

## Conclusion

The recommended starting point is this set of small cooperating state machines. Merge a boundary when two states always change together. Split one when independent timing, retry, or ownership makes the combined code harder to reason about. Keep the Radio Manager and packet custody boundaries strict because the hardware and reliability rules depend on them.
