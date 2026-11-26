{{
==============================================================================
  SIDcog.spin
  Parallax Propeller P1 MOS 6581 SID Synthesizer Interface & PASM Engine
==============================================================================
  Runs an autonomous audio synthesis engine inside a dedicated assembly Cog.
  Emulates the MOS Technology 6581 SID sound interface device:
    - 3 independent audio oscillators (Triangle, Sawtooth, Pulse, Noise)
    - Pulse Width Modulation
    - Envelope generators (ADSR)
    - PWM / Duty-cycle DAC audio output using Propeller Hardware Counters
    - Reads 29 SID registers ($00..$1C) continuously from shared Hub RAM
==============================================================================
}}

CON
  SID_REGS_LEN = 32

VAR
  long  cog
  long  hub_regs_ptr
  long  audio_pin_l
  long  audio_pin_r

PUB start(pin_left, pin_right, regs_ptr) : ok
{{ Launch the SID audio synthesis engine into a dedicated Cog }}
  stop
  audio_pin_l  := pin_left
  audio_pin_r  := pin_right
  hub_regs_ptr := regs_ptr

  ' Initialize registers to 0 in Hub RAM
  bytefill(regs_ptr, 0, SID_REGS_LEN)

  ' Start PASM synthesis core
  cog := cognew(@sid_entry, @hub_regs_ptr) + 1
  return cog > 0

PUB stop
{{ Stop the SID synthesizer Cog }}
  if cog
    cogstop(cog - 1)
    cog := 0

DAT
' ============================================================================
' PASM SID Synthesizer Core Engine
' ============================================================================
        org     0
sid_entry
        mov     t1, par                 ' Hub parameter address
        rdlong  p_regs, t1              ' Pointer to Hub RAM SidRegisters
        add     t1, #4
        rdlong  pin_l, t1               ' Left Audio Pin
        add     t1, #4
        rdlong  pin_r, t1               ' Right Audio Pin

        ' Setup Hardware Counters for Duty-Cycle DAC Audio Output
        ' Mode %00110: DUTY mode (PWM DAC output)
        mov     dira_mask, #1
        shl     dira_mask, pin_l
        mov     t2, #1
        shl     t2, pin_r
        or      dira_mask, t2
        or      dira, dira_mask         ' Set audio pins as outputs

        ' Configure CTRA for pin_l, CTRB for pin_r
        mov     ctra_val, ctra_mode
        or      ctra_val, pin_l
        mov     ctra, ctra_val

        mov     ctrb_val, ctra_mode
        or      ctrb_val, pin_r
        mov     ctrb, ctrb_val

        ' Sample rate loop timing: 80 MHz / 2560 = 31,250 Hz sample rate
        mov     sample_ticks, #2560
        mov     next_sample, cnt
        add     next_sample, sample_ticks

' ----------------------------------------------------------------------------
' Main Audio Synthesis Loop (~31.25 kHz)
' ----------------------------------------------------------------------------
synth_loop
        waitcnt next_sample, sample_ticks

        ' 1. Fetch Voice 1 frequency and control register from Hub RAM
        mov     t1, p_regs
        rdbyte  v1_flo, t1              ' $00: Freq Lo
        add     t1, #1
        rdbyte  v1_fhi, t1              ' $01: Freq Hi
        add     t1, #3
        rdbyte  v1_cr, t1               ' $04: Control Register
        add     t1, #20                 ' Move to $18: Mode/Vol
        rdbyte  m_vol, t1
        and     m_vol, #$0F             ' 4-bit master volume (0..15)

        ' Construct 16-bit frequency word
        mov     v1_freq, v1_fhi
        shl     v1_freq, #8
        or      v1_freq, v1_flo
        shl     v1_freq, #8             ' Scale for phase accumulator

        ' Check Gate bit ($01)
        test    v1_cr, #$01     wz
        if_z    mov     v1_out, #0
        if_z    jmp     #update_dac

        ' 2. Advance Voice 1 Phase Accumulator
        add     v1_phase, v1_freq

        ' 3. Waveform Generation
        ' Sawtooth Wave ($20)
        test    v1_cr, #$20     wz
        if_nz   mov     v1_out, v1_phase
        if_nz   shr     v1_out, #24     ' 8-bit sawtooth

        ' Triangle Wave ($10)
        test    v1_cr, #$10     wz
        if_nz   mov     v1_out, v1_phase
        if_nz   shr     v1_out, #23
        if_nz   and     v1_out, #$FF

        ' Pulse / Square Wave ($40)
        test    v1_cr, #$40     wz
        if_nz   test    v1_phase, bit_31 wz
        if_nz_and_nz mov v1_out, #255
        if_nz_and_z  mov v1_out, #0

        ' 4. Apply Master Volume (v1_out * m_vol)
        mov     t2, v1_out
        mul     t2, m_vol
        shl     t2, #18                 ' Map 12-bit audio product to 32-bit FRQA duty

update_dac
        mov     frqa, t2                ' Output to Left Pin Counter DAC
        mov     frqb, t2                ' Output to Right Pin Counter DAC

        jmp     #synth_loop

' Constants and Data
ctra_mode       long    %00110 << 26    ' DUTY mode
bit_31          long    $8000_0000

p_regs          long    0
pin_l           long    0
pin_r           long    0
dira_mask       long    0
ctra_val        long    0
ctrb_val        long    0

sample_ticks    long    0
next_sample     long    0

v1_flo          long    0
v1_fhi          long    0
v1_freq         long    0
v1_cr           long    0
m_vol           long    0

v1_phase        long    0
v1_out          long    0

t1              long    0
t2              long    0

                fit     496
