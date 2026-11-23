/*
 * options.h - user choices kept in options.ini, next to the executable
 *
 *   fmv=0|1      Digital Video Cartridge (menu Materiel)
 *   aspect=0|1   picture format: 0 = 4:3, 1 = 16:9 (menu Affichage, F2)
 *   filter=0..5  video filter: 0 none, 1 smooth, 2 scanlines, 3 CRT,
 *                4 sharp smooth, 5 edge smooth (F4)
 *   pads=0|1     controllers on / off (menu Controles)
 */

#ifndef OPTIONS_H
#define OPTIONS_H

/* value of key, or def when missing */
int  option_get(const char *key, int def);
/* set key = value and rewrite the file (other keys kept) */
void option_set(const char *key, int value);

#endif
