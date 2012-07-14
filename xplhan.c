/*
*    
*    Copyright (C) 2012  Stephen A. Rodgers
*
*    This program is free software: you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*
*    
*
*
*/


/* Define these if not defined */

#ifndef VERSION
	#define VERSION "X.X.X"
#endif

#ifndef EMAIL
	#define EMAIL "hwstar@rodgers.sdcoxmail.com"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <ctype.h>
#include <getopt.h>
#include <limits.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <poll.h>
#include <xPL.h>
#include "types.h"
#include "notify.h"
#include "confread.h"
#include "socket.h"

#define MALLOC_ERROR	malloc_error(__FILE__,__LINE__)

#define SHORT_OPTIONS "c:d:f:hi:l:ns:v"

#define WS_SIZE 256

#define DEF_PID_FILE		"/var/run/xplhan.pid"
#define DEF_CONFIG_FILE		"/etc/xplhan.conf"
#define DEF_INSTANCE_ID		"test"
#define DEF_HOST			"localhost"
#define DEF_SERVICE			"1129"

#define MAX_SERVICES 64
#define MAX_UNITS_PER_COMMAND 5
#define MAX_HAN_DEVICE 16

typedef enum {GNOP=0x00, GVLV= 0x10, GRLY= 0x11, GTMP=0x12, GOUT=0x13, GINP=0x14, GACD=0x15} hanCommands_t;

typedef enum {NULLUNIT=0, FAHRENHEIT, CELSIUS, VOLTS, AMPS, HERTZ, OUTPUT} units_t;
 
typedef struct cloverrides {
	unsigned pid_file : 1;
	unsigned instance_id : 1;
	unsigned log_path : 1;
	unsigned interface : 1;
} clOverride_t;

/* Command keyword to code map */
typedef struct han_command_map hanCommandMap_t;
struct han_command_map{
	hanCommands_t code;
	units_t valid_units[MAX_UNITS_PER_COMMAND];
	String keyword;
};

/* Units to code map */
typedef struct units_map unitsMap_t;
struct units_map{
	units_t code;
	String keyword;
};

	
typedef struct service_entry serviceEntry_t;
typedef serviceEntry_t * serviceEntryPtr_t;

/*
 * Service entry data structure.
 * There is one of these for each virtual service in this gateway
 */
 
struct service_entry
{
	unsigned address;
	hanCommands_t cmd;
	units_t units;
	unsigned service_id;
	uint32_t iid_hash;
	Bool is_sensor;
	String instance_id;
	String class;
	String type;
	xPL_ServicePtr xplService;
	serviceEntryPtr_t prev;
	serviceEntryPtr_t next;		
};

/*
 * Work Queue data structure
 * These are created by incoming xPL requests and commands
 */


typedef struct workq_entry workQEntry_t;
typedef workQEntry_t * workQEntryPtr_t;


struct workq_entry
{
	serviceEntryPtr_t sp;
	String cmd;
	workQEntryPtr_t prev;
	workQEntryPtr_t next;
};


typedef struct response response_t;
typedef response_t * responsePtr_t;

struct response
{
	uint_least8_t address;
	uint_least8_t command;
	uint_least8_t params[16];
}__attribute__ ((__packed__));
	

/*
 * Variables
 */

char *progName;
int debugLvl = 0; 

static Bool noBackground = FALSE;
static Bool cmdFail = FALSE;
static int hanSock = -1;
static clOverride_t clOverride = {0,0,0,0};

static serviceEntryPtr_t serviceEntryHead = NULL;
static serviceEntryPtr_t serviceEntryTail = NULL;
static serviceEntryPtr_t pendingResponse = NULL;
static workQEntryPtr_t workQHead = NULL;
static workQEntryPtr_t workQTail = NULL;

static ConfigEntryPtr_t	configEntry = NULL;

static char configFile[WS_SIZE] = DEF_CONFIG_FILE;
static char interface[WS_SIZE] = "";
static char logPath[WS_SIZE] = "";
static char instanceID[WS_SIZE] = DEF_INSTANCE_ID;
static char pidFile[WS_SIZE] = DEF_PID_FILE;
static char host[WS_SIZE] = DEF_HOST;
static char service[WS_SIZE] = DEF_SERVICE;


/* Commandline options. */

static const struct option longOptions[] = {
	{"config-file", 1, 0, 'c'},
	{"debug", 1, 0, 'd'},
	{"help", 0, 0, 'h'},
	{"interface", 1, 0, 'i'},	
	{"instance", 1, 0, 's'},	
	{"log", 1, 0, 'l'},
	{"no-background", 0, 0, 'n'},
	{"pid-file", 0, 0, 'f'},
	{"version", 0, 0, 'v'},
	{0, 0, 0, 0}
};

/* Han command map */

static const hanCommandMap_t hanCommandMap[] = {
	{GTMP, {FAHRENHEIT, CELSIUS, NULLUNIT}, "gtmp"},
	{GACD, {VOLTS, HERTZ, NULLUNIT}, "gacd"},
	{GOUT, {OUTPUT,NULLUNIT}, "gout"},
	{GNOP, {NULLUNIT}, NULL}
};

