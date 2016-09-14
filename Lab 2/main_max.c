/*main file for Lab 2 in TDTS06
Written by :
Oskar von Heideken, oskvo980@student.liu.se
and
Max Wennerfeldt, maxwe992@student.liu.se

The goal of the lab is to create a Proxy!
*/

/*
Server part - receives the HTTP req. from the browser and delivers it to the client part
Client part - determines to which web server it should connect and sends the HTTP req. and receives the HTTP response.
              The Client then delivers the received content to the server part which sends it to the browser

*/

/*
Requirement specifications:
-The proxy should support both HTTP/1.0 and HTTP/1.1.
-Handles simple HTTP GET interactions between client and server
-Blocks requests for undesirable URLs, using HTTP redirection to display this error page instead
-Detects inappropriate content bytes within a Web page before it is returned to the user, and redirecting to this error page
-Imposes no limit on the size of the transferred HTTP data
    Note: Using the realloc() function to increase the size of the buffer (allocated to receiving the HTTP data from the server) is also considered as imposing a limit. Instead, you must chose a size for the buffer and manage the data in it intelligently so that it does not result in lost packets or unsent data being overwritten when receiving responses. It is also recommended that you avoid performing type casting operations when receiving and sending data from the buffer.
-Is compatible with all major browsers (e.g. Internet Explorer, Mozilla Firefox, Google Chrome, etc.) without the requirement to tweak any advanced feature
-Allows the user to select the proxy port (i.e. the port number should not be hard coded)
-Is smart in selection of what HTTP content should be searched for the forbidden keywords. For example, you probably agree that it is not wise to search inside compressed or other non-text-based HTTP content such as graphic files, etc.
-(Optional) Supporting file upload using the POST method
-You do not have to relay HTTPS requests through the proxy
*/
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include <regex.h>

// Default port if none is given by the users
#define DEFAULTPORT "3123"
#define HTTPPORT    "80"

#define SERVBACKLOG 10

#define MAXBUFLEN 900000

#define DEBUG

#define REDIRECTHEADER "HTTP/1.1 302 Found\r\nDate: Thu, 01 Sep 2016 16:00:52 GMT\r\nServer: Apache\r\nLocation: http://www.ida.liu.se/~TDTS04/labs/2011/ass2/error2.html\r\nContent-Length: 317\r\nConnection: close\r\nContent-Type: text/html; charset=iso-8859-1\r\n\r\n<!DOCTYPE HTML PUBLIC '-//IETF//DTD HTML 2.0//EN'>\n<html><head>\n<title>302 A bad BAD page</title>\n</head><body>\n<h1>You tries to access a bad BAD page</h1>\n<p>For more information regarding what is a BAD site, click <a href=http://www.ida.liu.se/~TDTS04/labs/2011/ass2/error2.html>here</a>.</p>\n</body></html>\n"

#define REDIRECTLEN 561

#define STOPATCLOSE 0
#define STOPATBLOCKING 1
#define MSGLISTSIZE 254

#define OUTGOING	0
#define INCOMMING 	1

struct msg_list
{
    char 			 	data[MSGLISTSIZE];
    unsigned char		dataLen;
    struct msg_list* 	next;
};

//Signal handler
void sigchld_handler(int s)
{
    // waitpid() might overwrite errno, so we save and restore it:
    int saved_errno = errno;

    while(waitpid(-1, NULL, WNOHANG) > 0);

    errno = saved_errno;
}

