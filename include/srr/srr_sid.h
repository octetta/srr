#ifndef SRR_SID_H
#define SRR_SID_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * SRR Parallax Propeller P1 SID Service Protocol (MOS 6581 / 8580 SIDcog)
 * ============================================================================
 * Exposes a hardware MOS 6581 SID sound synthesizer running in a dedicated
 * Propeller Cog (via Ahlebrand's SIDcog or emulator) as a standard SRR resource.
 *
 * Wire Service ID: 0x0051 ("SID")
 */

#define SRR_SVC_SID             0x0051U

/* Operations */
#define SRR_SID_OP_WRITE_REG    0x0001U /* Write 1 register: [reg(1B), val(1B)] -> Reply [status(1B)] */
#define SRR_SID_OP_WRITE_BATCH  0x0002U /* Write batch: [ {reg, val}... ] -> Reply [written_count(1B)] */
#define SRR_SID_OP_WRITE_DUMP   0x0003U /* Write full 25-byte register block ($00..$18) -> Reply [status] */
#define SRR_SID_OP_RESET        0x0004U /* Silence all voices and clear registers -> Reply [status] */
#define SRR_SID_OP_READ_REGS    0x0005U /* Read back current 25 registers -> Reply [25 bytes] */
#define SRR_SID_OP_NOTE_ON      0x0006U /* Note on: [voice(1B), note(1B), wave(1B), ad(1B), sr(1B)] */
#define SRR_SID_OP_NOTE_OFF     0x0007U /* Note off: [voice(1B)] */

/* Standard SID Registers (MOS 6581 / 8580) */
#define SRR_SID_NUM_REGS        25U
#define SRR_SID_TOTAL_REGS      32U

/* Voice 1 */
#define SRR_SID_V1_FREQ_LO      0x00U
#define SRR_SID_V1_FREQ_HI      0x01U
#define SRR_SID_V1_PW_LO        0x02U
#define SRR_SID_V1_PW_HI        0x03U
#define SRR_SID_V1_CR           0x04U
#define SRR_SID_V1_AD           0x05U
#define SRR_SID_V1_SR           0x06U

/* Voice 2 */
#define SRR_SID_V2_FREQ_LO      0x07U
#define SRR_SID_V2_FREQ_HI      0x08U
#define SRR_SID_V2_PW_LO        0x09U
#define SRR_SID_V2_PW_HI        0x0AU
#define SRR_SID_V2_CR           0x0BU
#define SRR_SID_V2_AD           0x0CU
#define SRR_SID_V2_SR           0x0DU

/* Voice 3 */
#define SRR_SID_V3_FREQ_LO      0x0EU
#define SRR_SID_V3_FREQ_HI      0x0FU
#define SRR_SID_V3_PW_LO        0x10U
#define SRR_SID_V3_PW_HI        0x11U
#define SRR_SID_V3_CR           0x12U
#define SRR_SID_V3_AD           0x13U
#define SRR_SID_V3_SR           0x14U

/* Filter and Volume */
#define SRR_SID_FC_LO           0x15U
#define SRR_SID_FC_HI           0x16U
#define SRR_SID_RES_FILT        0x17U
#define SRR_SID_MODE_VOL        0x18U

/* Control Register (CR) Bits */
#define SRR_SID_CR_GATE         0x01U
#define SRR_SID_CR_SYNC         0x02U
#define SRR_SID_CR_RING         0x04U
#define SRR_SID_CR_TEST         0x08U
#define SRR_SID_CR_TRI          0x10U
#define SRR_SID_CR_SAW          0x20U
#define SRR_SID_CR_PULSE        0x40U
#define SRR_SID_CR_NOISE        0x80U

/* Filter Mode Bits */
#define SRR_SID_FILT_LP         0x10U
#define SRR_SID_FILT_BP         0x20U
#define SRR_SID_FILT_HP         0x40U
#define SRR_SID_FILT_3OFF       0x80U
#define SRR_SID_VOL_MASK        0x0FU

/* Register write pair for batch operations */
#pragma pack(push, 1)
typedef struct {
    uint8_t reg;
    uint8_t val;
} srr_sid_reg_write_t;

typedef struct {
    uint8_t voice;  /* 0, 1, or 2 */
    uint8_t note;   /* MIDI note number: 0..127 */
    uint8_t wave;   /* Waveform bits: SRR_SID_CR_TRI, SAW, PULSE, NOISE */
    uint8_t ad;     /* Attack (4 bits) / Decay (4 bits) */
    uint8_t sr;     /* Sustain (4 bits) / Release (4 bits) */
} srr_sid_note_on_t;
#pragma pack(pop)

/* Helper: Calculate 16-bit SID frequency word from frequency in Hertz (PAL clock: 985248 Hz) */
static inline uint16_t srr_sid_freq_hz(float hz) {
    if (hz <= 0.0f) return 0;
    /* Formula: (Hz * 16777216) / 985248 */
    double word = ((double)hz * 16777216.0) / 985248.0;
    if (word > 65535.0) word = 65535.0;
    return (uint16_t)word;
}

/* Helper: Calculate 16-bit SID frequency word from MIDI note number (A4 = note 69 = 440 Hz) */
static inline uint16_t srr_sid_freq_midi(uint8_t note) {
    /* Lookup table for MIDI notes 24 (C1 = 32.7 Hz) to 96 (C7 = 2093.0 Hz) */
    static const uint16_t midi_sid_freqs[128] = {
        /* Notes 0 - 11 */
        140, 148, 157, 166, 176, 187, 198, 210, 222, 236, 250, 265,
        /* Notes 12 - 23 (Octave 0) */
        281, 297, 315, 334, 354, 375, 397, 421, 446, 472, 500, 530,
        /* Notes 24 - 35 (Octave 1: C1 = 268) */
        561, 595, 630, 668, 707, 749, 794, 841, 891, 944, 1000, 1060,
        /* Notes 36 - 47 (Octave 2: C2) */
        1123, 1190, 1261, 1336, 1415, 1499, 1589, 1683, 1783, 1889, 2002, 2121,
        /* Notes 48 - 59 (Octave 3: C3) */
        2247, 2380, 2522, 2672, 2831, 2999, 3178, 3367, 3567, 3779, 4004, 4242,
        /* Notes 60 - 71 (Octave 4: Middle C=60, A4=69=7082) */
        4494, 4761, 5045, 5345, 5663, 5999, 6356, 6734, 7134, 7558, 8008, 8484,
        /* Notes 72 - 83 (Octave 5: C5) */
        8989, 9523, 10089, 10689, 11325, 11998, 12712, 13468, 14269, 15117, 16016, 16969,
        /* Notes 84 - 95 (Octave 6: C6) */
        17977, 19046, 20179, 21378, 22650, 23997, 25424, 26935, 28537, 30234, 32032, 33937,
        /* Notes 96 - 107 (Octave 7: C7) */
        35955, 38093, 40358, 42757, 45300, 47994, 50848, 53871, 57074, 60468, 64063, 65535,
        /* Notes 108 - 127 */
        65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535,
        65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535, 65535
    };
    return (note < 128) ? midi_sid_freqs[note] : 0;
}

#ifdef __cplusplus
}
#endif

#endif /* SRR_SID_H */
