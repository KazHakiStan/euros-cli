#ifndef EUROS_CHAT_H
#define EUROS_CHAT_H

// `euros chat [group]`: line-mode live room. Returns the process exit code.
int chat_run(int argc, char **argv);

// `euros rooms`: the main room and the groups you are in.
int chat_list_rooms(void);

#endif
