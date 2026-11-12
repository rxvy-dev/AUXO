#ifndef AUXO_SND_H
#define AUXO_SND_H
/* ALSA's kernel interface for Linux programs (kern/snd.c), on kern/hda.c */
#include <stdint.h>
struct task; struct file; struct node;

void     snd_init(void);                                    /* finds the card, makes /dev/snd */
int      snd_open(struct file *f);                          /* 0, or -EBUSY: one player at a time */
void     snd_close(struct file *f);
long     snd_ioctl(struct task *t, struct file *f, uint64_t req, uint64_t arg);
int      snd_ready(struct file *f);                         /* poll: 4 writable, 8 error */
uint64_t snd_rdev(struct node *n);                          /* major 116 */
struct node *snd_mmap_node(struct file *f, uint64_t off, uint64_t len);   /* the ring, for mmap */

#endif
