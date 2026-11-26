{{
==============================================================================
  SRR_SID_Server.spin
  Parallax Propeller P1 SRR SID Sound Engine Server (Spin + PASM)
==============================================================================
  - Cog 0: SRR Wire Framing Engine & SID Resource Manager
  - Cog 1: FullDuplexSerial (PASM high-speed UART @ 115,200 baud)
  - Cog 2: SIDcog (PASM MOS 6581 sound synthesizer on audio pins P10/P11)
==============================================================================
}}

CON
  _clkmode = xtal1 + pll16x
  _xinfreq = 5_000_000

  ' Pin Configuration
  UART_RX_PIN    = 31
  UART_TX_PIN    = 30
  UART_BAUD      = 115_200
  AUDIO_PIN_L    = 10
  AUDIO_PIN_R    = 11

  ' SRR Protocol Constants
  SRR_MAGIC      = $53525231          ' "SRR1"
  SRR_VERSION    = 1
  SRR_MSG_REQ    = 2
  SRR_MSG_REPLY  = 3
  SRR_MSG_ERROR  = 4
  SRR_MSG_PULSE  = 5

  ' SID Service Definitions
  SVC_SID        = $0051              ' "SID"
  OP_WRITE_REG   = $0001              ' Write single register: [reg, val]
  OP_WRITE_BATCH = $0002              ' Write batch: [ {reg, val}... ]
  OP_WRITE_DUMP  = $0003              ' Write full 25-byte register block
  OP_RESET       = $0004              ' Silence and reset all voices
  OP_READ_REGS   = $0005              ' Read 25 registers

  MAX_FRAME_SIZE = 96

OBJ
  uart : "FullDuplexSerial"
  sid  : "SIDcog"

VAR
  byte  SidRegisters[32]              ' Shared Hub RAM SID Register Bank ($00..$1F)
  byte  RxBuffer[MAX_FRAME_SIZE]
  byte  DecBuffer[MAX_FRAME_SIZE]
  byte  TxBuffer[MAX_FRAME_SIZE]
  byte  TxRaw[MAX_FRAME_SIZE]

PUB Main | rx_len, b
{{ Main entry point: boots PASM subsystems and enters SRR frame loop }}
  ' 1. Start PASM Full-Duplex UART
  uart.start(UART_RX_PIN, UART_TX_PIN, 0, UART_BAUD)

  ' 2. Start PASM SID Synthesizer Core on Audio Pins P10 and P11
  sid.start(AUDIO_PIN_L, AUDIO_PIN_R, @SidRegisters)

  ' Set default master volume to max (reg $18 = $0F)
  SidRegisters[$18] := $0F

  rx_len := 0

  ' 3. Frame Processing Loop
  repeat
    b := uart.rxcheck
    if b <> -1
      if b == 0
        ' Zero delimiter: complete COBS frame received
        if rx_len > 0
          ProcessFrame(rx_len)
          rx_len := 0
      else
        if rx_len < (MAX_FRAME_SIZE - 1)
          RxBuffer[rx_len++] := b
        else
          rx_len := 0 ' Overflow, drop frame

PRI ProcessFrame(cobs_len) | dec_len, crc_calc, crc_pkt, msg_type, svc, op, tid_lo, tid_hi, plen, p_payload
{{ Decodes COBS frame, verifies CRC-16, and dispatches to SID engine }}
  dec_len := DecodeCOBS(@RxBuffer, cobs_len, @DecBuffer)
  if dec_len < 34 ' 32-byte header + 2-byte CRC
    return

  ' Verify CRC-16
  crc_calc := ComputeCRC16(@DecBuffer, dec_len - 2, $FFFF)
  crc_pkt  := DecBuffer[dec_len - 2] | (DecBuffer[dec_len - 1] << 8)
  if crc_calc <> crc_pkt
    return ' CRC error, drop

  ' Verify SRR Magic & Version
  if long[@DecBuffer[0]] <> SRR_MAGIC or word[@DecBuffer[4]] <> SRR_VERSION
    return

  msg_type := word[@DecBuffer[6]]
  svc      := word[@DecBuffer[10]]
  op       := word[@DecBuffer[12]]
  tid_lo   := long[@DecBuffer[20]]
  tid_hi   := long[@DecBuffer[24]]
  plen     := long[@DecBuffer[28]]
  p_payload:= @DecBuffer[32]

  ' Dispatch SID Service
  if svc == SVC_SID
    if msg_type == SRR_MSG_REQ
      HandleRequest(op, tid_lo, tid_hi, p_payload, plen)
    elseif msg_type == SRR_MSG_PULSE
      HandlePulse(op, p_payload, plen)