// get sockaddr, IPv4 or IPv6:
//Source: http://beej.us/guide/bgnet/output/html/multipage/clientserver.html#simpleserver
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET)
    {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

//Incomming client (client -> server) handler function declaration.
void inc_client_handler(int incClientFd, char incClient[INET6_ADDRSTRLEN]);

int dest_connect(char dest[], char destClient[INET6_ADDRSTRLEN]);

struct msg_list* receive_from_socket(int socketFd, char stopat);

int process_list(struct msg_list* pList, char direction, char* pMsg, char* pDest);

int get_list_len(struct msg_list* pList);

int get_line_from_list(struct msg_list* pList, char* pLine);

void clear_list(struct msg_list* pList);

int main(int argc)
{

    int 		servFd, incClientFd;
    int 		servPort = DEFAULTPORT;
    struct 		addrinfo hints, *pServInfo, *pIter;
    struct 		sockaddr_storage incClientAddr;
    socklen_t 	addrLen;
    char 		incClient[INET6_ADDRSTRLEN];

    //To clear zombies
    struct sigaction sa;

    //Retrive and check input port arguments
    if(argc > 1024)
        servPort = argc;
    else
        printf("Invalidor no port provided, using defualt port %u\n",atoi(DEFAULTPORT));

    int pAddrList;

    //Set server side socket info to stream socket with the local IP
    memset(&hints, 0, sizeof(hints));
    hints.ai_family 	= AF_UNSPEC;
    hints.ai_socktype 	= SOCK_STREAM;
    hints.ai_flags 		= AI_PASSIVE;

    //Try retrive address information
    int resp;
    if((resp = getaddrinfo(NULL, DEFAULTPORT, &hints, &pServInfo))!= 0)
    {
        //Error while tyring to retrieve address information, print error and end the program with status 1
        fprintf(stderr, "server side: Error in getaddrinfo: %s\n", gai_strerror(resp));
        return 1;
    }

    //We got the address info, try opening the socket
    for(pIter = pServInfo; pIter != NULL; pIter->ai_next)
    {
        //Try to open socket
        if((servFd = socket(pIter->ai_family, pIter->ai_socktype, pIter->ai_protocol)) == -1)
        {
            //If we could not open the socket, print error message and move on to next interation
            perror("server side: Could not open socket");
            continue;
        }

        //Socket is open, set socket options
        int yes = 1;
        if(setsockopt(servFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1)
        {
            //Unable to set socket options, exit the program with status 1
            perror("server side: Unable to set socket options");
            exit(1);
        }

        //Socket open and options is set, try bind to socket
        if(bind(servFd, pIter->ai_addr, pIter->ai_addrlen) == -1)
        {
            //Unable to bind to the socket, close the socket and move on to next iteration
            close(servFd);
            perror("server side: Unable to bind to the socket");
            continue;
        }

        //Socket open and binded, break the loop
        break;
    }

    //We can now free the server address info as we have the socket open
    freeaddrinfo(pServInfo);

    //If we managed to bind no socket we end the program with status 1
    if(pIter == NULL)
    {
        fprintf(stderr, "server side: Failed dto bind to socket\n");
        exit(1);
    }

    //Try to start listen to the socket, if not able, exit the program with status 1
    if(listen(servFd, SERVBACKLOG) == -1)
    {
        perror("server side: Unable to s tart listening to socket");
        exit(1);
    }

    //Before entering server side loop, clean zombie processes
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1)
    {
        perror("Error cleaning up dead processes: sigaction");
        exit(1);
    }

    printf("server side: Waiting for connections...\n");

    //Server side main loop
    while(1)
    {
        addrLen 	= sizeof(incClientAddr);
        //Wait for a new connection
        incClientFd = accept(servFd, (struct sockaddr *)&incClientAddr, &addrLen);
        if(incClientFd == -1)
        {
            //Unable to accept the connection, move on
            perror("server side: Unable to accept incomming client connection");
            continue;
        }

        //Connection accepted, retreive client information
        inet_ntop(incClientAddr.ss_family,get_in_addr((struct sockaddr *)&incClientAddr),incClient, sizeof(incClient));
        printf("server side: Connection accepted from %s\n",incClient);

        //Fork client handler child processes
        if(!fork())
        {
            //Run incomming client handler
            inc_client_handler(incClientFd, incClient);

            printf("server side: Ending %s child process.",incClient);
            close(incClientFd);
            exit(0);
        }
    }

    close(servFd);

    return 0;
}


/*
	Incomming client handler for server side client connection (client -> server)
*/
void inc_client_handler(int incClientFd, char incClient[INET6_ADDRSTRLEN])
{

    // Destination filedescriptor
    int 	destFd;
    // Destination IP
    char 	destClient[INET6_ADDRSTRLEN];
    // Destination URL pointer
    char*    pDest;

    struct msg_list* pRecList;

    // Message buffer pointer
    char* 	pMsg;
    int 	msgLen;

    // Incomming client main loop
    // TODO: End when final respons is sent (think keep-alive);
    while(1)
    {
        // Read from socket (non blocking)
        pRecList = receive_from_socket(incClientFd,STOPATBLOCKING);
#ifdef DEBUG
            printf("server side: received msg\n");
#endif


        // If recLen is zero, the client closed the connection, end!
        if(pRecList->dataLen == 0)
            break;

        // Process the list and create a char string msg
        int status = process_list(pRecList, OUTGOING, pMsg, pDest);


#ifdef DEBUG
            printf("server side: received msg\n");
#endif
        //Length of msg
        msgLen = sizeof(pMsg);

        //If not a redirect, procceed
        if(status == 0)
        {

#ifdef DEBUG
            pMsg[msgLen] = '\0';
            printf("server side:\n%s received msg:\n'%s'\n",incClient,pMsg);
#endif

            //Open socket to the destination
            destFd = dest_connect(pDest, destClient);

            //If we managed to open the socket, write the modified message to the destination
            if(destFd != -1)
            {
                //Number of bytes sent
                int sentLend = 0;
                //Write request to destination (send until we have sent sendLen number o f bytess)
                while(sentLend != msgLen)
                {
                    if((sentLend += send(destFd, pMsg, msgLen, 0)) == -1)
                    {
                        perror("client side: Error sending to destination");
                    }
                }

                //Free pMsg and pDest
                if(pMsg)
                    free(pMsg);
                if(pDest)
                    free(pDest);

                // Clean list if any trash is remaining
                clear_list(pRecList);

                // Read from socket (non blocking)
                pRecList = receive_from_socket(destFd,STOPATCLOSE);

                // Process the list and create a char string msg
                int status = process_list(pRecList, INCOMMING, pMsg, pDest);

                //Length of msg
                msgLen = sizeof(pMsg);
#ifdef DEBUG
                pMsg[msgLen] = '\0';
                if(status == 1)
                {
                    printf("client side:\n%s Received a bad message, returning 'redirect message':\n'%s'\n",incClient,pMsg);
                }
                else
                {
                    printf("client side:\n%s Received message:\n'%s'\n",incClient,pMsg);
                }
#endif
            }


#ifdef DEBUG
            else
            {
                pMsg[sizeof(pMsg)] = '\0';
                printf("server side:\n%s Received a bad message, returning 'redirect message':\n'%s'\n",incClient,pMsg);
            }
#else
        }
#endif


            int sentLend = 0;

            //Write request to destination (send until we have sent sendLen number o f bytess)
            while(sentLend != msgLen)
            {
                if((sentLend += send(incClientFd, pMsg, msgLen, 0)) == -1)
                {
                    perror("client side: Error sending to destination");
                }
            }

            //Free anything that might be remaining before moving on
            if(pMsg)
                free(pMsg);
            if(pDest)
                free(pDest);
            clear_list(pRecList);


        }

    }
}

