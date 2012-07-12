/*
 * Socket handling headers.
 */

#ifndef SOCKET_H
#define SOCKET_H

#include <sys/socket.h>

/* Prototypes. */

int socketConnectIP(const char *host, const char *service, int family, int socktype);
int socketReadLineNonBlocking(int socket, unsigned *pos, char *line, int maxline);
int socketPrintf(int socket, const char *format, ...);


#endif