PRI HandleRequest(op, tid_lo, tid_hi, payload, plen) | reg, val, i, resp_byte
{{ Handles synchronous request: updates registers and transmits SRR reply }}
  case op
    OP_WRITE_REG:
      if plen >= 2
        reg := byte[payload][0]
        val := byte[payload][1]
        if reg < 32
          SidRegisters[reg] := val
      resp_byte := 0
      SendReply(SVC_SID, op, tid_lo, tid_hi, @resp_byte, 1)

    OP_WRITE_BATCH:
      i := 0
      repeat while (i + 1) < plen
        reg := byte[payload][i]
        val := byte[payload][i + 1]
        if reg < 32
          SidRegisters[reg] := val
        i += 2
      resp_byte := (i / 2)
      SendReply(SVC_SID, op, tid_lo, tid_hi, @resp_byte, 1)

    OP_WRITE_DUMP:
      bytemove(@SidRegisters, payload, plen <# 25)
      resp_byte := 0
      SendReply(SVC_SID, op, tid_lo, tid_hi, @resp_byte, 1)

    OP_RESET:
      bytefill(@SidRegisters, 0, 32)
      SidRegisters[$18] := $0F ' Reset volume to max
      resp_byte := 0
      SendReply(SVC_SID, op, tid_lo, tid_hi, @resp_byte, 1)

    OP_READ_REGS:
      SendReply(SVC_SID, op, tid_lo, tid_hi, @SidRegisters, 25)

PRI HandlePulse(op, payload, plen) | reg, val
{{ Handles asynchronous one-way pulse: zero reply emitted }}
  if plen >= 2
    reg := byte[payload][0]
    val := byte[payload][1]
    if reg < 32
      SidRegisters[reg] := val
  elseif op < 32 and plen >= 1
    SidRegisters[op] := byte[payload][0]

PRI SendReply(svc, op, tid_lo, tid_hi, payload, plen) | raw_len, cobs_len, crc, i
{{ Encodes SRR Reply Header + Payload + CRC-16 into COBS and sends via UART }}
  long[@TxRaw[0]]  := SRR_MAGIC
  word[@TxRaw[4]]  := SRR_VERSION
  word[@TxRaw[6]]  := SRR_MSG_REPLY
  word[@TxRaw[8]]  := 0 ' Flags
  word[@TxRaw[10]] := svc
  word[@TxRaw[12]] := op
  word[@TxRaw[14]] := 0 ' Reserved
  long[@TxRaw[16]] := 0 ' Session ID
  long[@TxRaw[20]] := tid_lo
  long[@TxRaw[24]] := tid_hi
  long[@TxRaw[28]] := plen

  if plen > 0
    bytemove(@TxRaw[32], payload, plen)

  raw_len := 32 + plen
  crc := ComputeCRC16(@TxRaw, raw_len, $FFFF)
  TxRaw[raw_len++] := crc & $FF
  TxRaw[raw_len++] := (crc >> 8) & $FF

  cobs_len := EncodeCOBS(@TxRaw, raw_len, @TxBuffer)
  TxBuffer[cobs_len++] := 0 ' Trailing delimiter

  repeat i from 0 to cobs_len - 1
    uart.tx(TxBuffer[i])

PRI DecodeCOBS(src, src_len, dst) : dst_len | src_idx, code, i
  src_idx := 0
  dst_len := 0
  repeat while src_idx < src_len
    code := byte[src][src_idx++]
    if code == 0
      return 0
    repeat i from 1 to code - 1
      if src_idx >= src_len
        return dst_len
      byte[dst][dst_len++] := byte[src][src_idx++]
    if code < $FF and src_idx < src_len
      byte[dst][dst_len++] := 0

PRI EncodeCOBS(src, src_len, dst) : dst_len | code_idx, code, i, b
  code_idx := 0
  dst_len := 1
  code := 1
  repeat i from 0 to src_len - 1
    b := byte[src][i]
    if b == 0
      byte[dst][code_idx] := code
      code_idx := dst_len++
      code := 1
    else
      byte[dst][dst_len++] := b
      code++
      if code == $FF
        byte[dst][code_idx] := code
        code_idx := dst_len++
        code := 1
  byte[dst][code_idx] := code

PRI ComputeCRC16(data_ptr, len, crc) : result | i, b, j
  result := crc
  repeat i from 0 to len - 1
    b := byte[data_ptr][i]
    result ^= (b << 8)
    repeat j from 0 to 7
      if (result & $8000)
        result := ((result << 1) ^ $1021) & $FFFF
      else
        result := (result << 1) & $FFFF
