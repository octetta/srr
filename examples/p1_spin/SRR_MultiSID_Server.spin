{{
==============================================================================
  SRR_MultiSID_Server.spin
  Parallax Propeller P1 Multi-SID (Up to 6 SIDs / 18 Voices) Server
==============================================================================
  Harnesses all 8 Cogs of the Parallax Propeller P1:
    - Cog 0: SRR Wire Framing Engine & Multi-SID Resource Router
    - Cog 1: FullDuplexSerial (PASM high-speed UART @ 115,200 - 230,400 baud)
    - Cog 2: SID #0 (Left Stereo Master  - Pins P10/P11, Voices 1..3)
    - Cog 3: SID #1 (Right Stereo Master - Pins P12/P13, Voices 4..6)
    - Cog 4: SID #2 (Voices 7..9   - Pins P14/P15)
    - Cog 5: SID #3 (Voices 10..12 - Pins P16/P17)
    - Cog 6: SID #4 (Voices 13..15 - Pins P18/P19)
    - Cog 7: SID #5 (Voices 16..18 - Pins P20/P21)

  Hub RAM Footprint: 6 SIDs x 32 bytes = 192 bytes total!
==============================================================================
}}

CON
  _clkmode = xtal1 + pll16x
  _xinfreq = 5_000_000

  UART_RX_PIN    = 31
  UART_TX_PIN    = 30
  UART_BAUD      = 115_200

  MAX_CHIPS      = 6
  REGS_PER_CHIP  = 32

  ' SRR Protocol
  SRR_MAGIC      = $53525231          ' "SRR1"
  SRR_VERSION    = 1
  SRR_MSG_REQ    = 2
  SRR_MSG_REPLY  = 3
  SRR_MSG_ERROR  = 4
  SRR_MSG_PULSE  = 5

  ' Service IDs: 0x0050..0x0055 for Chips 0..5
  SVC_SID_BASE   = $0050

  ' Operations
  OP_WRITE_REG         = $0001
  OP_WRITE_BATCH       = $0002
  OP_WRITE_DUMP        = $0003
  OP_RESET             = $0004
  OP_READ_REGS         = $0005

  OP_MULTI_WRITE_REG   = $0010
  OP_MULTI_WRITE_BATCH = $0011
  OP_RESET_ALL         = $0012
  OP_STEREO_PAN_NOTE   = $0013

  MAX_FRAME_SIZE = 128

OBJ
  uart : "FullDuplexSerial"
  sid0 : "SIDcog"
  sid1 : "SIDcog"
  sid2 : "SIDcog"
  sid3 : "SIDcog"
  sid4 : "SIDcog"
  sid5 : "SIDcog"

VAR
  ' 192 bytes Hub RAM array holding all 6 SID register windows
  byte  SidRegisters[MAX_CHIPS * REGS_PER_CHIP]
  byte  RxBuffer[MAX_FRAME_SIZE]
  byte  DecBuffer[MAX_FRAME_SIZE]
  byte  TxBuffer[MAX_FRAME_SIZE]
  byte  TxRaw[MAX_FRAME_SIZE]

PUB Main | rx_len, b
{{ Main entry point: launches up to 6 PASM SID synth cogs and enters SRR loop }}
  ' 1. Start PASM UART
  uart.start(UART_RX_PIN, UART_TX_PIN, 0, UART_BAUD)

  ' 2. Launch 6 independent SID synthesizers across Cogs 2..7
  sid0.start(10, 11, @SidRegisters[0 * REGS_PER_CHIP]) ' Cog 2: SID 0 (P10/P11)
  sid1.start(12, 13, @SidRegisters[1 * REGS_PER_CHIP]) ' Cog 3: SID 1 (P12/P13)
  sid2.start(14, 15, @SidRegisters[2 * REGS_PER_CHIP]) ' Cog 4: SID 2 (P14/P15)
  sid3.start(16, 17, @SidRegisters[3 * REGS_PER_CHIP]) ' Cog 5: SID 3 (P16/P17)
  sid4.start(18, 19, @SidRegisters[4 * REGS_PER_CHIP]) ' Cog 6: SID 4 (P18/P19)
  sid5.start(20, 21, @SidRegisters[5 * REGS_PER_CHIP]) ' Cog 7: SID 5 (P20/P21)

  ResetAllChips

  rx_len := 0

  ' 3. Frame Processing Loop in Cog 0
  repeat
    b := uart.rxcheck
    if b <> -1
      if b == 0
        if rx_len > 0
          ProcessFrame(rx_len)
          rx_len := 0
      else
        if rx_len < (MAX_FRAME_SIZE - 1)
          RxBuffer[rx_len++] := b
        else
          rx_len := 0

PRI ResetAllChips | c
  repeat c from 0 to MAX_CHIPS - 1
    bytefill(@SidRegisters[c * REGS_PER_CHIP], 0, REGS_PER_CHIP)
    SidRegisters[c * REGS_PER_CHIP + $18] := $0F ' Max volume

