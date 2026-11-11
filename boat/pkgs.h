/* the package page of the home screen */
#ifndef PKGS_H
#define PKGS_H
#include "ui.h"

void pkgs_open(void);
int pkgs_key(key k);        /* returns 0 when it wants to go back home */
void pkgs_tick(void);
void pkgs_draw(canvas *c);
int pkgs_fps(void);
void pkgs_act(int action, int arg);
void pkgs_run(const char *title, const char *cmd);  /* start a job and show its output */
int pkgs_has(const char *name);
int pkgs_busy(void);                     /* is it installed */

#define PKG_ACTIONS 100     /* this page's actions start here */

#endif