/* Units map */

static const unitsMap_t unitsMap[] = {
	{FAHRENHEIT, "fahrenheit"},
	{CELSIUS,"celsius"},
	{VOLTS,"volts"},
	{AMPS,"amps"},
	{HERTZ,"hertz"},
	{OUTPUT,"output"},
	{NULLUNIT, NULL}
};

	


/* 
 * Allocate a memory block and zero it out
 */

static void *mallocz(size_t size)
{
	void *m = malloc(size);
	if(m)
		memset(m, 0, size);
	return m;
}
 
/*
 * Malloc error handler
 */
 
static void malloc_error(String file, int line)
{
	fatal("Out of memory in file %s, at line %d");
}


/*
* Convert a string to an unsigned int with bounds checking
*/

static Bool str2uns(String s, unsigned *num, unsigned min, unsigned max)
{
		long val;
		int len,i;
		if((!num) || (!s)){
			debug(DEBUG_UNEXPECTED, "NULL pointer passed to str2uns");
			return FALSE;
		}
		
		len = strlen(s);
		
		for(i = 0; i < len; i++){
			if(!isdigit(s[i]))
				break;
		}
		if(i != len)
			return FALSE;
			
		val = strtol(s, NULL, 0);
		if((val < min) || (val > max))
			return FALSE;
		*num = (unsigned) val;
		return TRUE;
}


/*
* Duplicate or split a string. 
*
* The string is copied, and the sep characters are replaced with nul's and a list pointers
* is built. 
* 
* If no sep characters are found, the string is just duped and returned.
*
* This function returns the number of arguments found.
*
* When the caller is finished with the list and the return value is non-zero he should free() the first entry.
* 
*
*/

static int dupOrSplitString(const String src, String *list, char sep, int limit)
{
		String p, q, srcCopy;
		int i;
		

		if((!src) || (!list) || (!limit))
			return 0;

		if(!(srcCopy = strdup(src)))
			MALLOC_ERROR;

		for(i = 0, q = srcCopy; (i < limit) && (p = strchr(q, sep)); i++, q = p + 1){
			*p = 0;
			list[i] = q;
		
		}

		list[i] = q;
		i++;

		return i;
}

/* 
 * Get the pid from a pidfile.  Returns the pid or -1 if it couldn't get the
 * pid (either not there, stale, or not accesible).
 */
static pid_t pid_read(char *filename) {
	FILE *file;
	pid_t pid;
	
	/* Get the pid from the file. */
	file=fopen(filename, "r");
	if(!file) {
		return(-1);
	}
	if(fscanf(file, "%d", &pid) != 1) {
		fclose(file);
		return(-1);
	}
	if(fclose(file) != 0) {
		return(-1);
	}
	
	/* Check that a process is running on this pid. */
	if(kill(pid, 0) != 0) {
		
		/* It might just be bad permissions, check to be sure. */
		if(errno == ESRCH) {
			return(-1);
		}
	}
	
	/* Return this pid. */
	return(pid);
}


/* 
 * Write the pid into a pid file.  Returns zero if it worked, non-zero
 * otherwise.
 */
static int pid_write(char *filename, pid_t pid) {
	FILE *file;
	
	/* Create the file. */
	file=fopen(filename, "w");
	if(!file) {
		return -1;
	}
	
	/* Write the pid into the file. */
	(void) fprintf(file, "%d\n", pid);
	if(ferror(file) != 0) {
		(void) fclose(file);
		return -1;
	}
	
	/* Close the file. */
	if(fclose(file) != 0) {
		return -1;
	}
	
	/* We finished ok. */
	return 0;
}

/*
* When the user hits ^C, logically shutdown
* (including telling the network the service is ending)
*/

static void shutdownHandler(int onSignal)
{
	serviceEntryPtr_t sp;
	
	for(sp = serviceEntryTail; sp; sp = sp->prev){
		xPL_setServiceEnabled(sp->xplService, FALSE);
		xPL_releaseService(sp->xplService);
		xPL_shutdown();
	}
	/* Unlink the pid file if we can. */
	(void) unlink(pidFile);
	exit(0);
}

/* 
 * Dequeue work Queue Entry
 */
  
workQEntryPtr_t dequeueWorkQueueEntry()
{
	workQEntryPtr_t res = NULL;
	
	if(workQTail){
		res = workQTail;
		if(workQTail->prev)
			workQTail->prev->next = NULL;
		else
			workQHead = NULL;
		workQTail = workQTail->prev;
	}
	return res;	
}

/* 
 * Free a work queue entry
 */

void freeWorkQueueEntry(workQEntryPtr_t wqe)
{
	if(wqe){
		if(wqe->cmd)
			free(wqe->cmd);
		free(wqe);
	}
}

/* 
 * Add a command to the work queue
 */
 
