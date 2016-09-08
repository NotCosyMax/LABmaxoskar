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

// Default port if none is given by the users
#define DEFAULTPORT "3123"
#define HTTPPORT    "80"

#define SERVBACKLOG 10

#define MAXBUFLEN 3000

#define DEBUG

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
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

//Incomming client (client -> server) handler function declaration.
void inc_client_handler(int incClientFd, char incClient[INET6_ADDRSTRLEN]);

int dest_connect(char dest[], char destClient[INET6_ADDRSTRLEN]);

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
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
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
	char 	destClient[INET6_ADDRSTRLEN];

	//Message buffer
	char buf[MAXBUFLEN];
	int  recLen;

	//Incomming client main loop
	//TODO: End when final respons is sent (think keep-alive);
	while(1)
	{

		memset(buf,0 , MAXBUFLEN);
		//Read out from socket
		if((recLen = recv(incClientFd, buf, MAXBUFLEN-1, 0)) == -1)
		{
			perror("server side: Error receiving from socket");
			//Exit?
		}

#ifdef DEBUG
		buf[MAXBUFLEN] = '\0';
		printf("server side:\n%s received:\n'%s'\n",incClient,buf);
#endif

		//Extract destination information
		//TODO: Make not hard coded


		//Do some filtering here, but for now we dont care about this


		//Open socket to the destination
		destFd = dest_connect("http://liu.se/", destClient);

        printf("got desc\n");
		if(destFd != -1)
		{
		    //Write request to destination
			int sentSize = 0;
			while(sentSize < recLen)
			{
				if((sentSize += send(destFd, buf, recLen, 0)) == -1)
				{
					perror("client side: Error sending to destination");
				}
			}

            printf("wrote to dest\n");
			memset(buf,0,MAXBUFLEN);
            //Wait for respons
			if((recLen = recv(destFd, buf, MAXBUFLEN-1, 0)) == -1)
			{
				perror("client side: Error receiving from socket");
				//Exit?
			}

			buf[MAXBUFLEN] = '\0';
			printf("client side:\n%s received:\n'%s'\n",destClient,buf);

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
	if((resp = getaddrinfo("130.236.5.66", HTTPPORT, &hints, &pServInfo))!= 0)
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
