/*
 * opl3.h - YMF262 (OPL3) FM synthesizer emulator for the bcmsound driver.
 *
 * Original work, written for PINT from the chip's documented behaviour.
 * Integer-only, no C runtime, no dynamic allocation: the driver embeds an
 * OPL3 structure in its device extension and feeds it the register writes
 * that NT's MIDI-to-FM driver (synthlib) would send to a real OPL3.
 * See opl3.c for details and known limitations.
 *
 * The structure holds pointers into itself: initialise it in place with
 * Opl3Init and do not copy it afterwards.
 */

#ifndef OPL3_H
#define OPL3_H

typedef signed char     OPL_S8;
typedef unsigned char   OPL_U8;
typedef signed short    OPL_S16;
typedef unsigned short  OPL_U16;
typedef signed int      OPL_S32;
typedef unsigned int    OPL_U32;

#define OPL3_NUM_OPS        36
#define OPL3_NUM_CHANNELS   18
#define OPL3_CHIP_RATE      49716   /* 14.31818 MHz / 288 */

typedef struct _OPL3_OP {
    const OPL_U16 *WaveTab; /* log-sine table of the selected waveform */
    OPL_U32 Phase;          /* phase accumulator, 2^20 per cycle */
    OPL_U32 Inc;            /* phase increment per chip sample (incl. vibrato) */
    OPL_U32 EnvFrac;        /* envelope rate accumulator, 16.16 */
    OPL_U32 AttackInc;      /* envelope steps per sample, 16.16 */
    OPL_U32 DecayInc;
    OPL_U32 ReleaseInc;
    OPL_U32 CurInc;         /* rate of the current envelope state */
    OPL_S16 Env;            /* envelope attenuation, 0..511 (0.1875 dB) */
    OPL_S16 Att8;           /* (Env + BaseAtt) * 8: log attenuation */
    OPL_S16 Target;         /* level that ends the current state */
    OPL_S16 BaseAtt;        /* TL + KSL attenuation, same units */
    OPL_S16 SustainLevel;   /* same units */
    OPL_S16 AmMask;         /* -1 when tremolo is on, else 0 */
    OPL_U8  State;          /* OPL_EG_* */
    OPL_U8  Wave;           /* effective waveform 0..7 */
    OPL_U8  AttackRate;     /* effective attack rate 0..63 */
    OPL_U8  KeyOn;          /* bit 0: channel key, bit 1: rhythm key */
    OPL_U8  Reg20;          /* AM VIB EGT KSR MULT */
    OPL_U8  Reg40;          /* KSL TL */
    OPL_U8  Reg60;          /* AR DR */
    OPL_U8  Reg80;          /* SL RR */
    OPL_U8  RegE0;          /* WS */
    OPL_U8  Channel;        /* owning channel 0..17 */
} OPL3_OP;

typedef struct _OPL3_CHANNEL {
    OPL3_OP *P[4];          /* operators in signal order for Algo */
    OPL_S16 Fb[2];          /* last two outputs of the first operator */
    OPL_U16 FNum;           /* 10-bit F-number */
    OPL_U8  Block;          /* 3-bit octave */
    OPL_U8  RegC0;          /* CHD CHC CHB CHA FB CNT */
    OPL_U8  KeyOn;          /* B0 bit 5 */
    OPL_U8  Mode;           /* OPL_CH_* */
    OPL_U8  Op[2];          /* operator indices */
    OPL_U8  Algo;           /* OPL_ALG_*: how Generate renders it */
    OPL_U8  FbShift;        /* 0: no feedback, else 9 - FB */
    OPL_S16 MaskL;          /* -1 when routed to the left output */
    OPL_S16 MaskR;          /* -1 when routed to the right output */
    OPL_S16 FmMask;         /* 2-op: -1 for FM (1 -> 2), 0 for AM (1 + 2) */
    OPL_S16 Pad;
} OPL3_CHANNEL;

typedef struct _OPL3 {
    OPL3_OP      Op[OPL3_NUM_OPS];
    OPL3_CHANNEL Ch[OPL3_NUM_CHANNELS];

    /* Global registers. */
    OPL_U8  New;            /* 0x105 bit 0: OPL3 mode */
    OPL_U8  Wse;            /* 0x01 bit 5: OPL2 waveform select enable */
    OPL_U8  Nts;            /* 0x08 bit 6: note select */
    OPL_U8  RegBD;          /* AM depth, VIB depth, rhythm, drum keys */
    OPL_U8  Conn4Op;        /* 0x104 */
    OPL_U8  Timer1;         /* 0x02 */
    OPL_U8  Timer2;         /* 0x03 */
    OPL_U8  TimerCtl;       /* 0x04 mask and start bits */
    OPL_U8  Status;         /* bits 6/5: timer flags */
    OPL_U8  VibPos;         /* 0..7 */
    OPL_U8  TremVal;        /* current tremolo attenuation */
    OPL_U8  Pad;

    /* Timers and LFOs, all clocked by chip samples. */
    OPL_U16 Timer1Cnt;
    OPL_U16 Timer2Cnt;
    OPL_U32 SampleCnt;      /* free-running chip sample counter */
    OPL_U32 Noise;          /* 23-bit LFSR for rhythm noise */

    /* Resampler: chip rate -> output rate. */
    OPL_U32 OutputRate;
    OPL_U32 Step;           /* chip samples per output frame, 16.16 */
    OPL_U32 Frac;           /* position between Prev and Cur, 16.16 */
    OPL_S32 PrevL, PrevR, CurL, CurR;
} OPL3;

void Opl3Init(OPL3 *Chip, unsigned long OutputRate);
void Opl3Reset(OPL3 *Chip);
void Opl3Write(OPL3 *Chip, unsigned int Register, unsigned char Value);
unsigned char Opl3ReadStatus(OPL3 *Chip);
void Opl3Render(OPL3 *Chip, short *Stereo, unsigned long Frames);
int Opl3IsSilent(OPL3 *Chip);

#endif /* OPL3_H */
