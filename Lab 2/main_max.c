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

/*
 DEFINES
 **************************************************
 */
// Default port if none is given by the users
#define DEFAULTPORT "3123"
#define HTTPPORT    "80"

// Number of active connections to proxy
#define SERVBACKLOG 20

// Verbose debugging
//#define DEBUG

// Redirect HTTP message and length defines

const char REDIRECTHEADER[] = "HTTP/1.1 302 Found\r\nLocation: http://www.ida.liu.se/~TDTS04/labs/2011/ass2/error2.html\r\nContent-Length: 310\r\nConnection: close\r\nContent-Type: text/html; charset=iso-8859-1\r\n\r\n<!DOCTYPE HTML PUBLIC '-//IETF//DTD HTML 2.0//EN'>\n<html><head>\n<title>302 A bad BAD page</title>\n</head><body>\n<h1>You tries to access a bad BAD page</h1>\n<p>For more information regarding what is a BAD site, click <a href=http://www.ida.liu.se/~TDTS04/labs/2011/ass2/error2.html>here</a>.</p>\n</body></html>\n";
const int REDIRECTLEN  = 485;

// Size of linked list segment
#define MSGLISTSIZE 2000

// Direction of the message
#define OUTGOING	0
#define INCOMMING 	1

/*
 Struct defines
 **************************************************
 */
struct msg_list
{
    // Data block
    char 			 	data[MSGLISTSIZE];
    // Data lenght
    int		            dataLen;
    // Next list section
    struct msg_list* 	next;
};

/*
 Signal handler and IP macro
 **************************************************
 */

// Signal handler
// Source: http://beej.us/guide/bgnet/output/html/multipage/clientserver.html#simpleserver
void sigchld_handler(int s)
{
    // waitpid() might overwrite errno, so we save and restore it:
    int saved_errno = errno;

    while(waitpid(-1, NULL, WNOHANG) > 0);

    errno = saved_errno;
}

