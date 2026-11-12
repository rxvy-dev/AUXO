#ifndef AUXO_HDA_H
#define AUXO_HDA_H
/* Intel High Definition Audio (kern/hda.c): one output stream, polled. */
#include <stdint.h>

int         hda_init(void);                 /* 0 if a controller with an output path was set up */
int         hda_present(void);
const char *hda_codec_name(void);           /* "10ec:0269" */
const char *hda_output_name(void);          /* "speaker", "headphones", "line out" */
uint8_t    *hda_ring(uint32_t *cap);        /* the play-out ring the controller reads */
int         hda_start(int rate, int channels, uint32_t bytes, uint32_t period);   /* 16-bit samples */
void        hda_stop(void);
uint32_t    hda_position(void);             /* byte the controller is at, 0 .. bytes-1 */
void        hda_set_volume(int pct, int mute);
int         hda_volume(void);
int         hda_muted(void);

#endif
