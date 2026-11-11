/* two more pages of the home screen: the extras picker and the settings */
#ifndef EXTRAS_H
#define EXTRAS_H
#include "ui.h"

#define EXTRA_ACTIONS 200
#define SET_ACTIONS 300

int extras_wanted(void);        /* nothing chosen or skipped yet in this session */
void extras_open(void);
int extras_key(key k);          /* 0 back home, 1 stay, 2 an install started */
void extras_draw(canvas *c, int online);

void set_open(void);
int set_key(key k);             /* 0 back home */
void set_draw(canvas *c);
int set_fps(void);
int set_clock12(void);
int cmd_apply(void);

void fix_open(void);
int fix_key(key k);
void fix_draw(canvas *c);
int fix_fps(void);

#endif
