#ifndef SERVER_H
#define SERVER_H
struct engine;
int server_start(int port, struct engine* e);
void server_stop(void);

int server_is_running(void);
int server_current_port(void);

#endif