PRI ProcessFrame(cobs_len) | dec_len, crc_calc, crc_pkt, msg_type, svc, op, tid_lo, tid_hi, plen, p_payload, chip
  dec_len := DecodeCOBS(@RxBuffer, cobs_len, @DecBuffer)
  if dec_len < 34
    return

  crc_calc := ComputeCRC16(@DecBuffer, dec_len - 2, $FFFF)
  crc_pkt  := DecBuffer[dec_len - 2] | (DecBuffer[dec_len - 1] << 8)
  if crc_calc <> crc_pkt
    return

  if long[@DecBuffer[0]] <> SRR_MAGIC or word[@DecBuffer[4]] <> SRR_VERSION
    return

  msg_type := word[@DecBuffer[6]]
  svc      := word[@DecBuffer[10]]
  op       := word[@DecBuffer[12]]
  tid_lo   := long[@DecBuffer[20]]
  tid_hi   := long[@DecBuffer[24]]
  plen     := long[@DecBuffer[28]]
  p_payload:= @DecBuffer[32]

  ' Route by Service ID: $0050..$0055
  if (svc >= SVC_SID_BASE) and (svc < (SVC_SID_BASE + MAX_CHIPS))
    chip := svc - SVC_SID_BASE
    if msg_type == SRR_MSG_REQ
      HandleSingleChipRequest(chip, op, tid_lo, tid_hi, p_payload, plen)
    elseif msg_type == SRR_MSG_PULSE
      HandlePulse(chip, op, p_payload, plen)
  elseif svc == SVC_SID_BASE or svc == $0051
    ' Multi-chip collective commands
    if msg_type == SRR_MSG_REQ
      HandleMultiChipRequest(op, tid_lo, tid_hi, p_payload, plen)

PRI HandleSingleChipRequest(chip, op, tid_lo, tid_hi, payload, plen) | reg, val, i, resp_byte, base
  base := chip * REGS_PER_CHIP
  case op
    OP_WRITE_REG:
      if plen >= 2
        reg := byte[payload][0]
        val := byte[payload][1]
        if reg < 32
          SidRegisters[base + reg] := val
      resp_byte := 0
      SendReply(SVC_SID_BASE + chip, op, tid_lo, tid_hi, @resp_byte, 1)

    OP_WRITE_BATCH:
      i := 0
      repeat while (i + 1) < plen
        reg := byte[payload][i]
        val := byte[payload][i + 1]
        if reg < 32
          SidRegisters[base + reg] := val
        i += 2
      resp_byte := (i / 2)
      SendReply(SVC_SID_BASE + chip, op, tid_lo, tid_hi, @resp_byte, 1)

    OP_WRITE_DUMP:
      bytemove(@SidRegisters[base], payload, plen <# 25)
      resp_byte := 0
      SendReply(SVC_SID_BASE + chip, op, tid_lo, tid_hi, @resp_byte, 1)

    OP_RESET:
      bytefill(@SidRegisters[base], 0, 32)
      SidRegisters[base + $18] := $0F
      resp_byte := 0
      SendReply(SVC_SID_BASE + chip, op, tid_lo, tid_hi, @resp_byte, 1)

    OP_READ_REGS:
      SendReply(SVC_SID_BASE + chip, op, tid_lo, tid_hi, @SidRegisters[base], 25)

PRI HandleMultiChipRequest(op, tid_lo, tid_hi, payload, plen) | chip, reg, val, i, resp_byte
  case op
    OP_MULTI_WRITE_REG:
      if plen >= 3
        chip := byte[payload][0]
        reg  := byte[payload][1]
        val  := byte[payload][2]
        if chip < MAX_CHIPS and reg < 32
          SidRegisters[chip * REGS_PER_CHIP + reg] := val
      resp_byte := 0
      SendReply(SVC_SID_BASE, op, tid_lo, tid_hi, @resp_byte, 1)

    OP_MULTI_WRITE_BATCH:
      i := 0
      repeat while (i + 2) < plen
        chip := byte[payload][i]
        reg  := byte[payload][i + 1]
        val  := byte[payload][i + 2]
        if chip < MAX_CHIPS and reg < 32
          SidRegisters[chip * REGS_PER_CHIP + reg] := val
        i += 3
      resp_byte := (i / 3)
      SendReply(SVC_SID_BASE, op, tid_lo, tid_hi, @resp_byte, 1)

    OP_RESET_ALL:
      ResetAllChips
      resp_byte := 0
      SendReply(SVC_SID_BASE, op, tid_lo, tid_hi, @resp_byte, 1)

PRI HandlePulse(chip, op, payload, plen) | reg, val, base
  base := chip * REGS_PER_CHIP
  if plen >= 2
    reg := byte[payload][0]
    val := byte[payload][1]
    if reg < 32
      SidRegisters[base + reg] := val
  elseif op < 32 and plen >= 1
    SidRegisters[base + op] := byte[payload][0]

PRI SendReply(svc, op, tid_lo, tid_hi, payload, plen) | raw_len, cobs_len, crc, i
  long[@TxRaw[0]]  := SRR_MAGIC
  word[@TxRaw[4]]  := SRR_VERSION
  word[@TxRaw[6]]  := SRR_MSG_REPLY
  word[@TxRaw[8]]  := 0
  word[@TxRaw[10]] := svc
  word[@TxRaw[12]] := op
  word[@TxRaw[14]] := 0
  long[@TxRaw[16]] := 0
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
  TxBuffer[cobs_len++] := 0

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
