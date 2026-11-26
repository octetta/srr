{{
==============================================================================
  FullDuplexSerial.spin
  Parallax Propeller P1 High-Speed Full-Duplex Serial Driver (Spin + PASM)
==============================================================================
  Runs a dedicated assembly Cog for true hardware-like UART transmission
  and reception up to 230,400+ baud at 80 MHz, using Hub RAM circular buffers.
==============================================================================
}}

CON
  BUFFER_SIZE = 64
  BUFFER_MASK = BUFFER_SIZE - 1

VAR
  long  cog                     ' Cog ID running the PASM driver
  long  rx_head                 ' 4 contiguous longs passed to PASM
  long  rx_tail
  long  tx_head
  long  tx_tail
  long  rx_pin
  long  tx_pin
  long  rxtx_mode
  long  bit_ticks
  long  buffer_ptr              ' Pointer to rx/tx circular buffers
  byte  rx_buffer[BUFFER_SIZE]
  byte  tx_buffer[BUFFER_SIZE]

PUB start(rxpin, txpin, mode, baudrate) : ok
{{ Start serial driver in a new Cog; returns false if no cog available }}
  stop
  rx_pin    := rxpin
  tx_pin    := txpin
  rxtx_mode := mode
  bit_ticks := clkfreq / baudrate
  buffer_ptr:= @rx_buffer

  rx_head := 0
  rx_tail := 0
  tx_head := 0
  tx_tail := 0

  cog := cognew(@entry, @rx_head) + 1
  return cog > 0

PUB stop
{{ Stop serial driver Cog and release hardware resources }}
  if cog
    cogstop(cog - 1)
    cog := 0

PUB rxcheck : rxbyte
{{ Non-blocking check for received byte; returns -1 if buffer empty }}
  rxbyte := -1
  if rx_tail <> rx_head
    rxbyte := rx_buffer[rx_tail]
    rx_tail := (rx_tail + 1) & BUFFER_MASK

PUB rxtime(ms) : rxbyte | wait_ticks, start_cnt
{{ Wait up to ms milliseconds for received byte; returns -1 on timeout }}
  wait_ticks := (clkfreq / 1000) * ms
  start_cnt := cnt
  repeat
    rxbyte := rxcheck
    if rxbyte <> -1
      return rxbyte
  while (cnt - start_cnt) < wait_ticks
  return -1

PUB rx : rxbyte
{{ Blocking receive byte }}
  repeat while (rxbyte := rxcheck) < 0

PUB tx(txbyte)
{{ Transmit byte (buffers and queues for PASM Cog) }}
  repeat while ((tx_head + 1) & BUFFER_MASK) == tx_tail
  tx_buffer[tx_head] := txbyte
  tx_head := (tx_head + 1) & BUFFER_MASK

PUB str(stringptr)
{{ Send null-terminated string }}
  repeat strsize(stringptr)
    tx(byte[stringptr++])

DAT
' ============================================================================
' PASM High-Speed UART Engine
' ============================================================================
        org     0
entry
        mov     t1, par                 ' Hub parameter address (@rx_head)
        mov     p_rxh, t1
        add     t1, #4
        mov     p_rxt, t1
        add     t1, #4
        mov     p_txh, t1
        add     t1, #4
        mov     p_txt, t1
        add     t1, #4
        rdlong  r_pin, t1               ' rx_pin
        add     t1, #4
        rdlong  t_pin, t1               ' tx_pin
        add     t1, #4
        rdlong  m_mode, t1              ' mode
        add     t1, #4
        rdlong  b_ticks, t1             ' bit_ticks
        add     t1, #4
        rdlong  p_bufs, t1              ' buffer_ptr (@rx_buffer)

        ' Setup Pin Masks and Directions
        mov     r_mask, #1
        shl     r_mask, r_pin
        mov     t_mask, #1
        shl     t_mask, t_pin

        or      outa, t_mask            ' TX line idle HIGH
        or      dira, t_mask            ' Make TX an output
        andn    dira, r_mask            ' Make RX an input

        mov     rx_cnt, #0
        mov     tx_cnt, #0

' ----------------------------------------------------------------------------
' Main Polling Loop
' ----------------------------------------------------------------------------
main_loop
        ' Poll RX line
        test    r_mask, ina     wz
        if_z    call    #receive_byte   ' Start bit detected (low)

        ' Poll TX buffer
        rdlong  t1, p_txh
        rdlong  t2, p_txt
        cmp     t1, t2          wz
        if_nz   call    #transmit_byte  ' Byte waiting to transmit

        jmp     #main_loop

' ----------------------------------------------------------------------------
' Receive Byte (Start bit already detected)
' ----------------------------------------------------------------------------
receive_byte
        mov     rx_time, cnt
        add     rx_time, b_ticks
        shr     b_ticks, #1
        add     rx_time, b_ticks        ' Center of bit 0
        shl     b_ticks, #1

        mov     rx_data, #0
        mov     rx_bits, #8

rx_bit_loop
        waitcnt rx_time, b_ticks
        test    r_mask, ina     wc
        rcr     rx_data, #1
        djnz    rx_bits, #rx_bit_loop

        ' Wait for stop bit
        waitcnt rx_time, b_ticks
        shr     rx_data, #24            ' Right-align received byte

        ' Store in RX buffer
        rdlong  t1, p_rxh
        mov     t2, t1
        add     t2, p_bufs
        wrbyte  rx_data, t2
        add     t1, #1
        and     t1, #BUFFER_MASK
        wrlong  t1, p_rxh

receive_byte_ret
        ret

' ----------------------------------------------------------------------------
' Transmit Byte
' ----------------------------------------------------------------------------
transmit_byte
        rdlong  t2, p_txt
        mov     t3, t2
        add     t3, p_bufs
        add     t3, #BUFFER_SIZE        ' tx_buffer is offset by BUFFER_SIZE
        rdbyte  tx_data, t3

        ' Advance tx_tail
        add     t2, #1
        and     t2, #BUFFER_MASK
        wrlong  t2, p_txt

        ' Frame: Start bit (0) + 8 data bits + Stop bit (1)
        shl     tx_data, #1             ' Prepend start bit 0
        or      tx_data, #$200          ' Append stop bit 1
        mov     tx_bits, #10
        mov     tx_time, cnt
        add     tx_time, b_ticks

tx_bit_loop
        test    tx_data, #1     wc
        muxc    outa, t_mask
        shr     tx_data, #1
        waitcnt tx_time, b_ticks
        djnz    tx_bits, #tx_bit_loop

transmit_byte_ret
        ret

' Variables
p_rxh   long    0
p_rxt   long    0
p_txh   long    0
p_txt   long    0
r_pin   long    0
t_pin   long    0
m_mode  long    0
b_ticks long    0
p_bufs  long    0
r_mask  long    0
t_mask  long    0

rx_time long    0
rx_data long    0
rx_bits long    0
rx_cnt  long    0

tx_time long    0
tx_data long    0
tx_bits long    0
tx_cnt  long    0

t1      long    0
t2      long    0
t3      long    0

        fit     496
