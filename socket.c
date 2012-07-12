/*
 * Code to handle the daemon and client's socket needs.
 *
 
 *
 * $Id$
 */

#include <stdio.h>
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/fcntl.h>
#include "notify.h"
#include "socket.h"

#define TRUE 1
#define FALSE 0
#define ERROR -1;

/*
 * Connect to the daemon socket.
 *
 * Returns the fd of the socket or -1 if error
 */
 
int socketConnectIP(const char *host, const char *service, int family, int socktype) {

	struct addrinfo hints, *list, *p, *ipv6 = NULL, *ipv4 = NULL;
	int sock, res;


	if((!host) || (!service))
		return -1;

  	memset(&hints, 0, sizeof hints);
	
	hints.ai_family = family;
	hints.ai_socktype = socktype;
	
	// Get the address list
	if((res = getaddrinfo(host, service, &hints, &list)) == -1){
		debug(DEBUG_ACTION, "socket_connect_ip(): getaddrinfo failed: %s", gai_strerror(res));
		return -1;
	}
	for(p = list; p ; p = p->ai_next){
		if((!ipv6) && (p->ai_family == PF_INET6))
			ipv6 = p;
		if((!ipv4) && (p->ai_family == PF_INET))
			ipv4 = p;
	}

	if(!ipv4 && !ipv6){
		debug(DEBUG_ACTION,"socket_connect_ip(): Could not find a suitable IP address to connect to");
		return -1;
	}
	
	p = (ipv6) ? ipv6 : ipv4; // Prefer IPV6 over IPV4

	/* Create a socket for talking to the daemon program. */

	sock = socket(p->ai_family, p->ai_socktype,p->ai_protocol );
	if(sock == -1) {
		freeaddrinfo(list);
		debug(DEBUG_ACTION, "socket_connect_ip(): Could not create ip socket: %s", strerror(errno));
		return -1;
	}


	/* Connect the socket */

	if(connect(sock, (struct sockaddr *) p->ai_addr, p->ai_addrlen)) {
		freeaddrinfo(list);
		debug(DEBUG_ACTION, "socket_connect_ip(): Could not connect to inet host:port '%s:%s'.", host, service);
		return -1;
	}
	
	freeaddrinfo(list);

	/* Return this socket. */
	return(sock);
}

/*
 * Read a line of text from a socket.
 *
 */
 
int socketReadLineNonBlocking(int socket, unsigned *pos, char *line, int maxline) {
	char c;
	int res;
	
	if(!pos || !line)
		return ERROR;

	do{
		res = read(socket, &c, 1);	

		if(res < 0){
			if((errno != EAGAIN) && (errno != EWOULDBLOCK)){
				debug(DEBUG_UNEXPECTED, "Read error on fd %d: %s", socket, strerror(errno));
				*pos = 0;
				return ERROR;
			}
			return ERROR;
		}
		else if(res == 1){
			/* debug(DEBUG_ACTION,"Byte received"); */
			if(c == '\r') /* Ignore return */
				continue;
			if(c != '\n'){
				if(*pos < (maxline - 1))
					line[*pos++] = c;
				else
					debug(DEBUG_UNEXPECTED,"End of line buffer reached!");

			}
			else{
				debug(DEBUG_ACTION, "Line received");
				line[*pos] = 0;
				*pos = 0;
				return TRUE;
			}
		}
	} while(TRUE);

	return ERROR;
	
}

/* 
 * Print to a socket
 */
 
int socketPrintf(int socket, const char *format, ...){
	va_list ap;
	int res = 0;
    
	va_start(ap, format);

	if(socket >= 0)
		res = vdprintf(socket, format, ap);

	va_end(ap);

	return res;	
}

	
