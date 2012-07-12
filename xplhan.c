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
#define DEF_PORT			1129

#define MAX_SERVICES 32
#define MAX_MESSAGES_PER_INSTANCE 5
#define MAX_UNITS_PER_COMMAND 5

typedef enum {GNOP=0x00, GTMP=0x12, GACD=0x15} hanCommands_t;

typedef enum {NULLUNIT=0, FAHRENHEIT, CELSIUS, VOLTS, AMPS, HERTZ} units_t;
 
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
	uint32_t class_hash;
	String instance_id;
	String class;
	String type;
	xPL_MessagePtr messages[MAX_MESSAGES_PER_INSTANCE];
	xPL_ServicePtr xplService;
	serviceEntryPtr_t prev;
	serviceEntryPtr_t next;		
};


char *progName;
int debugLvl = 0; 

static Bool noBackground = FALSE;
static unsigned port = DEF_PORT;
static int hanSock = -1;
static clOverride_t clOverride = {0,0,0,0};

static serviceEntryPtr_t serviceEntryHead = NULL;
static serviceEntryPtr_t serviceEntryTail = NULL;

static ConfigEntryPtr_t	configEntry = NULL;

static char configFile[WS_SIZE] = DEF_CONFIG_FILE;
static char interface[WS_SIZE] = "";
static char logPath[WS_SIZE] = "";
static char instanceID[WS_SIZE] = DEF_INSTANCE_ID;
static char pidFile[WS_SIZE] = DEF_PID_FILE;
static char host[WS_SIZE] = DEF_HOST;


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
	{GNOP, {NULLUNIT}, NULL}
};

/* Units map */

static const unitsMap_t unitsMap[] = {
	{FAHRENHEIT, "fahrenheit"},
	{CELSIUS,"celsius"},
	{VOLTS,"volts"},
	{AMPS,"amps"},
	{HERTZ,"hertz"},
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
		if((!num) || (!s)){
			debug(DEBUG_UNEXPECTED, "NULL pointer passed to str2uns");
			return FALSE;
		}
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
* Our Listener 
*/



static void xPLListener(xPL_MessagePtr theMessage, xPL_ObjectPtr userValue)
{

	

}



/*
* Our tick handler. 
* This is used to synchonize the sending of data to the RCS thermostat.
*/

static void tickHandler(int userVal, xPL_ObjectPtr obj)
{

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
		
	/* Port */
	if((p = confreadValueBySectEntKey(se, "port"))){
		if(!str2uns(p, &port, 1, 65535))
			fatal("Port must be between 1 and 65535");
	}
			
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
		/* Hash class */
		sp->class_hash = confreadHash(sp->class);
			
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
			
		/* Map units */
		if(!(p = confreadValueBySectEntKey(se, "units")))
			fatal("units missing in stanza: %s", slist[i]);		
		for(j = 0; unitsMap[j].code ; j++){
			if(!strcmp(p, unitsMap[j].keyword))
				break;
		}
		if(!(sp->units = unitsMap[j].code))
			fatal("Unrecognized units: %s in stanza: %s", p, slist[i]);	
			
		
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
	 * Sanity check the command to units mapping 
	 */
	 
	 for(sp = serviceEntryHead; sp; sp = sp->next){
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
	
	/*
	 * Do a test connect to the han server
	 */
	 
	if(((hanSock = socketConnectIP("phones","1129", PF_UNSPEC, SOCK_STREAM)) < 0) || (socketPrintf(hanSock,"\n") < 0))
		fatal("Could not connect to han server");
	close(hanSock);
		
	fatal("Test exit"); /* DEBUG */

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
		sp->xplService = xPL_createService("hwstar", "xplhan", instanceID);
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

