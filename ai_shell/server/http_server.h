#ifndef AI_SHELL_HTTP_SERVER_H
#define AI_SHELL_HTTP_SERVER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

	// Start the HTTP server on a given port (blocking or threaded)
	int http_server_start(int port);

	// Stop server (optional)
	void http_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif

