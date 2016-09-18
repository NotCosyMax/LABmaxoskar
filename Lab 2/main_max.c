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

#define SERVBACKLOG 20

#define DEBUG

#define REDIRECTHEADER "HTTP/1.1 302 Found\r\nDate: Thu, 01 Sep 2016 16:00:52 GMT\r\nServer: Apache\r\nLocation: http://www.ida.liu.se/~TDTS04/labs/2011/ass2/error2.html\r\nContent-Length: 317\r\nConnection: close\r\nContent-Type: text/html; charset=iso-8859-1\r\n\r\n<!DOCTYPE HTML PUBLIC '-//IETF//DTD HTML 2.0//EN'>\n<html><head>\n<title>302 A bad BAD page</title>\n</head><body>\n<h1>You tries to access a bad BAD page</h1>\n<p>For more information regarding what is a BAD site, click <a href=http://www.ida.liu.se/~TDTS04/labs/2011/ass2/error2.html>here</a>.</p>\n</body></html>\n"

#define REDIRECTLEN 561

#define STOPATCLOSE 0
#define STOPATBLOCKING 1
#define MSGLISTSIZE 2000

#define OUTGOING	0
#define INCOMMING 	1

struct msg_list
{
    char 			 	data[MSGLISTSIZE];
    int		dataLen;
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

char* receive_from_socket(int socketFd, char direction, int* msgLen);

int process_msg(char** pMsg, int* msgLen, char** pDestInc, char direction);

char* get_line_from_buffer(char* pMsg, int* maxLen, int* lineLen);

int get_header_len(char *pMsg,int* msgLen);

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

    // Message buffer pointer
    char* 	pMsg;
    int 	msgLen;

    // Incomming client main loop
    // TODO: End when final respons is sent (think keep-alive);
    while(1)
    {
        printf("waiting for new..\n");

        msgLen = 0;

        // Read from socket (non blocking)
        pMsg = receive_from_socket(incClientFd, OUTGOING, &msgLen);
#ifdef DEBUG
        printf("server side: received msg with length: %u\n", msgLen);
#endif
        // If recLen is zero, the client closed the connection, end!
        if(!msgLen)
        {
#ifdef DEBUG
            printf("server side: incomming socket closed.\n");
#endif
            break;
        }

        printf("process shoit %u\n", pMsg);
        int status;
        if(pMsg)
            status = process_msg(&pMsg,&msgLen,&pDest,OUTGOING);


        pMsg[msgLen] = '\0';
<<<<<<< HEAD
     //   printf("Test: len %u, message\n %s\n",msgLen,pMsg);
=======
        printf("Test: len %u, message\n %s\n",msgLen,pMsg);
>>>>>>> c1cbc448c6b62317a102dfb543ce1039b0fdca21

#ifdef DEBUG
        //   printf("server side: processed msg, status %u, length %u\n", status, msgLen);
#endif


        //If not a redirect, procceed
        if(status == 0)
        {

#ifdef DEBUG
          //  printf("server side:\n%s received msg:\n'%s'\n",incClient,pMsg);
#endif

            //Open socket to the destination
            destFd = dest_connect(pDest, destClient);

            //If we managed to open the socket, write the modified message to the destination
            if(destFd != -1)
            {

                if(pMsg)
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
                }


                //Free pMsg and pDest
                if(pMsg)
                {
                    free(pMsg);
                    pMsg = NULL;
                }
                if(pDest)
                {
                    free(pDest);
                    pDest = NULL;
                }

                msgLen = 0;
                // Read from socket (non blocking)
                pMsg = receive_from_socket(destFd, INCOMMING, &msgLen);

                int status;
                if(pMsg)
                    status = process_msg(&pMsg,&msgLen,&pDest,INCOMMING);

 #ifdef DEBUG
                if(0 == 1)
                {
<<<<<<< HEAD
                    //printf("client side:\n%s Received a bad message, returning 'redirect message':\n''\n",destClient,pMsg);
=======
                    printf("client side:\n%s Received a bad message, returning 'redirect message':\n''\n",destClient,pMsg);
>>>>>>> c1cbc448c6b62317a102dfb543ce1039b0fdca21
                }
                else
                {
                    //printf("client side:\n%s Received message:\n'%s'\n",destClient,pMsg);
                }
#endif
            }


#ifdef DEBUG
            else
            {
              //  printf("server side:\n%s Received a bad message, returning 'redirect message':\n'%s'\n",destClient,pMsg);
            }
#else
        }
#endif
            }
        if(pMsg)
        {
            int sentLend = 0;
            // msgLen = strlen(pMsg);
              printf("sending n: %u bytes\n",msgLen);
            //Write request to destination (send until we have sent sendLen number o f bytess)
            while(sentLend != msgLen)
            {
                if((sentLend += send(incClientFd, pMsg, msgLen, 0)) == -1)
                {
                    perror("client side: Error sending to destination");
                }
            }
        }

        //Free anything that might be remaining before moving on
        //Free pMsg and pDest
        if(pMsg)
        {
            free(pMsg);
            pMsg = NULL;
        }
