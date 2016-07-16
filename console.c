/* CONSOLE Interface program */

/* This connects to MCP and allows commands to be sent and replies recieved.  Once connected
 debug output can be sent to the console instead of a log file.
 
 It does not actually implement the Telnet protocol for a little security through obscurity
 */


#include <stdio.h>	// for FILE
#include <stdlib.h>	// for timeval
#include <string.h>	// for strlen etc
#include <time.h>	// for ctime
#include <sys/types.h>	// for fd_set
// #include <sys/socket.h>
// #include <netinet/in.h>
#include <netdb.h>	// for sockaddr_in 
#include <fcntl.h>	// for O_RDWR
#include <termios.h>	// for termios
#include <unistd.h>		// for getopt
#ifdef linux
#include <errno.h>		// for Linux
#include <signal.h>
#endif

#define REVISION "$Revision: 1.10 $"
/* 0.0.1 12/11/2006 Fixed to send just '\r' for an empty line as otherwise this closes the socket. */
static char *id="$(#)$Id: console.c,v 1.10 2012/12/16 18:42:58 martin Exp $";
// 1.3 08/07/2007 Placed into CVS as console0-1 */
// 1.4 13/07/2007 File locking on program itself to prevent duplicates
// 1.5 20/08/2007 Conditional compilation for OSX/Linux
// 1.6 09/05/2010 Moved to /usr/local/bin
// 1.7 14/07/2010 Added readline capabilities (command line recall)
// 1.8 20/07/2011 Exit on exactly 'restart' not any command that starts with 'restart'
// 1.9 31/07/2011 Increase BUFISZE 2048 -> 8192
// 1.10 16/12/2012 Added SIGQUIT, SIGINT, SIGPIPE handler to reset terminal device.

#define PORTNO 10011

// Severity levels.  ERROR and FATAL terminate program
#define INFO	0
#define	WARN	1
#define	ERROR	2
#define	FATAL	3
// Socket retry params
#define NUMRETRIES 3
#define RETRYDELAY	1000000	/* microseconds */
// Steca values expected
#define NUMPARAMS 15
// Set to if(0) to disable debugging
#define DEBUG if(debug >= 1)
// If defined, send fixed data instead of timeout message
#define DEBUGCOMMS

/* SOCKET CLIENT */

/* Command line params: 
 1 - hostname  - default locahost
 2 - port - default 10010
 */

#ifndef linux
extern
#endif
int errno;  

// Procedures in this file
void sockSend(const char * msg);	// send a string
int processSocket(void);			// process server message
int processLine(void);		// validate complete line
void usage(void);					// standard usage message

/* GLOBALS */
int sockfd = 0;
int debug = 0;

#define BUFSIZE 8192	/* should be longer than max possible line of text from Steca */
char serialbuf[BUFSIZE];	// data accumulates in this global
char * serbufptr = &serialbuf[0];
struct termios oldSettings;
void reset(int sig);
void append(char c);
void dobackspace();
void doleftarrow();
void dorightarrow();
void douparrow();
void dodownarrow();
void dodel();
void beep();
void appendline();
void showline();	// requires global prompt[];
void expandcrnl(char * buf);	// Deal with CR -> CRNL

enum mode {ANY, GOTESC, GOTCSI, NEARLYDEL} state;
#define BACKSPACE 0x7f
#define ESC 0x1b
#define LINELEN 80

char line[LINELEN + 1];
int lineend, linepos;
char prompt[] = ">> ";

#define NUMLINES 80
#define HISTSIZE 2000
char * lineptr[NUMLINES];
char history[HISTSIZE];
int numlines, curline;