// get sockaddr, IPv4 or IPv6:
// Source: http://beej.us/guide/bgnet/output/html/multipage/clientserver.html#simpleserver
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET)
    {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

/*
 Function prototypes
 **************************************************
 */

// Incomming client (client -> server) handler function.
void inc_client_handler(int incClientFd, char incClient[INET6_ADDRSTRLEN]);

// Read message from socket to a linked list and returns the received message in a serialized C string.
char* receive_from_socket(int socketFd, char direction, int* msgLen);

// Process the received message: Extract host. Change connection type. Check for "bad" keywords
int process_msg(char** pMsg, int* msgLen, char** pDestInc, char direction, bool* pModCon);

// Extract a single line from a C string.
char* get_line_from_buffer(char* pMsg, int* maxLen, int* lineLen);

// Open and connect a socket to the destination
int dest_connect(char* dest, char destClient[INET6_ADDRSTRLEN]);

int main(int argc, char** argv)
{
    // Variables
    int 		servFd, incClientFd;
    char 		*pServPort;
    struct 		addrinfo hints, *pServInfo, *pIter;
    struct 		sockaddr_storage incClientAddr;
    socklen_t 	addrLen;
    char 		incClient[INET6_ADDRSTRLEN];

    // To clear zombies
    struct sigaction sa;

    // Retrive and check input port arguments
    if((argc > 1) && (atoi(argv[1]) > 1024))
    {
        // Convert port argument to string
        int sizeOfPort = sizeof(atoi(argv[1]));
        pServPort = (char *) malloc(sizeOfPort + 1);
        memcpy(pServPort,argv[1],sizeOfPort);
        pServPort[sizeOfPort] = '\0';
        printf("User port provided: %u\n", atoi(pServPort));
    }
    else
    {
        // Set defualt port
        int sizeOfPort = sizeof(atoi(DEFAULTPORT));
        pServPort = (char *) malloc(sizeOfPort+1);
        memcpy(pServPort,DEFAULTPORT,sizeOfPort);
        pServPort[sizeOfPort] = '\0';
        printf("Invalid or no port provided, using defualt port %u\n",atoi(DEFAULTPORT));
    }


    // Set server side socket info to stream socket with the local IP
    memset(&hints, 0, sizeof(hints));
    hints.ai_family 	= AF_UNSPEC;
    hints.ai_socktype 	= SOCK_STREAM;
    hints.ai_flags 		= AI_PASSIVE;

    // Try retrive address information
    int resp;
    if((resp = getaddrinfo(NULL, pServPort, &hints, &pServInfo))!= 0)
    {
        // Error while tyring to retrieve address information, print error and end the program with status 1
        fprintf(stderr, "server side: Error in getaddrinfo: %s\n", gai_strerror(resp));
        return 1;
    }

    // We got the address info, try opening the socket
    for(pIter = pServInfo; pIter != NULL; pIter->ai_next)
    {
        // Try to open socket
        if((servFd = socket(pIter->ai_family, pIter->ai_socktype, pIter->ai_protocol)) == -1)
        {
            // If we could not open the socket, print error message and move on to next interation
            perror("server side: Could not open socket");
            continue;
        }

        // Socket is open, set socket options
        int yes = 1;
        if(setsockopt(servFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1)
        {
            // Unable to set socket options, exit the program with status 1
            perror("server side: Unable to set socket options");
            exit(1);
        }

        // Socket open and options is set, try bind to socket
        if(bind(servFd, pIter->ai_addr, pIter->ai_addrlen) == -1)
        {
            // Unable to bind to the socket, close the socket and move on to next iteration
            close(servFd);
            perror("server side: Unable to bind to the socket");
            continue;
        }

        // Socket open and binded, break the loop
        break;
    }

    // We can now free the server address info as we have the socket open
    freeaddrinfo(pServInfo);

    // If we managed to bind no socket we end the program with status 1
    if(pIter == NULL)
    {
        fprintf(stderr, "server side: Failed dto bind to socket\n");
        exit(1);
    }

    // Try to start listen to the socket, if not able, exit the program with status 1
    if(listen(servFd, SERVBACKLOG) == -1)
    {
        perror("server side: Unable to s tart listening to socket");
        exit(1);
    }

    // Before entering server side loop, clean zombie processes
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1)
    {
        perror("Error cleaning up dead processes: sigaction");
        exit(1);
    }

    printf("server side: Waiting for connections...\n");

    // Server side main loop
    while(1)
    {
        addrLen 	= sizeof(incClientAddr);
        // Wait for a new connection
        incClientFd = accept(servFd, (struct sockaddr *)&incClientAddr, &addrLen);
        if(incClientFd == -1)
        {
            // Unable to accept the connection, move on
            perror("server side: Unable to accept incomming client connection");
            continue;
        }

        // Connection accepted, retreive client information
        inet_ntop(incClientAddr.ss_family,get_in_addr((struct sockaddr *)&incClientAddr),incClient, sizeof(incClient));
        printf("server side: Connection accepted from %s\n",incClient);

        // Fork client handler child processes
        if(!fork())
        {
            // Run incomming client handler
            inc_client_handler(incClientFd, incClient);

            printf("server side: Ending %s child process.",incClient);
            close(incClientFd);
            exit(0);
        }
    }

    free(pServPort);

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

    // Message connection type was modified
    bool modCon = false;

    // Incomming client main loop
    while(1)
    {
        // Has the connection type of the received message been modified?
        modCon = false;

#ifdef DEBUG
        printf("Waiting for new connection..\n");
#endif
        // Set initial msg length to zero
        msgLen = 0;

        // Read from local client socket
        pMsg = receive_from_socket(incClientFd, OUTGOING, &msgLen);

#ifdef DEBUG
        printf("server side: received msg with length: %u\n", msgLen);
#endif
        // If recLen is zero, the client closed the connection -> end it all!
        if(!msgLen)
        {
#ifdef DEBUG
            printf("server side: incomming socket closed.\n");
#endif
            break;
        }

        // Process the incomming message. Check for "bad words", extract destination host and change connection type to close if needed.
        int status;
        if(pMsg)
            status = process_msg(&pMsg,&msgLen,&pDest,OUTGOING,&modCon);

#ifdef DEBUG
        pMsg[msgLen] = '\0';
        printf("server side: processed msg (status %u, length %u):\n%s\n", status, msgLen,pMsg);
#endif


        // If status indicates that we are not performing a redirect, procceed with sending the message to the destination.
        if(status == 0)
        {

            // Open socket to the destination
            destFd = dest_connect(pDest, destClient);

            // If we managed to open the socket, write the message to the destination
            if(destFd != -1)
            {

                if(pMsg)
                {
                    // Number of bytes sent
                    int sentLend = 0;
                    // Write request to destination (send until we have sent sendLen number of bytes)
                    while(sentLend != msgLen)
                    {
                        if((sentLend += send(destFd, pMsg, msgLen, 0)) == -1)
                        {
                            perror("client side: Error sending to destination");
                        }
                    }
                }


                // Free pMsg and pDest
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

                // set msgLen to zero before receiving
                msgLen = 0;
                // Read from destination socket
                pMsg = receive_from_socket(destFd, INCOMMING, &msgLen);

                // Process recevied message
                int status;
                if(pMsg)
                    status = process_msg(&pMsg,&msgLen,&pDest,INCOMMING,&modCon);

#ifdef DEBUG
                pMsg[msgLen] = '\0';
                if(status == 1)
                {
                    printf("client side:\n%s Received a bad message, returning 'redirect message':\n''\n",destClient,pMsg);
                }
                else
                {
                    printf("client side:\n%s Received message:\n'%s'\n",destClient,pMsg);
                }
                else
                {
                    printf("server side:\n%s Received a bad message, returning 'redirect message':\n'%s'\n",destClient,pMsg);
                }
#else
            }
#endif

            }
            if(pMsg)
            {
                int sentLend = 0;

                // Write message to destination (send until we have sent sendLen number of bytes)
                while(sentLend != msgLen)
                {
                    if((sentLend += send(incClientFd, pMsg, msgLen, 0)) == -1)
                    {
                        perror("client side: Error sending to destination");
                    }
                }
            }

            //Free anything that might be remaining before moving on
            if(pMsg)
            {
                free(pMsg);
                pMsg = NULL;
            }

        }
    }

    /*
    	Read message from socket to a linked list and returns the received message in a serialized C string.
    */
    char* receive_from_socket(int socketFd, char direction, int* msgLen)
    {

        // Allocate receive list entity
        struct msg_list* pRecList = (struct msg_list*) malloc(sizeof(struct msg_list));
        struct msg_list* pCurListSeg = pRecList;

        // Point to null as next
        pCurListSeg->next = NULL;
        pCurListSeg->dataLen = 0;

        int recLen = 0;
        int tmpLen = 0;
        int bufLen = MSGLISTSIZE;

        char tmpBuf[MSGLISTSIZE];

        // Read out at most MSGLISTSIZE from socket, reading in to temp buffer then copies it to the list.
        tmpLen = recv(socketFd, tmpBuf, bufLen, 0);
        memcpy(pCurListSeg->data,tmpBuf,tmpLen);

        // If tmpLen is zero, the connection is closed. return NULL.
        if(tmpLen == 0)
            return NULL;

        do
        {
            // Add tmpLen to total read length
            recLen += tmpLen;
            pCurListSeg->dataLen += tmpLen;

            //Check for double termination
            int dataLen = pCurListSeg->dataLen;
            if((direction == OUTGOING) && (pCurListSeg->data[dataLen-4] == '\r') &&
                    (pCurListSeg->data[dataLen-3] == '\n') &&
                    (pCurListSeg->data[dataLen-2] == '\r') &&
                    (pCurListSeg->data[dataLen-1] == '\n'))
                break;

            //If we have filled this segment, allocate a new one
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


            // Read out more from socket (at most bufLen)
            tmpLen = recv(socketFd, tmpBuf, bufLen, 0);
            // Copy the read data into the list
            memcpy(&pCurListSeg->data[MSGLISTSIZE-bufLen],tmpBuf,tmpLen);

            // If tmpLen is zero, the connection is closed. Break!
            if(tmpLen == 0)
                break;

        }
        while(tmpLen > 0);

        // Message received, make it into a C string!
        char* pMsg = (char *) malloc(recLen);
        pCurListSeg = pRecList;

        //Copy over to the C string
        int i;
        int segI = 0;
        for(i = 0; i < recLen; i++)
        {
            pMsg[i] = pCurListSeg->data[segI];

            segI++;

            // End of segment, remove and move on
            if(segI == pCurListSeg->dataLen)
            {
                struct msg_list* tmp = pCurListSeg;
                pCurListSeg = pCurListSeg->next;
                free(tmp);
                segI = 0;
            }
        }

        *msgLen = recLen;

        // Return pointer to the C string
        return pMsg;
    }

    /*
    	Processes the list content line by line and serializes the line to a processes char string.
    	The processed char string is either a modified "good" message, or a redirect message.
    	If returned with status 0, the message can go forward as planned.
    	If returned with status 1, we have a redirect message due to "bad" request
    */

    int process_msg(char** pMsg, int* msgLen, char** pDestInc, char direction, bool* pModCon)
    {

        /*
        	First we need to get the total message length to allocate a appropriate char string.
        	We add 5 bytes to the length to make room for a possible modification of the connection type (+1 for null termination for printf purposes).
        */

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
        if((retv = regcomp(&regBW, "[N|n]orrk.*ping|aftonbladet|[S|s]ponge[B|b]ob|(Britney Spears)|(Paris Hilton)", REG_ICASE|REG_EXTENDED)))
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

        // Have a specific line been found?
        bool 	hostFound 		= false;
        bool 	conFound		= false;

        // The message content is considered being text until proven otherwise
        bool	textType		= true;

        do
        {
            // Read out line from msg buffer to be processed
            pLine = get_line_from_buffer(*pMsg,msgLen,&lineLen);

            // If lineLen is zero, there is no more to read from the msg
            if(lineLen == 0)
                break;

            // Check the type if the content. If we have already discovered it is not text, we don't need to check.
            if(textType && (direction == INCOMMING) && !(regexec(&regCont, pLine, 0, NULL, 0)))
            {

                // Is the type text?
                if(regexec(&regType, pLine, 0, NULL, 0))
                {
                    // It was not text...
                    textType = false;
                }
            }

            if((direction == OUTGOING) && !(regexec(&regBW, pLine, 0, NULL, 0)))
            {

                printf("BAD KEYWORD FOUND! EXECUTING COUNTERMEASURES TO PREVENT BRAIN DECAY!\n");

                // A "bad" request detected, create a redirect message and send back to the client.
                if(pNewMsg)
                    free(pNewMsg);
                if(*pMsg)
                    free(*pMsg);
                // Allocate space for the redirect message
                pNewMsg = (char *) malloc(REDIRECTLEN);
                memcpy(pNewMsg, REDIRECTHEADER, REDIRECTLEN);
                // Set msgLen of the message
                *msgLen = REDIRECTLEN;
                // Change pMsg pointer
                *pMsg = pNewMsg;

                //Return with status 1
                return 1;
            }
            // Check if it is the "Host" line (only if it is an outgoing request and we have not yet found the host information)
            else if((direction == OUTGOING) && !hostFound && !(regexec(&regHost, pLine, 0, NULL, 0)))
            {

                // Allocate the destination, length lineLen -  8(Leading characters) + 1 (Null termination)
                pDest = (char *) malloc(lineLen - 7);

                // Extract host URL/destination (do not copy \r\n)
                memcpy(pDest,&pLine[6],lineLen-8);
                // Null terminate the "dest" string
                pDest[lineLen-8] = '\0';
                *pDestInc = pDest;

                // Write to msg
                memcpy(&pNewMsg[newMsgLen],pLine,lineLen);
                newMsgLen += lineLen;

#ifdef DEBUG
                printf("Host line found in message: %s\n", pDest);
#endif

                hostFound = true;
            }
            // Check if it is the "Connection" line
            else if((direction == OUTGOING) && !conFound && !(regexec(&regConTest, pLine, 0, NULL, 0)))
            {


                if(direction == OUTGOING)
                {
                    // Set "Connection: close" if outgoing message
                    if(!(regexec(&regCon, pLine, 0, NULL, 0)))
                    {
                        memcpy(&pNewMsg[newMsgLen],"Connection: close\r\n",19);
                        newMsgLen += 19;
                        *pModCon = true;
                    }
                    // Else, just copy the line
                    else
                    {
                        memcpy(&pNewMsg[newMsgLen],pLine,lineLen);
                        newMsgLen += lineLen;
                    }
                }
                else if(direction == INCOMMING)
                {
                    // Set "Connection: keep-alive" if incomming and previus modified
                    if(*pModCon)
                    {
                        memcpy(&pNewMsg[newMsgLen],"Connection: keep-alive\r\n",24);
                        newMsgLen += 24;
                    }
                    // Else, just copy the line
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
                // Just copy line to msg buffer
                memcpy(&pNewMsg[newMsgLen],pLine,lineLen);
                newMsgLen += lineLen;
            }

            // If it is a incomming message with content, we stop reading out line by line after the header (ends with a 2byte line "\r\n").
            // On the content we instead perform checking on the whole content (if text type), and if ok (or not text type)
            // we copy the remaining part of the message.

            if((direction == INCOMMING) && (lineLen == 2) && (strcmp(pLine,"\r\n") == 0))
            {

                if(textType)
                {
                    // Content is text type, check for "bad" words
                    if(!(regexec(&regBW, *pMsg, 0, NULL, 0)))
                    {
                        printf("BAD KEYWORD FOUND! EXECUTING COUNTERMEASURES TO PREVENT BRAIN DECAY!\n");

                        // A "bad" message detected, create a redirect message and send back to the client.
                        if(pNewMsg)
                            free(pNewMsg);
                        if(*pMsg)
                            free(*pMsg);

                        // Allocate space for the redirect message
                        pNewMsg = (char *) malloc(REDIRECTLEN);
                        memcpy(pNewMsg, REDIRECTHEADER, REDIRECTLEN);
                        // Set msgLen of the message
                        *msgLen = REDIRECTLEN;
                        // Change pMsg pointer
                        *pMsg = pNewMsg;

                        // Free the line before return
                        free(pLine);

                        // Return with status 1
                        return 1;
                    }
                    // Content is ok, copy everything and break
                    else
                    {
                        free(pLine);
                        memcpy(&pNewMsg[newMsgLen],*pMsg,*msgLen);
                        newMsgLen += *msgLen;
                        break;

                    }
                }
                // Content is not text, copy everything and break
                else
                {

                    free(pLine);
                    memcpy(&pNewMsg[newMsgLen],*pMsg,*msgLen);
                    newMsgLen += *msgLen;
                    break;
                }

            }

            // Free line before next run
            if(pLine)
                free(pLine);

        }
        while(1);

        // Change pointers of previous pMsg to the new one
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

        //If we reach here, we can continue on with sending the message (status 0)
        return 0;
    }


    /*
    	Read out line from buffer
    */
    char* get_line_from_buffer(char* pMsg, int* maxLen, int* lineLen)
    {

        // Count how long the line is
        if(*maxLen > 0)
        {
            int i = 1;
            while(i < *maxLen)
            {
                if(pMsg[i] == '\n')
                    break;

                i++;
            }

            // Add one more after the loop
            i++;

            // Extract the line
            char* tmp;
            tmp = (char *) malloc(i+1);
            memcpy(tmp,pMsg,i);

            tmp[i] = '\0';
#ifdef DEBUG
            printf("Line found: %s\n",tmp);
#endif

            // Remove extracted data from message and realign buffer
            memmove(pMsg,&pMsg[i],*maxLen-i);
            *maxLen -= i;

            // Set line length
            *lineLen = i;

            // Return pointer to the line
            return tmp;
        }

        // Return zero and null if nothing found
        *lineLen = 0;
        return NULL;
    }

    /*
    	Opens a socket to the destination on HTTP port 80 and tries to connect to it
    */
    int dest_connect(char* dest, char destClient[INET6_ADDRSTRLEN])
    {
        int 		destFd;
        struct 		addrinfo hints, *pServInfo, *pIter;

        // Set destination information
        memset(&hints, 0, sizeof(hints));
        hints.ai_family 	= AF_UNSPEC;
        hints.ai_socktype 	= SOCK_STREAM;

        // Try retrive address information
        int resp;
        if((resp = getaddrinfo(dest, HTTPPORT, &hints, &pServInfo))!= 0)
        {
            // Error while tyring to retrieve address information, print error and end the program with status 1
            fprintf(stderr, "client side: Error in getaddrinfo: %s\n", gai_strerror(resp));
            return -1;
        }

        // We got the address info, try opening the socket
        for(pIter = pServInfo; pIter != NULL; pIter->ai_next)
        {
            // Try to open socket
            if((destFd = socket(pIter->ai_family, pIter->ai_socktype, pIter->ai_protocol)) == -1)
            {
                // If we could not open the socket, print error message and move on to next interation
                perror("client side: Could not open socket");
                continue;
            }

            // Socket is open, try connect
            if(connect(destFd, pIter->ai_addr, pIter->ai_addrlen) == -1)
            {
                // Unable to connect to socket
                close(destFd);
                perror("client side: Unable to connect to socket");
                continue;
            }

            // Socket open and connected, break the loop
            break;
        }

        // If we where not able to connect, return -1
        if (pIter == NULL)
        {
            fprintf(stderr, "client side: Failed to connect to destination");
            return -1;
        }

        // Get destination address information
        inet_ntop(pIter->ai_family, get_in_addr((struct sockaddr *)&pIter->ai_addr), destClient, sizeof(destClient));

        printf("client side: Connecting to %s\n", destClient);

        // Free what we are not using anymore
        freeaddrinfo(pServInfo);

        // Return descriptor to handler
        return destFd;
    }