<<<<<<< HEAD
=======
        if(pDest)
        {
            free(pDest);
            pDest = NULL;
        }

>>>>>>> c1cbc448c6b62317a102dfb543ce1039b0fdca21

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

char* receive_from_socket(int socketFd, char direction, int* msgLen)
<<<<<<< HEAD
{

    //Allocate reclist entity
    struct msg_list* pRecList = (struct msg_list*) malloc(sizeof(struct msg_list));
    struct msg_list* pCurListSeg = pRecList;

    //Point to null as next
    pCurListSeg->next = NULL;
    pCurListSeg->dataLen = 0;

    int recLen = 0;
    int tmpLen = 0;
    int bufLen = MSGLISTSIZE;

    char tmpBuf[MSGLISTSIZE];

    //Read out at most MSGLISTSIZE from socket (first call blocking as we might wait for a request)

    tmpLen = recv(socketFd, tmpBuf, bufLen, 0);

    memcpy(pCurListSeg->data,tmpBuf,tmpLen);

    if(tmpLen == 0)
        return NULL;

    do
    {

        recLen += tmpLen;

        //We continue to read
        pCurListSeg->dataLen += tmpLen;

        int dataLen = pCurListSeg->dataLen;
        //Check for double termination
        if((direction == OUTGOING) && (pCurListSeg->data[dataLen-4] == '\r') &&
                (pCurListSeg->data[dataLen-3] == '\n') &&
                (pCurListSeg->data[dataLen-2] == '\r') &&
                (pCurListSeg->data[dataLen-1] == '\n'))
            break;

        //If we haev filled this segment, allocate a new one
        if(pCurListSeg->dataLen == MSGLISTSIZE)
        {
            //Allocate new list segment
            struct msg_list* pNewSeg = (struct msg_list*) malloc(sizeof(struct msg_list));
            //Point to next previus segment
            pNewSeg->next = NULL;
            pNewSeg->dataLen = 0;
            //Change pRecList header
            pCurListSeg->next = pNewSeg;
            pCurListSeg = pNewSeg;

            bufLen = MSGLISTSIZE;
        }
        else
        {
            bufLen -= tmpLen;
        }


        //Read out at most MSGLISTSIZE from socket (non blocking)
        tmpLen = recv(socketFd, tmpBuf, bufLen, 0);

        memcpy(&pCurListSeg->data[MSGLISTSIZE-bufLen],tmpBuf,tmpLen);

        //If we are not reading in "unblocked" mode, break right away when we recLen = 0;
        if(tmpLen == 0)
            break;

    }
    while(1);

    //Message received, make it linear bitch!
    char* pMsg = (char *) malloc(recLen);
    pCurListSeg = pRecList;

    //Copy over to line buffer
    int i;
    int segI = 0;
    for(i = 0; i < recLen; i++)
    {
        pMsg[i] = pCurListSeg->data[segI];

        segI++;

        if(segI == pCurListSeg->dataLen)
        {
            struct msg_list* tmp = pCurListSeg;
            pCurListSeg = pCurListSeg->next;
            free(tmp);
            segI = 0;
        }
    }

    *msgLen = recLen;

    //Return pointer
    return pMsg;
}