/********/
/* MAIN */
/********/
int main(int argc, char *argv[])
// arg1: serial device file
// arg2: optional timeout in seconds, default 60
// arg3: optional 'nolog' to carry on when filesystem full
{
    struct sockaddr_in serv_addr;
    struct hostent *server;
	
	//    char buffer[256];	// only used in logon message 
	char * hostname = "localhost";
	int portnum = PORTNO; 
	
	int run = 1;		// set to 0 to stop main loop
	fd_set readfd; 
	int numfds;
	int option, fd; 
	// Command line arguments
	
	// optind = -1;
	opterr = 0;
	while ((option = getopt(argc, argv, "d?V")) != -1) {
		switch (option) {
			case '?': usage(); exit(1);
			case 'd': debug = 1; break;
			case 'V': printf("%s\n", id); exit(0);
		}
	}
	
	DEBUG printf("Debug on. optind %d argc %d\n", optind, argc);
#ifdef linux	
	if (((fd = open("/bin/console", O_RDONLY)) == -1) && (fd = open("/usr/local/bin/console", O_RDONLY)) == -1) {
#else
	if ((fd = open(argv[0], O_RDONLY)) == -1) {
#endif
		fprintf(stderr, "Can't open self for locking ");
		perror(argv[0]);
		exit(2);
	}
	if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
		fprintf(stderr, "Console is already running.");
		exit(2);
	}

	if (optind < argc) hostname = argv[optind];	// get hostname
	if (++optind < argc) portnum = atoi(argv[optind]);	// get optional controller number: parameter 2

	// Set up socket 
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) 
		fprintf(stderr, "FATAL creating socket");
	server = gethostbyname(hostname);
	if (server == NULL) {
		fprintf(stderr, "FATAL Cannot resolve localhost");
	}
	bzero((char *) &serv_addr, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	bcopy((char *)server->h_addr, 
		  (char *)&serv_addr.sin_addr.s_addr,
		  server->h_length);
	serv_addr.sin_port = htons(PORTNO);
	if (connect(sockfd,(struct sockaddr *) &serv_addr,sizeof(serv_addr)) < 0) {
		sockfd = 0;
		fprintf(stderr, "ERROR connecting to socket");
	}	
	
	// Set up tty
	struct termios newSettings;
	tcgetattr(STDIN_FILENO, &newSettings);
	tcgetattr(STDIN_FILENO, &oldSettings);
	cfmakeraw(&newSettings);
	newSettings.c_lflag |= ISIG;	// permit INTR
	newSettings.c_oflag |= ONLCR;	// This doesn't work!
	if (tcsetattr(STDIN_FILENO, TCSANOW, &newSettings))
		perror("Setting Stdin");
	
	signal(SIGQUIT, reset);	
	signal(SIGINT, reset);	
	signal(SIGTERM, reset);	
	signal(SIGPIPE, reset);		
	
	// Logon to server
	/* NOT NEEDED 
	 sprintf(buffer, "logon console %s %d %d", VERSION, getpid(), controllernum);
	 sockSend(buffer);
	 */	
	numfds = sockfd + 1;		// nfds parameter to select. One more than highest descriptor
	
	// Main Loop
	FD_ZERO(&readfd); 		struct timeval timeout;
	fprintf(stderr, ">> ");
	
	lineend = linepos = numlines = curline = 0;
	showline();
	
	while(run) {
		timeout.tv_sec = 6;
		timeout.tv_usec = 0;
		FD_SET(sockfd, &readfd);
		FD_SET(STDIN_FILENO,  &readfd);
		if (select(numfds, &readfd, NULL, NULL, NULL) == 0) {	// select timed out. Bad news 
		}
		DEBUG fprintf(stderr, "Select finished: ");
		if (FD_ISSET(STDIN_FILENO, &readfd)) {	// get some input
			run = processTerminal();
			DEBUG fprintf(stderr, "After processLine, run = %d\n", run);
		}
		if (FD_ISSET(sockfd, &readfd)) {
			run &= processSocket();	// the server may request a shutdown by setting run to 0
			DEBUG fprintf(stderr, "After processSocket, run = %d\n", run);
		}
	}
	close(sockfd);
	fprintf(stderr, "\r\n");
	reset(0);
	
	return 0;
	}

/*********/
/* USAGE */
/*********/
void usage(void) {
	printf("Usage: console [hostname] [portnum]\n");
	return;
}

/************/
/* SOCKSEND */
/************/
void sockSend(const char * msg) {
	// Send the string to the server.  May terminate the program if necessary
	short int msglen, written;
	int retries = NUMRETRIES;
	
	msglen = strlen(msg);
	written = htons(msglen);
	
	if (write(sockfd, &written, 2) != 2) { // Can't even send length ??
		sockfd = 0;		// prevent logmsg trying to write to socket!
		fprintf(stderr, "ERROR Can't write a length to socket");
	}
	while ((written = write(sockfd, msg, msglen)) < msglen) {
		// not all written at first go
		msg += written; msglen =- written;
		DEBUG fprintf(stderr, "Only wrote %d; %d left \n", written, msglen);
		if (--retries == 0) {
			fprintf(stderr, "WARN Timed out writing to server"); 
			return;
		}
		usleep(RETRYDELAY);
	}
	DEBUG fprintf(stderr, "Socksend on fd %d sent '%s'\n", sockfd, msg);
}

