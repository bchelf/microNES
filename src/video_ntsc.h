#ifndef SMB2350_VIDEO_NTSC_H
#define SMB2350_VIDEO_NTSC_H

// Monochrome composite video on a simple two-resistor DAC:
// - GP0 through 1k to RCA center for sync / blank bias
// - GP1 through 470R to RCA center for added luma
// - RCA shell to GND, with the display assumed to provide 75R termination
#define SMB2350_VIDEO_PIN_BASE 0u
#define SMB2350_VIDEO_PIN_COUNT 2u

void video_ntsc_init(void);
void video_ntsc_start(void);

#endif