/*
	Processes the list content line by line and serializes the line to a processes char string.
	The processed char string is either a modified "good" message, or a redirect message.
	If returned with status 0, the message can go forward as planned.
	If returned with status 1, we have a redirect message due to "bad" request
*/

bool test = false;

int process_msg(char** pMsg, int* msgLen, char** pDestInc, char direction)
{

=======
{

    //Allocate reclist entity
    struct msg_list* pRecList = (struct msg_list*) malloc(sizeof(struct msg_list));
    struct msg_list* pCurListSeg = pRecList;

    //Point to null as next
    pCurListSeg->next = NULL;
    pCurListSeg->dataLen = 0;

    int recLen = 0;
    int tmpLen = 0;
    int bufLen = MSGLISTSIZE;

    char tmpBuf[MSGLISTSIZE];

    //Read out at most MSGLISTSIZE from socket (first call blocking as we might wait for a request)

    tmpLen = recv(socketFd, tmpBuf, bufLen, 0);

    memcpy(pCurListSeg->data,tmpBuf,tmpLen);

    if(tmpLen == 0)
        return NULL;

    do
    {

        recLen += tmpLen;

        //We continue to read
        pCurListSeg->dataLen += tmpLen;

        int dataLen = pCurListSeg->dataLen;
        //Check for double termination
        if((direction == OUTGOING) && (pCurListSeg->data[dataLen-4] == '\r') &&
                (pCurListSeg->data[dataLen-3] == '\n') &&
                (pCurListSeg->data[dataLen-2] == '\r') &&
                (pCurListSeg->data[dataLen-1] == '\n'))
            break;

        //If we haev filled this segment, allocate a new one
        if(pCurListSeg->dataLen == MSGLISTSIZE)
        {
            //Allocate new list segment
            struct msg_list* pNewSeg = (struct msg_list*) malloc(sizeof(struct msg_list));
            //Point to next previus segment
            pNewSeg->next = NULL;
            pNewSeg->dataLen = 0;
            //Change pRecList header
            pCurListSeg->next = pNewSeg;
            pCurListSeg = pNewSeg;

            bufLen = MSGLISTSIZE;
        }
        else
        {
            bufLen -= tmpLen;
        }


        //Read out at most MSGLISTSIZE from socket (non blocking)
        tmpLen = recv(socketFd, tmpBuf, bufLen, 0);

        memcpy(&pCurListSeg->data[MSGLISTSIZE-bufLen],tmpBuf,tmpLen);

        //If we are not reading in "unblocked" mode, break right away when we recLen = 0;
        if(tmpLen == 0)
            break;

    }
    while(1);

    //Message received, make it linear bitch!
    char* pMsg = (char *) malloc(recLen);
    pCurListSeg = pRecList;

    //Copy over to line buffer
    int i;
    int segI = 0;
    for(i = 0; i < recLen; i++)
    {
        pMsg[i] = pCurListSeg->data[segI];

        segI++;

        if(segI == pCurListSeg->dataLen)
        {
            struct msg_list* tmp = pCurListSeg;
            pCurListSeg = pCurListSeg->next;
            free(tmp);
            segI = 0;
        }
    }

    *msgLen = recLen;

    //Return pointer
    return pMsg;
}

/*
	Processes the list content line by line and serializes the line to a processes char string.
	The processed char string is either a modified "good" message, or a redirect message.
	If returned with status 0, the message can go forward as planned.
	If returned with status 1, we have a redirect message due to "bad" request
*/

bool test = false;

int process_msg(char** pMsg, int* msgLen, char** pDestInc, char direction)
{

>>>>>>> c1cbc448c6b62317a102dfb543ce1039b0fdca21
    /*
    	First we need to get the total message length to allocate a appropriate char string.
    	We add 5 bytes to the length to make room for a possible modification of the connection type (+1 for null termination for printf purposes).
    */
    //printf("messagelenght: %u\n", *msgLen);
    char*   pNewMsg			= (char *) malloc(*msgLen + 6);
    int     newMsgLen       = 0;
    char*   pDest           = NULL;
    char*   pLine           = NULL;
    int     lineLen         = 0;
    char**  header          = NULL;

    //Reg. Exp. for detecting "bad" words as well as "Host", "Connection" and "Length" lines in HTTP header
    regex_t regBW, regHost, regCon, regConTest, regCont, regType;
    int retv;

    if((retv = regcomp(&regType, "[C|c]ontent.[T|t]ype.* text", REG_ICASE|REG_EXTENDED)))
    {
        printf("Reg exp comp. fail\n");
    }
<<<<<<< HEAD
    if((retv = regcomp(&regBW, "[N|n]orrk.*ping|aftonbladet|[S|s]ponge[B|b]ob|(Britney Spears)|(Paris Hilton)", REG_ICASE|REG_EXTENDED)))
=======
    if((retv = regcomp(&regBW, "norrk.ping|aftonbladet|SpongeBob|(Britney Spears)|(Paris Hilton)", REG_ICASE|REG_EXTENDED)))
>>>>>>> c1cbc448c6b62317a102dfb543ce1039b0fdca21
    {
        printf("Reg exp comp. fail\n");
    }
    if((retv = regcomp(&regHost, "Host. ", REG_ICASE|REG_EXTENDED)))
    {
        printf("Reg exp comp. fail\n");
    }
    if((retv = regcomp(&regCon, "Connection. [K|k]eep.[A|a]live", REG_ICASE|REG_EXTENDED)))
    {
        printf("Reg exp comp. fail\n");
    }
    if((retv = regcomp(&regConTest, "Connection. ", REG_ICASE|REG_EXTENDED)))
    {
        printf("Reg exp comp. fail\n");
    }
    if((retv = regcomp(&regCont, "[C|c]ontent.[T|t]ype", REG_ICASE|REG_EXTENDED)))
    {
        printf("Reg exp comp. fail\n");
    }

    bool 	hostFound 		= false;
    bool 	conFound		= false;

    //The message content is considered being text until proven otherwise
    bool	textType		= true;
    int headerLen           = 0;


    do
    {
<<<<<<< HEAD
        regmatch_t matchE;

        //Read out line from msg buffer to be processed
        pLine = get_line_from_buffer(*pMsg,msgLen,&lineLen);

=======
        printf("get line %u %u\n",*msgLen,*pMsg);
        //Read out line from msg buffer to be processed
        pLine = get_line_from_buffer(*pMsg,msgLen,&lineLen);

        printf("line gotten\n");

>>>>>>> c1cbc448c6b62317a102dfb543ce1039b0fdca21
        if(lineLen == 0)
            break;

        int match;

        if((direction == INCOMMING) && !(match = regexec(&regCont, pLine, 0, NULL, 0)))
        {

            //Is the type ok?
            if(regexec(&regType, pLine, 0, NULL, 0))
            {
                textType = false;
            }
        }

        if((direction == OUTGOING) && !(match = regexec(&regBW, pLine, 0, NULL, 0)))
        {
            printf("bad word\n");
<<<<<<< HEAD


=======
>>>>>>> c1cbc448c6b62317a102dfb543ce1039b0fdca21
            //A "bad" request detected, create a redirect message and send back to the client.
            if(pNewMsg)
                free(pNewMsg);
            if(*pMsg)
                free(*pMsg);

            pNewMsg = (char *) malloc(REDIRECTLEN);
            memcpy(pNewMsg, REDIRECTHEADER, REDIRECTLEN);

            *msgLen = REDIRECTLEN;
            *pMsg = pNewMsg;
            printf("redirect\n");
            //Return with status 1
            return 1;
        }
        //Check if it is the "Host" line (only if it is an outgoing request and we have not yet found the host information)
        else if((direction == OUTGOING) && !hostFound && !(match = regexec(&regHost, pLine, 0, NULL, 0)))
        {

            //Allocate the destination, length lineLen -  8(Leading characters) + 1 (Null termination)
            pDest = (char *) malloc(lineLen - 7);

            //Extract host URL/destination (do not copy \r\n)
            memcpy(pDest,&pLine[6],lineLen-8);
            //Null terminate the "dest" string
            pDest[lineLen-8] = '\0';
            *pDestInc = pDest;

            //Write to msg
            memcpy(&pNewMsg[newMsgLen],pLine,lineLen);
            newMsgLen += lineLen;

#ifdef DEBUG
            printf("Host line found in message: %s\n", pDest);
#endif

            hostFound = true;
        }
        //Check if it is the "Connection" line
        else if((direction == OUTGOING) && !conFound && !(match = regexec(&regConTest, pLine, 0, NULL, 0)))
        {

            //Set "Connection: close" if outgoing message
            if(direction == OUTGOING)
            {
                if(!(match = regexec(&regCon, pLine, 0, NULL, 0)))
                {
                    memcpy(&pNewMsg[newMsgLen],"Connection: close\r\n",19);
                    newMsgLen += 19;
                    test = true;
                }
                else
                {
                    memcpy(&pNewMsg[newMsgLen],pLine,lineLen);
                    newMsgLen += lineLen;
                }
            }
            else if(direction == INCOMMING)
            {
                if(test)
                {
                    //Set "Connection: keep-alive" if incomming
                    memcpy(&pNewMsg[newMsgLen],"Connection: keep-alive\r\n",24);
                    newMsgLen += 24;
                }
                else
                {
                    memcpy(&pNewMsg[newMsgLen],pLine,lineLen);
                    newMsgLen += lineLen;
                }


            }

#ifdef DEBUG
            printf("Connection line found in message\n");
#endif
        }
        else
        {
            //Nothing special, just copy line to msg buffer
            memcpy(&pNewMsg[newMsgLen],pLine,lineLen);
            newMsgLen += lineLen;
        }

        if((direction == INCOMMING) && (lineLen == 2) && (strcmp(pLine,"\r\n") == 0))
        {
            if(textType)
            {
                if(!(match = regexec(&regBW, *pMsg, 0, NULL, 0)))
                {
                    printf("bad word\n");
<<<<<<< HEAD



=======
>>>>>>> c1cbc448c6b62317a102dfb543ce1039b0fdca21
                    //A "bad" request detected, create a redirect message and send back to the client.
                    if(pNewMsg)
                        free(pNewMsg);
                    if(*pMsg)
                        free(*pMsg);

                    pNewMsg = (char *) malloc(REDIRECTLEN);
                    memcpy(pNewMsg, REDIRECTHEADER, REDIRECTLEN);

                    *msgLen = REDIRECTLEN;
                    *pMsg = pNewMsg;
                    printf("redirect\n");
                    //Return with status 1
                    return 1;
                }
                else
                {
                                free(pLine);
                memcpy(&pNewMsg[newMsgLen],*pMsg,*msgLen);
                newMsgLen += *msgLen;
                break;
                }
            }
            else
            {
                free(pLine);
                memcpy(&pNewMsg[newMsgLen],*pMsg,*msgLen);
                newMsgLen += *msgLen;
                break;
            }

        }

        if(pLine)
            free(pLine);

    }
    while(1);

    printf("done\n");

    *msgLen = newMsgLen;
    free(*pMsg);
    *pMsg = pNewMsg;

    // Free regexs
    regfree(&regBW);
    regfree(&regCon);
    regfree(&regConTest);
    regfree(&regCont);
    regfree(&regType);
    regfree(&regHost);

    //If we reach here, we can continue on with sending the message
    return 0;
}


/*
	Read out line from buffer
*/
char* get_line_from_buffer(char* pMsg, int* maxLen, int* lineLen)
{

    //Count how long the line is
    if(*maxLen > 0)
    {
        int i = 1;
        while(i < *maxLen)
        {
            if(pMsg[i] == '\n')
                break;

            i++;
        }

        i++;
        //Extract the line
        char* tmp;

        tmp = (char *) malloc(i+1);
        memcpy(tmp,pMsg,i);

#ifdef DEBUG
        tmp[i] = '\0';
        //printf("Line found: %s\n",tmp);
#endif


        //Remove extracted data from message
        memmove(pMsg,&pMsg[i],*maxLen-i);
        *maxLen -= i;

        *lineLen = i;

        return tmp;
    }
    *lineLen = 0;
    return NULL;

}