/*****************/
/* PROCESSSOCKET */
/*****************/
int processSocket(void){
	// Deal with commands from MCP.  Return to 0 to do a shutdown
	short int msglen, numread;
	char buffer[BUFSIZE];	// about 128 is good but rather excessive since longest message is 'truncate'
	char * cp = &buffer[0];
	int retries = NUMRETRIES;
	
	if (read(sockfd, &msglen, 2) != 2) {
		fprintf(stderr, "WARN Failed to read length from socket");
		return 0;
	}
	msglen =  ntohs(msglen);
	if (msglen >= BUFSIZE) {
		fprintf(stderr, "\nWarning incoming packet is %d\n", msglen);
	}
	// How to safely discard?
	while ((numread = read(sockfd, cp, msglen)) < msglen) {
		cp += numread;
		msglen -= numread;
		if (--retries == 0) {
			fprintf(stderr, "WARN Timed out reading from server");
			return 0;
		}
		usleep(RETRYDELAY);
	}
	cp[numread] = '\0';	// terminate the buffer 
	
	if (strcmp(buffer, "Ok") == 0)
		return 1;	// Just acknowledgement
	
	expandcrnl(buffer);
	fprintf(stderr, "\r\n%s", prompt);
	return 1;	
};

/*******************/
/* PROCESSTERMINAL */
/*******************/
int processTerminal() {
	// STDIN is readable
	char c;
	int retval;
	read(STDIN_FILENO, &c, 1);
	if (c == 4)	return 0;			// Ctrl-D to finish
	// fprintf(stderr, "0x%x ", c);
	if (c == ESC)				{state = GOTESC; return 1;}
	if (c == '\r')				{retval = processLine(); appendline(); return retval;}
	if (c == BACKSPACE)			{dobackspace();return 1;}
	if (state == GOTESC) {
		if (c == '[')	{state = GOTCSI; return 1;}
		state = ANY;
		return 1;
	}
	if (state == GOTCSI) {
		state = ANY;
		switch(c) {
			case 'A':	douparrow(); break;
			case 'B':	dodownarrow(); break;
			case 'C':	dorightarrow(); return 1;
			case 'D':	doleftarrow(); return 1;
			case '3':	state = NEARLYDEL; return 1;
		}
		return 1;
	}
	if (state == NEARLYDEL && c == '~') {state = ANY; dodel(); return 1;}
	append(c);
	return 1;
	
}
	
/***************/
/* PROCESSLINE */
/***************/
int processLine(void) {
	// Process a line of data from the terminal.  Return 0 to stop program
	// Normally, strip off the <CR>
	DEBUG fprintf(stderr, "Got '%s'\n", line);
	fputs("\r\n", stderr);
	if (strncasecmp(line, "exit", 4) == 0) return 0;
	
	if (strcmp(line, "localdebug 0\n") == 0) {	// turn off debug
		debug = 0;
		fprintf(stderr, "Local Debug OFF\n>>");
		return 1;
	}
	if (strcmp(line, "localdebug 1\n") == 0) {	// enable debugging
		debug = 1;
		fprintf(stderr, "Local Debug ON\n>>");
		return 1;
	}
	if (strncasecmp(line, "help", 4) == 0 ||
		strncmp(line, "?", 1) == 0)
		fprintf(stderr, "Local commands: exit, localdebug 0|1\r\nRemote commands: ");
	// otherwise send it
	if (strlen(line)) 
		sockSend(line);
	if (strcasecmp(line, "restart") == 0)
		return 0;
	return 1;
};

void reset(int sig) {
	// Reset serial line before exit
	tcsetattr(STDIN_FILENO, TCSANOW, &oldSettings);

	// fprintf(stderr,"Exit on signal %d\n'%s'\n", sig, line);
	exit(0);
}