void queueCommand(String cmd, serviceEntryPtr_t sp)
{

	workQEntryPtr_t wq = NULL;
	
	/* Allocate work queue entry */
	if(!(wq = mallocz(sizeof(workQEntry_t))))
		MALLOC_ERROR;
	debug(DEBUG_ACTION, "queueCommand()");
	wq->cmd = cmd;
	wq->sp = sp;
	
	if(!workQHead)
		workQHead = workQTail = wq;
	else{
		wq->next = workQHead;
		workQHead->prev = wq;
		workQHead = wq;
	}
}

/*
 * Act on the response from a GOUT command
 */
 
void GOUTAction(unsigned char pcount, responsePtr_t resp, serviceEntryPtr_t sp)
{
	char *res, dev[12];
	xPL_MessagePtr msg;

	
	/* Check for correct number of parameters */
	
	if(pcount != 3){
		debug(DEBUG_UNEXPECTED, "GOUTAction(): Received an incorrect number of parameters, got %u, need 3", pcount);
		return;
	}

	if(resp->params[1] != 2)
		return; /* Only respond when status is requested */
		
	/* Conversion statements */
	if(resp->params[2] == 1)
		res = "high";
	else if(resp->params[2] == 0)
		res = "low";
	else{
		debug(DEBUG_UNEXPECTED,"Unexpected state received: %d", resp->params[2]);
		return; 
	}
	
	/* Format device as string */
	snprintf(dev, 12, "%d", resp->params[0]);

	/* Build a message */
	
	if(!(msg = xPL_createBroadcastMessage(sp->xplService, xPL_MESSAGE_STATUS)))
		debug(DEBUG_UNEXPECTED, "GOUTaction(): Could not create status message");
	xPL_setSchema(msg, "sensor", "basic");
	xPL_setMessageNamedValue(msg, "device", dev);
	xPL_setMessageNamedValue(msg, "type", "output");
	xPL_setMessageNamedValue(msg, "current", res);
	
	
	/* Send the message */
	
	xPL_sendMessage(msg);
	
	/* Release the resource */
	
	xPL_releaseMessage(msg);

}


/*
 * Act on the response from a GACD command
 */
 
void GACDAction(unsigned char pcount, responsePtr_t resp, serviceEntryPtr_t sp)
{
	char ws[12];
	uint_least16_t voltsX10, freqX100;
	xPL_MessagePtr msg;
	float volts, freq;
	
	/* Check for correct number of parameters */
	
	if(pcount != 4){
		debug(DEBUG_UNEXPECTED, "GACDAction(): Received an incorrect number of parameters, got %u, need 4", pcount);
		return;
	}
	/* Conversion statements */
	
	voltsX10 = (((uint_least16_t) resp->params[1]) << 8) + resp->params[0];
	freqX100 = (((uint_least16_t) resp->params[3]) << 8) + resp->params[2];
	volts = ((float) voltsX10)/10;
	freq = ((float) freqX100)/100;
	
	debug(DEBUG_ACTION, "AC Volts = %3.1f, AC Frequency = %2.2f", volts, freq);
	
	/* Build a message */
	
	if(!(msg = xPL_createBroadcastMessage(sp->xplService, xPL_MESSAGE_STATUS)))
		debug(DEBUG_UNEXPECTED, "gtmpaction(): Could not create status message");
	xPL_setSchema(msg, "sensor", "basic");
	if(sp->units == VOLTS){ /* Voltage or frequency? */
		xPL_setMessageNamedValue(msg, "type", "volts");
		snprintf(ws, 10, "%3.1f", volts);
		xPL_setMessageNamedValue(msg, "current", ws);
		xPL_setMessageNamedValue(msg, "units", "volts");
	}
	else{
		xPL_setMessageNamedValue(msg, "type", "frequency");
		snprintf(ws, 10, "%2.2f", freq);
		xPL_setMessageNamedValue(msg, "current", ws);
		xPL_setMessageNamedValue(msg, "units", "hertz");
	}
	
	/* Send the message */
	
	xPL_sendMessage(msg);
	
	/* Release the resource */
	
	xPL_releaseMessage(msg);

}




/*
 * Act on response from GTMP command
 */

void GTMPAction(unsigned char pcount, responsePtr_t resp, serviceEntryPtr_t sp)
{
	int val = 0;
	char ws[12];
	unsigned countsPerC;
	unsigned ch;
	int_least16_t rawTemp;
	xPL_MessagePtr msg;
	
	
	if(pcount != 5){
		debug(DEBUG_UNEXPECTED, "GTMPAction(): Received an incorrect number of parameters, got %u, need 5", pcount);
		return;
	}
	ch = resp->params[0];
	countsPerC = (unsigned) resp->params[1];
	rawTemp = (int_least16_t) ((((uint_least16_t) resp->params[4]) << 8) + resp->params[3]);
	
	printf("Raw temp = %d, counts per c = %d\n", rawTemp, countsPerC);
	
	/* Do conversion per units field */
	if(sp->units == CELSIUS){
		val = rawTemp / countsPerC;
	}
	else if (sp->units == FAHRENHEIT){
		val = (9 * ((int) rawTemp))/(5 * ((int) countsPerC)) + 32;
	}
	else{
		debug(DEBUG_UNEXPECTED, "GTMPAction(): Invalid unit for conversion");
	}

	
	if(!(msg = xPL_createBroadcastMessage(sp->xplService, xPL_MESSAGE_STATUS)))
		debug(DEBUG_UNEXPECTED, "GTMPaction(): Could not create status message");
	xPL_setSchema(msg,"sensor","basic");
	snprintf(ws, 10, "%d", ch);
	xPL_setMessageNamedValue(msg, "device", ws); 
	xPL_setMessageNamedValue(msg, "type", "temp");
	snprintf(ws, 10, "%d", val);
	xPL_setMessageNamedValue(msg, "current", ws);
	xPL_setMessageNamedValue(msg, "units", (sp->units == CELSIUS) ? "celsius" : "fahrenheit");
	xPL_sendMessage(msg);
	xPL_releaseMessage(msg);

}