int dest_connect(char dest[], char destClient[INET6_ADDRSTRLEN])
{
    int 		destFd;
    struct 		addrinfo hints, *pServInfo, *pIter;

    //Set destination information
    memset(&hints, 0, sizeof(hints));
    hints.ai_family 	= AF_UNSPEC;
    hints.ai_socktype 	= SOCK_STREAM;

    //Try retrive address information
    int resp;
    if((resp = getaddrinfo(dest, HTTPPORT, &hints, &pServInfo))!= 0)
    {
#ifdef DEBUG
        //Error while tyring to retrieve address information, print error and end the program with status 1
        fprintf(stderr, "client side: Error in getaddrinfo: %s\n", gai_strerror(resp));
#endif
        return -1;
    }

    //We got the address info, try opening the socket
    for(pIter = pServInfo; pIter != NULL; pIter->ai_next)
    {
        //Try to open socket
        if((destFd = socket(pIter->ai_family, pIter->ai_socktype, pIter->ai_protocol)) == -1)
        {
            //If we could not open the socket, print error message and move on to next interation
            perror("client side: Could not open socket");
            continue;
        }

        //Socket is open, try connect
        if(connect(destFd, pIter->ai_addr, pIter->ai_addrlen) == -1)
        {
            //Unable to connect to socket
            close(destFd);
            perror("client side: Unable to connect to socket");
            continue;
        }

        //Socket open and connected, break the loop
        break;
    }

    //If we where not able to connect, return -1
    if (pIter == NULL)
    {
        fprintf(stderr, "client side: Failed to connect to destination");
        return -1;
    }

    //Get destination address information
    inet_ntop(pIter->ai_family, get_in_addr((struct sockaddr *)&pIter->ai_addr), destClient, sizeof(destClient));

    printf("client side: Connecting to %s\n", destClient);

    //Free what we are not using anymore
    freeaddrinfo(pServInfo);

    //Return descriptor to handler
    return destFd;
}

