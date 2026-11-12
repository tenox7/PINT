/*++

Module Name:

    bcmsndid.h

Abstract:

    What the sound driver of the Raspberry Pi (bcmsound.sys) and its
    multimedia driver (bcmsnd32.dll) share: the name of the service, whose
    Parameters\Device0\Devices the kernel driver fills with its devices and
    the multimedia driver reads, and the numbers of the strings of the
    multimedia driver. As in the drivers of the Windows Sound System, the
    kernel driver names its devices, mixer lines and controls by these
    numbers, which the multimedia driver (drvlib) turns into its strings.

--*/

#ifndef _BCMSNDID_
#define _BCMSNDID_

#define BCMSOUND_SERVICE_NAME           L"Bcmsound"

//
// The product numbers of the devices (MM_MICROSOFT's, as the WSS reports).
//

#define BCMSOUND_PID_WAVEOUT            0x0068
#define BCMSOUND_PID_SYNTH              0x0069
#define BCMSOUND_PID_MIXER              0x006A

#define BCMSOUND_VERSION                0x0100

//
// The strings. 1-31 are synthlib's alerts (SR_ALERT 1, SR_ALERT_NOPATCH 10,
// SR_ALERT_NORESOURCE 11).
//
// The devices: the wave output as it plays, the FM synthesizer (MIDI
// output) and the mixer.
//

#define IDS_WAVEOUT_HDMI                100
#define IDS_WAVEOUT_JACK                101
#define IDS_WAVEOUT_SILENT              102
#define IDS_SYNTH                       103
#define IDS_MIXER                       104

//
// The lines and controls of the mixer.
//

#define IDS_DEST_SPEAKERS_SHORT         110
#define IDS_DEST_SPEAKERS_LONG          111
#define IDS_SRC_WAVEOUT_SHORT           112
#define IDS_SRC_WAVEOUT_LONG            113
#define IDS_SRC_SYNTH_SHORT             114
#define IDS_SRC_SYNTH_LONG              115

#define IDS_CONTROL_VOLUME_SHORT        120
#define IDS_CONTROL_VOLUME_LONG         121
#define IDS_CONTROL_MUTE_SHORT          122
#define IDS_CONTROL_MUTE_LONG           123
#define IDS_CONTROL_WAVEVOLUME_LONG     124
#define IDS_CONTROL_WAVEMUTE_LONG       125
#define IDS_CONTROL_SYNTHVOLUME_LONG    126
#define IDS_CONTROL_SYNTHMUTE_LONG      127

#endif // _BCMSNDID_
