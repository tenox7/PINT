/*++

Module Name:

    opl3.h

Abstract:

    The emulator of the YMF262 (OPL3) FM synthesizer of the sound driver
    (opl3.c), written for PINT from the documented behaviour of the chip.
    It uses integers only, no C runtime and no dynamic allocation: the
    driver holds an OPL3 in its global device information and gives it the
    register writes that NT's MIDI driver of the FM synthesizer (synthlib)
    would send to the chip. See opl3.c for its limitations.

    The structure holds pointers into itself: Opl3Init initializes it in
    place, and it must not be copied afterwards.

--*/

#ifndef _OPL3_
#define _OPL3_

//
// The operators and channels of the chip, and its sample rate
// (14.31818 MHz / 288).
//

#define OPL3_NUM_OPS        36
#define OPL3_NUM_CHANNELS   18
#define OPL3_CHIP_RATE      49716

typedef struct _OPL3_OP {

    //
    // The log-sine table of the selected waveform, the phase accumulator
    // (2^20 a cycle) and its increment a chip sample (with the vibrato).
    //

    const USHORT *WaveTab;
    UINT Phase;
    UINT Inc;

    //
    // The envelope: the accumulator of its rate (16.16), the steps a sample
    // of its states (16.16) and the rate of the current state.
    //

    UINT EnvFrac;
    UINT AttackInc;
    UINT DecayInc;
    UINT ReleaseInc;
    UINT CurInc;

    //
    // The attenuation of the envelope (0-511, 0.1875 dB), the log
    // attenuation ((Env + BaseAtt) * 8), the level that ends the current
    // state, the attenuation of TL and KSL and the sustain level (the units
    // of Env), and the mask of the tremolo (-1 when it is on, else 0).
    //

    SHORT Env;
    SHORT Att8;
    SHORT Target;
    SHORT BaseAtt;
    SHORT SustainLevel;
    SHORT AmMask;

    //
    // The state of the envelope (OPL_EG_*), the effective waveform (0-7)
    // and attack rate (0-63), and the keys (bit 0 the channel's, bit 1 the
    // rhythm's).
    //

    UCHAR State;
    UCHAR Wave;
    UCHAR AttackRate;
    UCHAR KeyOn;

    //
    // The registers: AM VIB EGT KSR MULT, KSL TL, AR DR, SL RR and WS; and
    // the channel that owns the operator (0-17).
    //

    UCHAR Reg20;
    UCHAR Reg40;
    UCHAR Reg60;
    UCHAR Reg80;
    UCHAR RegE0;
    UCHAR Channel;
} OPL3_OP;

typedef struct _OPL3_CHANNEL {

    //
    // The operators in the order of the signal for Algo, the last two
    // outputs of the first operator, the F-number (10 bits) and the octave
    // (3 bits).
    //

    OPL3_OP *P[4];
    SHORT Fb[2];
    USHORT FNum;
    UCHAR Block;

    //
    // The register CHD CHC CHB CHA FB CNT, the key (B0 bit 5), the mode
    // (OPL_CH_*), the indices of the operators, how OplGenerate renders the
    // channel (OPL_ALG_*) and the shift of the feedback (0 for none, else
    // 9 - FB).
    //

    UCHAR RegC0;
    UCHAR KeyOn;
    UCHAR Mode;
    UCHAR Op[2];
    UCHAR Algo;
    UCHAR FbShift;

    //
    // The masks: -1 when the channel goes to the left output, to the right
    // output, and for FM (1 -> 2) of a channel of two operators (0 for AM,
    // 1 + 2).
    //

    SHORT MaskL;
    SHORT MaskR;
    SHORT FmMask;
    SHORT Pad;
} OPL3_CHANNEL;

typedef struct _OPL3 {
    OPL3_OP Op[OPL3_NUM_OPS];
    OPL3_CHANNEL Ch[OPL3_NUM_CHANNELS];

    //
    // The global registers: the OPL3 mode (0x105 bit 0), the waveform
    // select enable of an OPL2 (0x01 bit 5), the note select (0x08 bit 6),
    // the depths of the tremolo and the vibrato, the rhythm and the keys of
    // the drums (0xBD), the channels of four operators (0x104), the timers
    // (0x02, 0x03) and their mask and start bits (0x04); the status (bits 6
    // and 5 the flags of the timers), the position of the vibrato (0-7) and
    // the current attenuation of the tremolo.
    //

    UCHAR New;
    UCHAR Wse;
    UCHAR Nts;
    UCHAR RegBD;
    UCHAR Conn4Op;
    UCHAR Timer1;
    UCHAR Timer2;
    UCHAR TimerCtl;
    UCHAR Status;
    UCHAR VibPos;
    UCHAR TremVal;
    UCHAR Pad;

    //
    // The timers and the LFOs, all clocked by the chip samples: the counts
    // of the timers, the free-running counter of the chip samples and the
    // 23-bit LFSR of the noise of the rhythm.
    //

    USHORT Timer1Cnt;
    USHORT Timer2Cnt;
    UINT SampleCnt;
    UINT Noise;

    //
    // The resampler, from the rate of the chip to the output's: the chip
    // samples an output frame (16.16), the position between the previous
    // and the current chip sample (16.16), and those samples.
    //

    UINT OutputRate;
    UINT Step;
    UINT Frac;
    INT PrevL;
    INT PrevR;
    INT CurL;
    INT CurR;
} OPL3;

VOID
Opl3Init(
    OUT OPL3 *Chip,
    IN ULONG OutputRate
    );

VOID
Opl3Reset(
    IN OUT OPL3 *Chip
    );

VOID
Opl3Write(
    IN OUT OPL3 *Chip,
    IN UINT Register,
    IN UCHAR Value
    );

UCHAR
Opl3ReadStatus(
    IN OPL3 *Chip
    );

VOID
Opl3Render(
    IN OUT OPL3 *Chip,
    OUT PSHORT Stereo,
    IN ULONG Frames
    );

INT
Opl3IsSilent(
    IN OPL3 *Chip
    );

#endif // _OPL3_