/*
 * Convert 2 characters into a uint_least8_t
 */
uint_least8_t hex2(String s)
{
	uint_least8_t  res = 0;
	int i;
	for(i = 0; i < 2; i++){
		res <<= 4;
		if((s[i] >= '0') &&  (s[i] <= '9'))
			res |= (uint_least8_t) (s[i] - '0');
		else if((s[i] >= 'A') && (s[i] <= 'F'))
			res |= (uint_least8_t) (s[i] - ('A' - 10));
		else
			break;
	}
	return res;	
}


/*
 * Decode the response, and figure out what to do with it
 */
 
static void decodeResponse(String r)
{
	int i, pcount;
	response_t response;
	
	debug(DEBUG_ACTION, "Line received: %s", r);
	if(!strncmp(r, "RS", 2)){
		response.address = hex2(r + 2);
		response.command = hex2(r + 4);
		pcount = (strlen(r) - 6) >> 1;
		for(i = 0; i < pcount; i++){
			response.params[i] = hex2(r + 6 + (i << 1));
		}
		debug_hexdump(DEBUG_ACTION, &response, pcount + 2, "Binary response dump: ");
		if((pendingResponse) && (pendingResponse->address == (unsigned) response.address)){
			switch((hanCommands_t) response.command){
				case GTMP: /* Temperature */
					GTMPAction(pcount, &response, pendingResponse);
					break;
					
				case GACD: /* AC voltage and frequency */
					GACDAction(pcount, &response, pendingResponse);
					break;
					
				case GOUT: /* Outputs */
					GOUTAction(pcount, &response, pendingResponse);
					break;
				
				default:
					debug(DEBUG_UNEXPECTED, "Unknown response received");
					break;
			}
			pendingResponse = NULL;
		}			
	}
}


	

/*
 * Handler for han socket events
 */

static void hanHandler(int fd, int revents, int userValue)
{
	static unsigned pos = 0;
	static char response[WS_SIZE];
	int res;
	

	debug(DEBUG_ACTION,"revents = %08X", revents);
	
	
	res = socketReadLineNonBlocking(fd, &pos, response, WS_SIZE);
	if(res == -1)
		debug(DEBUG_UNEXPECTED, "Socket read returned error");
	else if (res == 1){
		if(!response[0]){
			/* EOF. We must close the socket and re-open it later */
			xPL_removeIODevice(hanSock);
			close(hanSock);
			hanSock = -1;
			cmdFail = TRUE;
			return;
		}
		decodeResponse(response);
	}
	
	
		
}

/* 
 * Queue GOUT Sensor Request
 */
 
static void queueGOUTRequest(unsigned dev, unsigned subcommand, serviceEntryPtr_t sp)
{
	String cmd;
	
	/* Allocate buffer for command */
	if(!(cmd = mallocz(WS_SIZE)))
		MALLOC_ERROR;
	/* Format command */
	snprintf(cmd, WS_SIZE, "CA%02X%02X%02X%02X00",sp->address, (unsigned ) sp->cmd, dev, subcommand );
	queueCommand(cmd, sp);
}


/*
 * do HAN GOUT command
 */

static void doHanGOUT(xPL_MessagePtr theMessage, serviceEntryPtr_t sp)
{
	unsigned dev;
	unsigned outputState;
	
	

	
	const String device = xPL_getMessageNamedValue(theMessage, "device");

	
	if(!device){
		debug(DEBUG_UNEXPECTED,"doHanGout(): device missing, device required");
		return;
	}
	
	if(!str2uns(device, &dev, 0, MAX_HAN_DEVICE)){
		debug(DEBUG_UNEXPECTED,"doHanGout(): bad device number: %s", device);
		return;
	}		
		
	
	/* GOUT supports both control and sensor schemas */
	
	if(sp->is_sensor){ /* Sensor Request ? */
		const String request = xPL_getMessageNamedValue(theMessage, "request");
		if(!request){
			debug(DEBUG_UNEXPECTED,"doHanGout(): request missing, request required");
			return;
		}
			
		if(strcmp(request, "current")){
			debug(DEBUG_UNEXPECTED,"doHanGout(): only the current request is supported");
			return;
		}
		/* Queue command */
		queueGOUTRequest(dev, 2, sp);
	}
	else{ /* Else assume control request */
		const String type = xPL_getMessageNamedValue(theMessage, "type");
		const String current = xPL_getMessageNamedValue(theMessage, "current");
	
		if(!type){
			debug(DEBUG_UNEXPECTED,"doHanGout(): type missing, type required");
			return;
		}
		
		if(!current){
			debug(DEBUG_UNEXPECTED,"doHanGout(): current missing, current required");
			return;
		}			
		
		if(strcmp(type, "output")){
			debug(DEBUG_UNEXPECTED,"doHanGout(): sensor type must be 'output'");
			return;
		}
		if(!strcmp(current, "high"))
			outputState = 1;
		else if(!strcmp(current, "low"))
			outputState = 0;
		else{
			debug(DEBUG_UNEXPECTED,"current must be one of: high, low");
			return;
		}
		queueGOUTRequest(dev, outputState, sp);
		
	}
	
}