struct msg_list* receive_from_socket(int socketFd, char stopat)
{
    //Set flags to be non blocking (receiving from client) or blocking (receiving from destination)
    char flags;
    if(stopat == STOPATCLOSE)
        flags = 0;
    else
        flags = MSG_DONTWAIT;

    //Allocate first list entity
    struct msg_list* pRecList = (struct msg_list*) malloc(sizeof(struct msg_list));
    //Point to null as next
    pRecList->next = NULL;

    int recLen;
    char bufLen = MSGLISTSIZE;

    //Read out at most MSGLISTSIZE from socket (first call blocking as we might wait for a request)
    recLen = recv(socketFd, &pRecList->data[MSGLISTSIZE-bufLen], bufLen, 0);

    printf("got first msg\n");

    do
    {
        //If recList returns "EAGAIN" or "EWOULDBLOCK" we stop reading (this is only true if stopat is set)
        if((stopat == STOPATBLOCKING) && ((errno == EAGAIN) || (errno == EWOULDBLOCK)))
        {
            break;
        }
        else
        {
         printf("got second msg\n");
            //We continue to read
            pRecList->dataLen += recLen;

            //If we haev filled this segment, allocate a new one
            if(recLen == MSGLISTSIZE)
            {
                //Allocate new list segment
                struct msg_list* pNewSeg = (struct msg_list*) malloc(sizeof(struct msg_list));
                //Point to next previus segment
                pNewSeg->next = pRecList;
                //Change pRecList header
                pRecList = pNewSeg;

                bufLen = MSGLISTSIZE;
            }
            else
            {
                bufLen -= recLen;
            }
        }

        //If we are not reading in "unblocked" mode, break right away
        if(!stopat)
            break;

        //Read out at most MSGLISTSIZE from socket (non blocking)
        recLen = recv(socketFd, &pRecList->data[MSGLISTSIZE-bufLen], bufLen, flags);


    }
    while(1);


    //Return pointer
    return pRecList;
}