void append(char c) {
	// fprintf(stderr, "Append '%c' ", c);
	if (lineend == LINELEN) {
		beep();
		return;
	}
	if (lineend == linepos) {	// append at end
		line[lineend++] = c;
		linepos++;
	}
	else
	{
		memmove(line + linepos + 1, line + linepos, lineend - linepos);
		lineend++;
		line[linepos] = c;
		linepos++;
		fputs("\x1b[@", stderr);
		// When inserting into middle of line
	}
	fputc(c, stderr);
	// fprintf(stderr, "\r\n'%s'   ", line);
}

void dobackspace() {
	// fprintf(stderr ,"Dobackspace ");
	if (linepos == 0) {
		beep();
		return;
	}
	if (linepos == lineend) {		// delete from end of line
		fputs("\x1b[D \x1b[D", stderr);
		linepos--;
		lineend--;
		line[linepos] = 0;
		//		fputc(BACKSPACE, stderr);
	}
	else				// delete from middle of line
	{
		linepos--;
		lineend--;
		memmove(line + linepos, line + linepos + 1, lineend - linepos + 1);
		line[lineend] = 0;
		fputs("\x1b[D\x1b[P", stderr);
	}
}

void doleftarrow() {
	// fprintf(stderr ,"Doleftarrow ");
	if (linepos) {
		linepos--;
		fputs("\x1b[D", stderr);
	}
	else beep();
}
void dorightarrow() {
	// fprintf(stderr ,"Dorightarrow ");
	if (lineend == linepos) {
		beep();
		return;
	}
	linepos++;
	fputs("\x1b[C", stderr);
}
void douparrow() {
	// fprintf(stderr ,"Douparrow ");
	if (curline == 0) {
		beep();
		return;
	}
	strcpy(line, lineptr[--curline]);
	showline();
}

void dodownarrow() {
	// fprintf(stderr ,"Dodownarrow %d %d", curline, numlines);
	if (curline == numlines) {
		beep();
		return;
	}
	if (++curline == numlines) 		// create empty line
		strcpy(line, "");
	else
		strcpy(line, lineptr[curline]);
	showline();
}

void showline() {
	fprintf(stderr, "\r%s\x1b[0K%s", prompt, line);  // Erase line after prompt before outputing rest of line
	linepos = lineend = strlen(line);
}

void dodel() {
	// fprintf(stderr ,"Dodel ");
	if (linepos == lineend) {
		beep();
		return;
	}
	memmove(line + linepos, line + linepos + 1, lineend - linepos + 1);
	if (linepos) linepos--;
	lineend--;
	line[lineend] = 0;
	fputs("\x1b[P", stderr);
}

void beep() { fputc(7, stderr);}

void appendline() {	// Add current line to history if NOT the last line already.
	char * ptr;
	int i;
	// 	fprintf(stderr, "History %d lines\r\n", numlines);
	if (numlines == 0) ptr = history;
	else
		ptr = lineptr[numlines - 1];		// Point to last line
	// 	fprintf(stderr, "\r\n\r\n'%s' == '%s' %d\r\n\r\n", ptr, line, strcmp(ptr,line));
	if (strcmp(line, ptr)) {		// Only if new line is not the same as last line ...
		if (numlines) ptr += strlen(lineptr[numlines - 1])+1;
		lineptr[numlines++] = strcpy(ptr, line);
		// 		fprintf(stderr,"Added at %x\r\n ", ptr);
	}
	/*for(i = 0; i < numlines; i++)
		fprintf(stderr, "\r%x '%s'\n\r", lineptr[i], lineptr[i]);
	*/
	curline = numlines;
	bzero(line, LINELEN);
	linepos = lineend = 0;
//	line[0] = 0;
	showline();
}


	void expandcrnl(char * buf) {
		char *cp;
		for (cp = buf; *cp; cp++) 
			if (*cp == '\n') fputs("\r\n", stderr);
			else fputc(*cp, stderr);
	}
/* Finite State Machine
 
 ANYCHAR
 0x7F ---> BACKSPACE
 NL   ---> DONE
 ESC  ---> GOT ESC
 else append()
 
 GOT ESC
 [	---> GOT CSI
 else ANYCHAR
 
 GOT CSI
 A	---> do up arrow
 B	---> do down arrow
 C	---> do right arrow
 D	---> do left arrow
 3	---> NEARLY DEL
 else ANYCHAR
 
 NEARLY DEL
 ~	---> DELETE
 else anychar
 */

