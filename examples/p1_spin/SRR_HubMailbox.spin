{{
==============================================================================
  SRR_HubMailbox.spin
  Parallax Propeller P1 Multi-Cog Synchronous Message Passing (Spin + PASM)
==============================================================================
  Implements a hardware lock-arbitrated Hub RAM mailbox for synchronous
  Send / Receive / Reply and one-way Pulses across 8 Propeller Cogs.
==============================================================================
}}

CON
  _clkmode = xtal1 + pll16x
  _xinfreq = 5_000_000

  SRR_MAGIC   = $53525231          ' "SRR1"
  SRR_VERSION = 1

  ' Transaction States
  STATE_IDLE      = 0
  STATE_PENDING   = 1
  STATE_DELIVERED = 2
  STATE_REPLIED   = 3
  STATE_TIMEOUT   = 4
  STATE_ERROR     = 5

  ' Message Types
  MSG_REQUEST     = 2
  MSG_REPLY       = 3
  MSG_PULSE       = 5

  ' Buffer Capacities
  MAX_PAYLOAD     = 64
  NUM_COGS        = 8

VAR
  byte  LockNumber
  long  NextTransId

  ' 8-Cog Mailbox Array in Hub RAM
  ' Each Cog has:
  '   long state, trans_id, service_id, op_id, req_len, rep_len
  '   byte req_buf[MAX_PAYLOAD], rep_buf[MAX_PAYLOAD]
  long  CogState[NUM_COGS]
  long  CogTransId[NUM_COGS]
  long  CogServiceId[NUM_COGS]
  long  CogOpId[NUM_COGS]
  long  CogReqLen[NUM_COGS]
  long  CogRepLen[NUM_COGS]
  byte  CogReqBuf[NUM_COGS * MAX_PAYLOAD]
  byte  CogRepBuf[NUM_COGS * MAX_PAYLOAD]

PUB Init(AllocatedLock) : success
{{ Initialize the SRR Hub RAM Mailbox using a hardware lock }}
  LockNumber := AllocatedLock
  NextTransId := 1
  bytefill(@CogState, 0, NUM_COGS * 4)
  bytefill(@CogReqBuf, 0, NUM_COGS * MAX_PAYLOAD)
  bytefill(@CogRepBuf, 0, NUM_COGS * MAX_PAYLOAD)
  return 0

PUB SendRequest(TargetCog, ServiceId, OpId, DataPtr, DataLen, ReplyBuf, ReplyCap, TimeoutTicks) : status | tid, tStart, tExpire
{{ Client Cog: Synchronous Send/Receive/Reply with timeout }}
  if (TargetCog < 0) or (TargetCog >= NUM_COGS) or (DataLen > MAX_PAYLOAD)
    return -1

  ' Acquire hardware lock
  repeat while lockset(LockNumber)

  ' Check if target mailbox is ready
  if CogState[TargetCog] <> STATE_IDLE
    lockclr(LockNumber)
    return -2 ' Target busy

  tid := NextTransId++
  CogTransId[TargetCog] := tid
  CogServiceId[TargetCog] := ServiceId
  CogOpId[TargetCog] := OpId
  CogReqLen[TargetCog] := DataLen

  if DataLen > 0
    bytemove(@CogReqBuf[TargetCog * MAX_PAYLOAD], DataPtr, DataLen)

  CogState[TargetCog] := STATE_PENDING
  lockclr(LockNumber)

  ' Wait for reply or timeout
  tStart := cnt
  tExpire := tStart + TimeoutTicks

  repeat
    repeat while lockset(LockNumber)
    status := CogState[TargetCog]
    if status == STATE_REPLIED
      ' Copy reply data
      if CogRepLen[TargetCog] > 0 and ReplyCap > 0
        bytemove(ReplyBuf, @CogRepBuf[TargetCog * MAX_PAYLOAD], CogRepLen[TargetCog] <# ReplyCap)
      CogState[TargetCog] := STATE_IDLE
      lockclr(LockNumber)
      return 0 ' Success!

    if TimeoutTicks > 0 and (cnt - tStart > TimeoutTicks)
      CogState[TargetCog] := STATE_TIMEOUT
      lockclr(LockNumber)
      return -3 ' Timed out

    lockclr(LockNumber)

PUB Receive(ServerCog, OutSvc, OutOp, OutBuf, OutCap, OutLen) : trans_id | cur_state
{{ Server Cog: Polls its own Hub mailbox for an incoming request }}
  repeat while lockset(LockNumber)

  cur_state := CogState[ServerCog]
  if cur_state == STATE_PENDING
    trans_id := CogTransId[ServerCog]
    long[OutSvc] := CogServiceId[ServerCog]
    long[OutOp] := CogOpId[ServerCog]
    long[OutLen] := CogReqLen[ServerCog]

    if long[OutLen] > 0 and OutCap > 0
      bytemove(OutBuf, @CogReqBuf[ServerCog * MAX_PAYLOAD], long[OutLen] <# OutCap)

    CogState[ServerCog] := STATE_DELIVERED
    lockclr(LockNumber)
    return trans_id

  lockclr(LockNumber)
  return 0 ' No request pending

PUB Reply(ServerCog, ReplyDataPtr, ReplyLen) : ok
{{ Server Cog: Replies to the caller with synchronous data payload }}
  repeat while lockset(LockNumber)

  if CogState[ServerCog] == STATE_DELIVERED
    CogRepLen[ServerCog] := ReplyLen <# MAX_PAYLOAD
    if ReplyLen > 0
      bytemove(@CogRepBuf[ServerCog * MAX_PAYLOAD], ReplyDataPtr, CogRepLen[ServerCog])
    CogState[ServerCog] := STATE_REPLIED
    lockclr(LockNumber)
    return 0

  lockclr(LockNumber)
  return -1 ' Client already timed out or cancelled

PUB SendPulse(TargetCog, Code, Value) : ok
{{ Asynchronous one-way notification pulse without blocking }}
  repeat while lockset(LockNumber)
  CogServiceId[TargetCog] := Code
  CogOpId[TargetCog] := Value
  lockclr(LockNumber)
  return 0