/*
	Processes the list content line by line and serializes the line to a processes char string.
	The processed char string is either a modified "good" message, or a redirect message.
	If returned with status 0, the message can go forward as planned.
	If returned with status 1, we have a redirect message due to "bad" request
*/
int process_list(struct msg_list* pList, char direction, char* pMsg, char* pDest)
{
printf("3\n");
    //Reg. Exp. for detecting "bad" words as well as "Host", "Connection" and "Length" lines in HTTP header
    regex_t regBadWord, regHost, regCon, regCont, regType;
    int retv;
    if((retv = regcomp(&regBadWord, ".* norrkoping|aftonbladet", REG_ICASE|REG_EXTENDED)))
    {
        printf("Reg exp comp. fail\n");
    }
    if((retv = regcomp(&regHost, "^Host. ", REG_ICASE|REG_EXTENDED)))
    {
        printf("Reg exp comp. fail\n");
    }
    if((retv = regcomp(&regCon, "Connection. [K|k]eep.[A|a]live", REG_ICASE|REG_EXTENDED)))
    {
        printf("Reg exp comp. fail\n");
    }
    if((retv = regcomp(&regCont, "Content-Type.*", REG_ICASE|REG_EXTENDED)))
    {
        printf("Reg exp comp. fail\n");
    }
    if((retv = regcomp(&regType, "Content-Type.* text.html|text.xml", REG_ICASE|REG_EXTENDED)))
    {
        printf("Reg exp comp. fail\n");
    }


    /*
    	First we need to get the total message length to allocate a appropriate char string.
    	We add 5 bytes to the length to make room for a possible modification of the connection type (+1 for null termination for printf purposes).
    */
    printf("1\n");
    int		listLen			= get_list_len(pList);
    printf("2\n");
    pMsg					= (char *) malloc(listLen + 6);

    // Length of the processed message
    int 	msgLen  		= 0;

    int 	lineLen 		= 0;
    char* 	pLine			= NULL;

    bool 	hostFound 		= false;
    bool 	conFound		= false;

    //The message content is considered being text until proven otherwise
    bool	textType		= true;

printf("read line, %u\n",msgLen);
    //Read out first line
    lineLen = get_line_from_list(pList, pLine);



printf("got line\n");
    //Read out lines from list until -1 is returned.
    while(lineLen != -1)
    {


printf("got line\n");
        //Check if the line contains the type of the content, and if the type is text type (if incomming)
        if((direction == INCOMMING) && !(regexec(&regCont, pLine, 0, NULL, 0)))
        {
            //Is the type ok?
            if(regexec(&regType, pLine, 0, NULL, 0))
            {
                textType = false;
            }
        }

        //Start with checking for "bad" words (only as long as we think the contents is textType)
        if(textType && !(regexec(&regBadWord, pLine, 0, NULL, 0)))
        {
            //A "bad" request detected, create a redirect message and send back to the client.
            if(pMsg)
                free(pMsg);


            pMsg = (char *) malloc(REDIRECTLEN);
            memcpy(pMsg, REDIRECTHEADER, REDIRECTLEN);

            //Return with status 1
            return 1;

        }
        //Check if it is the "Host" line (only if it is an outgoing request and we have not yet found the host information)
        else if((direction == OUTGOING) && !hostFound && !(regexec(&regHost, pLine, 0, NULL, 0)))
        {
            //Allocate the destination, length lineLen - 7 (Leading characters) + 1 (Null termination)
            pDest = (char *) malloc(lineLen + 6);

            //Extract host URL/destination (do not copy \r\n)
            memcpy(pDest,&pLine[6],lineLen-8);
            //Null terminate the "dest" string
            pDest[lineLen-7] = '\0';

            //Write to msg
            memcpy(&pMsg[msgLen],pLine,lineLen);
            msgLen += lineLen;

#ifdef DEBUG
            printf("Host line found in message: %s\n", pDest);
#endif

            hostFound = true;
        }
        //Check if it is the "Connection" line
        else if(!conFound && !(regexec(&regCon, pLine, 0, NULL, 0)))
        {
            //Set "Connection: close" if outgoing message
            if(direction == OUTGOING)
            {
                memcpy(&pMsg[msgLen],"Connection: close\r\n",19);
                msgLen += 19;
            }
            else if(direction == INCOMMING)
            {
                //Set "Connection: keep-alive" if incomming
                memcpy(&pMsg[msgLen],"Connection: keep-alive\r\n",24);
                msgLen += 24;
            }

#ifdef DEBUG
            printf("Connection line found in message: %s\n", pLine);
#endif
        }
        else
        {
            //Nothing special, just copy line to msg buffer
            memcpy(&pMsg[msgLen],pLine,lineLen);
            msgLen += lineLen;
        }

        //Free current allocated line before we move on
        if(pLine)
            free(pLine);

printf("read line 2\n");
        //Read out next line from buffer
        lineLen = get_line_from_list(pList, pLine);
        printf("read line\n");
    }


    // Free regexs
    regfree(&regBadWord);
    regfree(&regCon);
    regfree(&regCont);
    regfree(&regType);
    regfree(&regHost);



    //If we reach here, we can continue on with sending the message
    return 0;
}

/*
	Calcualte and return the length of the received message
*/
int get_list_len(struct msg_list* pList)
{
    int len = 0;
printf("1\n");

    while(pList != NULL)
    {
        len += pList->dataLen;
        pList = pList->next;
    }
printf("2\n");
    return len;
}

/*
	Read out line from list buffer
*/
int get_line_from_list(struct msg_list* pList, char* pLine)
{
    // Search for long the next line is the list is
    int i = 0;
    // How far we are in the line segment data buffer
    int segI = 0;
    do
    {
        if(pList->data[i] == '\n');
        break;

        i++;
        segI++;

        // If we reached end of this segment, move on to next
        if(pList->dataLen == i)
        {
            // free list segment and move on
            struct msg_list* pListT;
            pListT 	= pList;
            pList 	= pList->next;
            if(pListT)
                free(pListT);
            segI = 0;

        }

    }
    while(pList);
    // Add one for "correct" length
    i++;

    // Nothing to extract
    if(i == 0)
        return -1;

    // Allocate the line
    pLine = (char *) malloc(i);

    // Read out the line from the list
    int j;
    for(j = 0; j < i; j++)
    {
        pLine[j] = pList->data[i];

    }

    // Shift list buffet to remove the line
    memmove(pList->data,&pList->data[segI],MSGLISTSIZE-segI);

    return i;
}

void clear_list(struct msg_list* pList)
{
    struct msg_list* pListT;

    while(pList)
    {
        pListT = pList;
        pList  = pList->next;
        if(pListT)
            free(pListT);
    }
}
