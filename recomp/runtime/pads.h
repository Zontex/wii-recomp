// Host controllers, read through SDL3: Switch Pro Controllers, Joy-Cons, Switch 2
// controllers and other gamepads, each standing in for a player's Wii Remote as
// controllers.ini says (pad_map.c has the mappings). Everything here runs on the main
// thread, which the Mac's controller frameworks need.
#pragma once

void pads_start(void);
// Reads the controllers and passes them to their players. Called from the window's loop.
void pads_poll(void);
// How long the window's loop may wait between polls: short while a controller is
// connected, so its motion arrives smoothly; longer with none.
double pads_wait_seconds(void);
// The control pipe's controller commands, "pad add pro" and so on, which drive virtual
// controllers through SDL, so tests go through everything a real one does. Any thread;
// they run at the next poll. Returns 0 for a line that isn't a pad command.
int pads_command(const char *line);