/*
 * do HAN GACD command 
 */
 
static void doHanGACD(xPL_MessagePtr theMessage, serviceEntryPtr_t sp)
{
	String cmd;
	const String request =  xPL_getMessageNamedValue(theMessage, "request");
	
	if(!request){
		debug(DEBUG_UNEXPECTED, "doHanGACD(): no request specified");
		return;
	}
	if(strcmp(request, "current")){ /* Only the current command is supported  */
		debug(DEBUG_UNEXPECTED, "doHanGACD(): only the current request is supported");
		return;
	}
	debug(DEBUG_ACTION, "doHanGACD()");	
		
	/* Allocate buffer for command */
	if(!(cmd = mallocz(WS_SIZE)))
		MALLOC_ERROR;
	
			
	/* Format command */
	snprintf(cmd, WS_SIZE, "CA%02X%02X00000000",sp->address, (unsigned ) sp->cmd);

	queueCommand(cmd, sp);
		
}

/*
 * Do HAN temperature command 
 */
 

static void doHanGTMP(xPL_MessagePtr theMessage, serviceEntryPtr_t sp)
{
	String cmd;
	unsigned dev;
	const String request =  xPL_getMessageNamedValue(theMessage, "request");
	const String device =  xPL_getMessageNamedValue(theMessage, "device");

	
	if(!request){
		debug(DEBUG_UNEXPECTED, "doHanGTMP(): no request specified");
		return;
	}
		
	if(device){	 /* If a device key is present, use it */
		if(!str2uns(device, &dev, 0, MAX_HAN_DEVICE))
			return;
	}
	else{
		debug(DEBUG_UNEXPECTED, "doHanGTMP(): no device specified, device is required");
		return;
	}
	
	if(strcmp(request, "current")){ 
		debug(DEBUG_UNEXPECTED, "doHanGTMP(): only the 'current' request is supported");
		return;
	}

		
	debug(DEBUG_ACTION, "doHanGTMP()");	
		
	/* Allocate buffer for command */
	if(!(cmd = mallocz(WS_SIZE)))
		MALLOC_ERROR;
	
			
	/* Format command */
	snprintf(cmd, WS_SIZE, "CA%02X%02X%02X00000000",sp->address, (unsigned ) sp->cmd, dev);

	queueCommand(cmd, sp);
	

}

/* 
 * HAN Command dispatcher
 */

static void dispatchHanCommand(xPL_MessagePtr theMessage, serviceEntryPtr_t sp)
{
	if((!theMessage) || (!sp))
		return;
		
	debug(DEBUG_ACTION, "dispatchHanCommand()");
	
	switch(sp->cmd){
		case GTMP:
			doHanGTMP(theMessage, sp);
			break;
		
		case GACD:
			doHanGACD(theMessage, sp);
			break;
			
		case GOUT:
			doHanGOUT(theMessage, sp);
			break;
			
		default:
			debug(DEBUG_UNEXPECTED,"Invalid han command received: %02X", (unsigned) sp->cmd);
			break; 
	}
}


/*
* Our Listener 
*/

static void xPLListener(xPL_MessagePtr theMessage, xPL_ObjectPtr userValue)
{
	uint32_t iid_hash;
	serviceEntryPtr_t sp;

	
	if(!xPL_isBroadcastMessage(theMessage)){ /* If not a broadcast message */
		if(xPL_MESSAGE_COMMAND == xPL_getMessageType(theMessage)){ /* If the message is a command */
			const String type = xPL_getSchemaType(theMessage);
			const String class = xPL_getSchemaClass(theMessage);
			const String instanceID = xPL_getTargetInstanceID(theMessage);
			debug(DEBUG_ACTION, "Received message from: %s-%s.%s,\n instance id: %s, class: %s, type: %s",
			xPL_getSourceVendor(theMessage), xPL_getSourceDeviceID(theMessage),
			xPL_getSourceInstanceID(theMessage), instanceID, class, type);
			
			/* Hash the incoming instance ID */
			iid_hash = confreadHash(instanceID);
			
			/* Traverse the service list looking for an instance ID which matches */
			for(sp = serviceEntryHead; sp; sp = sp->next){
				if((iid_hash == sp->iid_hash) && (!strcmp(instanceID, sp->instance_id)))
					break;
	
			}
			if((sp) && (!strcmp(class, sp->class)) && (!strcmp(type, sp->type)))
				dispatchHanCommand(theMessage, sp);	
			
		}
	}
}



