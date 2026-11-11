/* open windows, through the compositor's foreign-toplevel protocol */
#ifndef WINS_H
#define WINS_H

struct win { char title[80], app[48]; int id, active; };

void wins_start(void);
int wins_get(struct win *out, int max);
void wins_focus(int id);

#endif
