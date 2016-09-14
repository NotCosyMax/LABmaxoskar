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
#define MSGLISTSIZE 254;

struct msg_list {
	char 			 	data[MSGLISTSIZE];
	unsigned char		dataLen = 0;
	struct msg_list* 	next = NULL;
}

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

int check_list_content(struct msg_list* list, char* dest);

int get_line_from_buffer(char line[], char buf[], int maxsize);

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


/* Breif: Incomming client handler for server side client connection (client -> server)
*
*/
void inc_client_handler(int incClientFd, char incClient[INET6_ADDRSTRLEN])
{

    //Destination filedescriptor
    int 	destFd;
    //Destination IP
    char 	destClient[INET6_ADDRSTRLEN];
    //Destination URL
    char    dest[128];

    //Message buffers
    char recBuf[MAXBUFLEN];
    char sendBuf[MAXBUFLEN];
    int  recLen;
    int  sendLen;



    //Incomming client main loop
    //TODO: End when final respons is sent (think keep-alive);
    while(1)
    {
        //Read from socket (non blocking)
		struct msg_list* pRecList = receive_from_socket(incClientFd,STOPATBLOCKING);

		//If recLen is zero, the client closed the connection, end!
		if(pRecLen->dataLen == 0)
			break;
		
#ifdef DEBUG
        //recBuf[MAXBUFLEN] = '\0';
        //printf("server side:\n%s received before mod:\n'%s'\n",incClient,recBuf);
#endif

        //Connection type modified
        char conMod = 0;
        //Check if URL is ok
        int match = regexec(&regBadWord, recBuf, 0, NULL, 0);
        if(!match)
        {
            //Redirect to "safe" site
            printf("Bad site!\n");
            memset(sendBuf,0,MAXBUFLEN);
            memcpy(sendBuf,REDIRECTHEADER,REDIRECTLEN);
            sendLen = REDIRECTLEN;
            sendBuf[REDIRECTLEN] = '\0';

        }
        else
        {
            //Move recBuf to sendBuf line by line to catch "Host", "Connection", and "Length"
            //lines in order to manipulate the header if needed.



            //Set sendLen to zero
            sendLen = 0;
            //Clear send buffer
            memset(sendBuf,0,MAXBUFLEN);

            //Line buffer
            char line[MAXBUFLEN];
            int lineLen;

            //Result from line match with reg. exp.
            int matchLine;

            //If we received a message
            while(recLen > 0)
            {
                //Clear line buffer
                memset(line,0,MAXBUFLEN);
                //Read line
                lineLen = get_line_from_buffer(line,recBuf,recLen);
                //Subtract the lenght of the line from the received buffer length
                recLen -= lineLen;

                //Check if it is the "Host" line
                if((matchLine = regexec(&regHost, line, 0, NULL, 0)) == 0)
                {
                    memset(dest,0,128);
                    //Extract host URL/destination
                    memcpy(dest,&line[6],lineLen-8);
                    dest[lineLen-7] = '\0';

                    //Write to sendBuf
                    memcpy(&sendBuf[sendLen],line,lineLen);
                    sendLen += lineLen;
                    printf("found host %s\n",dest);
                }
                //Check if it is the "Connection" line
                else if((matchLine = regexec(&regCon, line, 0, NULL, 0)) == 0)
                {
                    //Change to connection type "close" if it is "keep-alive"
                    memcpy(&sendBuf[sendLen],"Connection: close\r\n",19);
                    sendLen += 19;
                    printf("found connection\n");
                    //We have modified the connection, flag it!
                    conMod = 1;
                }
                else
                {
                    //Nothing special, just copy line to sendBuf
                    memcpy(&sendBuf[sendLen],line,lineLen);
                    sendLen += lineLen;
                }

            }
#ifdef DEBUG
            sendBuf[MAXBUFLEN] = '\0';
            printf("server side:\n%s received after mod:\n'%s'\n",incClient,sendBuf);
#endif
            //Open socket to the destination
            destFd = dest_connect(dest, destClient);

            //If we managed to open the socket, write the modified message to the destination
            if(destFd != -1)
            {
                //Number of bytes sent
                int sentLend = 0;
                //Write request to destination (send until we have sent sendLen number o f bytess)
                while(sentLend != sendLen)
                {
                    if((sentLend += send(destFd, sendBuf, sendLen, 0)) == -1)
                    {
                        perror("client side: Error sending to destination");
                    }
                }


                memset(recBuf,0,MAXBUFLEN);
                recLen = 0;
                int recTemp = 0;
                char recBufTemp[MAXBUFLEN];
                memset(recBuf,0,MAXBUFLEN);

                do
                {

                    //Wait for respons
                    if((recTemp = recv(destFd, recBufTemp, MAXBUFLEN-1, 0)) == -1)
                    {
                        perror("client side: Error receiving from socket");
                        //Exit?
                    }
                    memcpy(&recBuf[recLen],recBufTemp,recTemp);
                    recLen += recTemp;

                }
                while(recTemp != 0);



                //Filter respons:
                // * Check content type.
                // * If text, look foor keywords
                // * If found, redirect to "safe site"
                // * Else, return Connection: keep-alive before sending the message back
                //Code here
				int match = regexec(&regType, recBuf, 0, NULL, 0);
				if(!match)
				{				
					match = regexec(&regBadWord, recBuf, 0, NULL, 0);
					if(!match)
					{
						//Redirect to "safe" site
						printf("Bad site!\n");
						memset(sendBuf,0,MAXBUFLEN);
						memcpy(sendBuf,REDIRECTHEADER,REDIRECTLEN);
						sendLen = REDIRECTLEN;
						sendBuf[REDIRECTLEN] = '\0';
						
						//Break loop, no need to do any further processing
						break;
					}
				}
				
                //Read line by line until we have found and changed the connection type(if we changed this earlier)
                if(conMod == 3)
                {
                    //Clear send buffer
                    memset(sendBuf,0,MAXBUFLEN);

                    //Line buffer
                    char line[MAXBUFLEN];
                    int lineLen;

                    //Result from line match with reg. exp.
                    int matchLine;

                    //Set sendLen to zero
                    sendLen = 0;

                    while(recLen > 0)
                    {
                        //Clear line buffer
                        memset(line,0,MAXBUFLEN);
                        //Read line
                        lineLen = get_line_from_buffer(line,recBuf,recLen);
                        //Subtract the lenght of the line from the received buffer length
                        recLen -= lineLen;

                        //Check if it is the "Connection" line
                        if((matchLine = regexec(&regConMod, line, 0, NULL, 0)) == 0)
                        {
                            //Change to connection type "close" if it is "keep-alive"
                            //memcpy(&sendBuf[sendLen],"Keep-Alive: timeout=5, max=100\r\n",32);
                            //sendLen += 32;
                            memcpy(&sendBuf[sendLen],"Connection: keep-alive\r\n",24);
                            sendLen += 24;
                            printf("found connection\n");
                            //We have modified the connection, flag it!
                            conMod = 0;

                            //Copy the rest of the message and end the loop
                            memcpy(&sendBuf[sendLen],recBuf,recLen);
                            sendLen += recLen;
                            break;
                        }
                        else
                        {
                            //Nothing special, just copy line to sendBuf
                            memcpy(&sendBuf[sendLen],line,lineLen);
                            sendLen += lineLen;
                        }

                    }
                }
                else
                {
                    //We do not need to modify the header
                    //Clear send buffer
                    memset(sendBuf,0,MAXBUFLEN);
                    memcpy(sendBuf,recBuf,recLen);
                    sendLen = recLen;
                }

            }


        }

#ifdef DEBUG
        sendBuf[MAXBUFLEN] = '\0';
        printf("client side: %s received and moded:\n'%s'\n",destClient,sendBuf);
#endif

        int sentLend = 0;

        //Write request to destination (send until we have sent sendLen number o f bytess)
        while(sentLend != sendLen)
        {
            if((sentLend += send(incClientFd, sendBuf, sendLen, 0)) == -1)
            {
                perror("client side: Error sending to destination");
            }
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
	
	int recLen
	char bufLen = MSGLISTSIZE;
	do
	{
		//Read out at most MSGLISTSIZE from socket
		recLen = recv(socketFd, &pRecList->data[MSGLISTSIZE-bufLen], bufLen, flags);
		
		//If recList returns "EAGAIN" or "EWOULDBLOCK" we stop reading (this is only true if stopat is set)
		if(stopat && ((recLen == EAGAIN) || (recLen == EWOULDBLOCK)))
		{
			//Flag we could not read anything
			pRecList->dataLen = -1;
			break;
		}
		else
		{
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
		
	}while(1);
	
	//Return pointer
	return pRecList;
}

//Check the contents of the listand returns either a modified list, or a list containing a redirect message.
//It also returns the destination as an argument
//The functions return with the status (-1 = redirect, 0 = no modification, 1 = modified) 
int check_list_content(struct msg_list* list, char* dest)
{
	
    //Reg. Exp. for detecting "bad" words as well as "Host", "Connection" and "Length" lines in HTTP header
    regex_t regUBadWord, regHost, regCon, regConMod, regType;
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
    if((retv = regcomp(&regConMod, "Connection. ", REG_ICASE|REG_EXTENDED)))
    {
        printf("Reg exp comp. fail\n");
    }
    if((retv = regcomp(&regType, "Content-Type.* text.html|text.xml", REG_ICASE|REG_EXTENDED)))
    {
        printf("Reg exp comp. fail\n");
    }
	
	//First we check if the list conatins a GET message or a HTTP respons
	int match = regexec(&regBadWord, recBuf, 0, NULL, 0);

	//If 
	
	return match;
}

int get_line_from_buffer(char line[], char buf[], int maxSize)
{

    int i;

    memset(line,0,MAXBUFLEN);

    for(i = 0; i < maxSize; i++)
    {

        line[i] = buf[i];

        if(buf[i] == '\n')
            break;

    }
    i++;

    //Shift buffer to remove extracted line
    memmove(buf,&buf[i],maxSize-i);

    return i;
}