/*
* Our tick handler. 
* This is used check for commands to send to the HAN server.
* If one is present, it is sent, and then dequeued and freed.
*/

static void tickHandler(int userVal, xPL_ObjectPtr obj)
{
	/* debug(DEBUG_ACTION,"TICK"); */
	if(workQTail){
		if(hanSock == -1){ /* Socket not connected. This could have been due to an EOF detected previously */
			if((hanSock = socketConnectIP(host, service, PF_UNSPEC, SOCK_STREAM)) < 0){
				debug(DEBUG_UNEXPECTED, "Could not open socket to han server (post fork)");
				freeWorkQueueEntry(dequeueWorkQueueEntry()); /* Can't process command */
				cmdFail = TRUE;
				/* FIXME: Need to find some way to notify the originator the command could not be completed */
				return;
			}
			cmdFail = FALSE;
			/* Add han socket to the xPL polling list */
			if(xPL_addIODevice(hanHandler, 1234, hanSock, TRUE, FALSE, FALSE) == FALSE)
				fatal("Could not register han socket fd with xPL");
		}
		debug(DEBUG_ACTION, "Sending command: %s", workQTail->cmd);
		if(socketPrintf(hanSock, "%s", workQTail->cmd) < 0){ /* Send the command */
			debug(DEBUG_UNEXPECTED, "Command TX failed");
			xPL_removeIODevice(hanSock);
			close(hanSock);
			hanSock = -1;
			cmdFail = TRUE;
		}
		if((workQTail->sp))
			pendingResponse = workQTail->sp; /* Save service entry for a response */
		else
			pendingResponse = NULL;
		freeWorkQueueEntry(dequeueWorkQueueEntry()); /* Remove command from queue, and free it */
	}
}


/*
* Show help
*/

void showHelp(void)
{
	printf("'%s' is a daemon that XXXXXXXXXX\n", progName);
	printf("via XXXXXXXXXXX\n");
	printf("\n");
	printf("Usage: %s [OPTION]...\n", progName);
	printf("\n");
	printf("  -c, --config-file PATH  Set the path to the config file\n");
	printf("  -d, --debug LEVEL       Set the debug level, 0 is off, the\n");
	printf("                          compiled-in default is %d and the max\n", debugLvl);
	printf("                          level allowed is %d\n", DEBUG_MAX);
	printf("  -f, --pid-file PATH     Set new pid file path, default is: %s\n", pidFile);
	printf("  -h, --help              Shows this\n");
	printf("  -i, --interface NAME    Set the broadcast interface (e.g. eth0)\n");
	printf("  -l, --log  PATH         Path name to debug log file when daemonized\n");
	printf("  -n, --no-background     Do not fork into the background (useful for debugging)\n");
	printf("  -s, --instance ID       Set instance id. Default is %s", instanceID);
	printf("  -v, --version           Display program version\n");
	printf("\n");
 	printf("Report bugs to <%s>\n\n", EMAIL);
	return;

}


/*
* main
*/


int main(int argc, char *argv[])
{
	int longindex;
	int optchar;
	int i,j;
	int serviceCount;
	String p;
	SectionEntryPtr_t se;
	serviceEntryPtr_t sp;
	String slist[MAX_SERVICES];

		

	/* Set the program name */
	progName=argv[0];

	/* Parse the arguments. */
	while((optchar=getopt_long(argc, argv, SHORT_OPTIONS, longOptions, &longindex)) != EOF) {
		
		/* Handle each argument. */
		switch(optchar) {
			
				/* Was it a long option? */
			case 0:
				
				/* Hrmm, something we don't know about? */
				fatal("Unhandled long getopt option '%s'", longOptions[longindex].name);
			
				/* If it was an error, exit right here. */
			case '?':
				exit(1);
		
				/* Was it a config file switch? */
			case 'c':
				confreadStringCopy(configFile, optarg, WS_SIZE - 1);
				debug(DEBUG_ACTION,"New config file path is: %s", configFile);
				break;
				
				/* Was it a debug level set? */
			case 'd':

				/* Save the value. */
				debugLvl=atoi(optarg);
				if(debugLvl < 0 || debugLvl > DEBUG_MAX) {
					fatal("Invalid debug level");
				}

				break;

			/* Was it a pid file switch? */
			case 'f':
				confreadStringCopy(pidFile, optarg, WS_SIZE - 1);
				clOverride.pid_file = 1;
				debug(DEBUG_ACTION,"New pid file path is: %s", pidFile);
				break;
			
				/* Was it a help request? */
			case 'h':
				showHelp();
				exit(0);

				/* Specify interface to broadcast on */
			case 'i': 
				confreadStringCopy(interface, optarg, WS_SIZE -1);
				clOverride.interface = 1;
				break;

			case 'l':
				/* Override log path*/
				confreadStringCopy(logPath, optarg, WS_SIZE - 1);
				clOverride.log_path = 1;
				debug(DEBUG_ACTION,"New log path is: %s",
				logPath);

				break;

				/* Was it a no-backgrounding request? */
			case 'n':
				/* Mark that we shouldn't background. */
				noBackground = TRUE;
				break;
	
						
				/* Was it an instance ID ? */
			case 's':
				confreadStringCopy(instanceID, optarg, WS_SIZE);
				clOverride.instance_id = 1;
				debug(DEBUG_ACTION,"New instance ID is: %s", instanceID);
				break;


				/* Was it a version request? */
			case 'v':
				printf("Version: %s\n", VERSION);
				exit(0);
	

			
				/* It was something weird.. */
			default:
				fatal("Unhandled getopt return value %d", optchar);
		}
	}

	
	/* If there were any extra arguments, we should complain. */

	if(optind < argc) {
		fatal("Extra argument on commandline, '%s'", argv[optind]);
	}

	/* Attempt to read a config file */
	
	if(!(configEntry =confreadScan(configFile, NULL)))
		exit(1);
	
	/* Attempt to get general stanza */	
	if(!(se = confreadFindSection(configEntry, "general")))
		fatal("Error in config file: general stanza does not exist");

	/* Host */
	if((p = confreadValueBySectEntKey(se, "host")))
		confreadStringCopy(host, p, WS_SIZE);
		
	/* Port/Service */
	if((p = confreadValueBySectEntKey(se, "port")))
		confreadStringCopy(service, p, WS_SIZE);
	
	
			
	/* Instance ID */
	if((!clOverride.instance_id) && (p = confreadValueBySectEntKey(se, "instance-id")))
		confreadStringCopy(instanceID, p, sizeof(instanceID));
		
	/* Interface */
	if((!clOverride.interface) && (p = confreadValueBySectEntKey(se, "interface")))
		confreadStringCopy(interface, p, sizeof(interface));
			
	/* pid file */
	if((!clOverride.pid_file) && (p = confreadValueBySectEntKey(se, "pid-file")))
		confreadStringCopy(pidFile, p, sizeof(pidFile));	
						
	/* log path */
	if((!clOverride.log_path) && (p = confreadValueBySectEntKey(se, "log-path")))
		confreadStringCopy(logPath, p, sizeof(logPath));
			
	/* Build the instance list */
	if(!(p = confreadValueBySectEntKey(se, "services")))
		fatal("At least one service must be defined in the general section");
	serviceCount = dupOrSplitString(p, slist, ',', MAX_SERVICES);
	
	for(i = 0; i < serviceCount; i++){
	
		if(!(se = confreadFindSection(configEntry, slist[i])))
			fatal("Stanza for service %s does not exist", slist[i]);
		if(!(p = confreadValueBySectEntKey(se, "instance")))
			fatal("Instance for service %s does not exist", slist[i]);
		
		/* Check for duplicate service name  or duplicate instance id */
		for(j = 0, sp = serviceEntryHead; j < i ; j++){
				if(!strcmp(slist[i], slist[j]))
					fatal("Service name %s is already defined", slist[j]);
				if(sp){	
					if(!strcmp(p, sp->instance_id))
						fatal("Instance id %s is already defined", p);
					sp= sp->next;
				}
		}
						
		/* Allocate a data structure */
		if(!(sp = mallocz(sizeof(serviceEntry_t))))
			MALLOC_ERROR;
			
		/* Save instance ID */	
		if(!(sp->instance_id = strdup(p))) 
			MALLOC_ERROR;
		/* Hash  instance ID */
		sp->iid_hash = confreadHash(sp->instance_id);
			
		/* Get Address */
		if(!(p = confreadValueBySectEntKey(se, "address")))
			fatal("Address missing in stanza: %s", slist[i]);
		if(!str2uns(p, &sp->address, 0, 254))
			fatal("In stanza %s, the address must be between 0 and 254", slist[i]);
		
		/* Get class */
		if(!(p = confreadValueBySectEntKey(se, "class")))
			fatal("class missing in stanza: %s", slist[i]);
		if(!(sp->class = strdup(p)))
			MALLOC_ERROR;
		if(!strcmp(sp->class, "sensor")) /* Set flag if sensor */
			sp->is_sensor = TRUE;
	
		
			
		/* Get type */
		if(!(p = confreadValueBySectEntKey(se, "type")))
			fatal("type missing in stanza: %s", slist[i]);
		if(!(sp->type = strdup(p)))
			MALLOC_ERROR;
			
		/* Map han command */
		if(!(p = confreadValueBySectEntKey(se, "han-command")))
			fatal("han-command missing in stanza: %s", slist[i]);		
		for(j = 0; hanCommandMap[j].code ; j++){
			if(!strcmp(p, hanCommandMap[j].keyword))
				break;
		}
		if(!(sp->cmd = hanCommandMap[j].code))
			fatal("Unrecognized han-command: %s in stanza: %s", p, slist[i]);
			
		/* Map units, if class is 'sensor' */
		if(sp->is_sensor){
			if(!(p = confreadValueBySectEntKey(se, "units")))
				fatal("units missing in stanza: %s", slist[i]);			
			for(j = 0; unitsMap[j].code ; j++){
				if(!strcmp(p, unitsMap[j].keyword))
					break;
			}
		}
		if(!(sp->units = unitsMap[j].code))
			fatal("Unrecognized units: %s in stanza: %s", p, slist[i]);	
				
		
		/* Add the service ID */
		sp->service_id = i;	
		
		/* Insert entry into service list */
		if(!serviceEntryHead)
			serviceEntryHead = serviceEntryTail = sp;
		else{
			serviceEntryTail->next = sp;
			sp->prev = serviceEntryTail;
			serviceEntryTail = sp;
		}	
	}
	free(slist[0]); /* Free service list */
	
	/*
	 * Sanity check the command to units mapping if class is 'sensor'
	 */
	 
	 for(sp = serviceEntryHead; sp; sp = sp->next){
		 if(sp->is_sensor){
			for(i = 0; hanCommandMap[i].code; i++){
				if(hanCommandMap[i].code == sp->cmd){
					for(j = 0; hanCommandMap[i].valid_units[j]; j++){
						if(sp->units == hanCommandMap[i].valid_units[j]){
							break;
						}
					}
					if(!hanCommandMap[i].valid_units[j]){
						fatal("Instance %s fails sanity check of han command to units", sp->instance_id);
					}
				}
			}
		}		
	}
	
	/*
	 * Do a test connect to the han server
	 */
 
	if((hanSock = socketConnectIP(host, service, PF_UNSPEC, SOCK_STREAM)) < 0)
		fatal("Could not connect to han server");
	close(hanSock);
	hanSock = -1;


	/* Turn on library debugging for level 5 */
	if(debugLvl >= 5)
		xPL_setDebugging(TRUE);

  	/* Make sure we are not already running (.pid file check). */
	if(pid_read(pidFile) != -1) {
		fatal("%s is already running", progName);
	}

	/* Fork into the background. */

	if(!noBackground) {
		int retval;
		debug(DEBUG_STATUS, "Forking into background");

    	/* 
		* If debugging is enabled, and we are daemonized, redirect the debug output to a log file if
    	* the path to the logfile is defined
		*/

		if((debugLvl) && (logPath[0]))                          
			notify_logpath(logPath);
			
	
		/* Fork and exit the parent */

		if((retval = fork())){
      			if(retval > 0)
				exit(0);  /* Exit parent */
			else
				fatal_with_reason(errno, "parent fork");
    		}



		/*
		* The child creates a new session leader
		* This divorces us from the controlling TTY
		*/

		if(setsid() == -1)
			fatal_with_reason(errno, "creating session leader with setsid");


		/*
		* Fork and exit the session leader, this prohibits
		* reattachment of a controlling TTY.
		*/

		if((retval = fork())){
			if(retval > 0)
        			exit(0); /* exit session leader */
			else
				fatal_with_reason(errno, "session leader fork");
		}

		/* 
		* Change to the root of all file systems to
		* prevent mount/unmount problems.
		*/

		if(chdir("/"))
			fatal_with_reason(errno, "chdir to /");

		/* set the desired umask bits */

		umask(022);
		
		/* Close STDIN, STDOUT, and STDERR */

		close(0);
		close(1);
		close(2);
		} 

	/* Set the xPL interface */
	xPL_setBroadcastInterface(interface);

	/* Start xPL up */
	if (!xPL_initialize(xPL_getParsedConnectionType())) {
		fatal("Unable to start xPL lib");
	}

	/* Initialize xplrcs service */

	/* Create virtual services and set our application version */
	for(sp = serviceEntryHead; sp; sp = sp->next){
		debug(DEBUG_EXPECTED, "Creating xplhan service with class: %s type: %s with instance ID: %s", sp->class, sp->type, sp->instance_id);
		sp->xplService = xPL_createService("hwstar", "xplhan", sp->instance_id);
		xPL_setServiceVersion(sp->xplService, VERSION);
	}


  	/* Install signal traps for proper shutdown */
 	signal(SIGTERM, shutdownHandler);
 	signal(SIGINT, shutdownHandler);
 
	/* Add 1 second tick service */
	xPL_addTimeoutHandler(tickHandler, 1, NULL);

  	/* And a listener for all xPL messages */
  	xPL_addMessageListener(xPLListener, NULL);


 	/* Enable the service */
 	for(sp = serviceEntryHead; sp; sp = sp->next){
		xPL_setServiceEnabled(sp->xplService, TRUE);
	}

	if(pid_write(pidFile, getpid()) != 0) {
		debug(DEBUG_UNEXPECTED, "Could not write pid file '%s'.", pidFile);
	}




 	/** Main Loop **/

	for (;;) {
		/* Let XPL run forever */
		xPL_processMessages(-1);
  	}

	exit(1);
	return 1;
}

