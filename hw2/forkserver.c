// Tyler Sontag (sontat)
// 15 December, 2015
// CSCI-4210 Project 4

// NOTE: When receiving file data, the server always ignores the last
// character because it assumes it is an extraneous newline character
// sent by the client (e.g. netcat).

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <arpa/inet.h>

#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <signal.h>
#include <sys/wait.h>
#include <netdb.h>

#define BLOCK_SIZE 1024 //lowered just because.
#define NUM_BLOCKS 128

//alarm function that simply returns to get us out of recvfrom()
static void recv_alarm(int signo) {return;}

//readn from pg 89 of textbook
ssize_t readn(int fd, void *vptr, size_t n)
{
	size_t nleft;
	ssize_t nread;
	char *ptr;
	ptr = vptr;
	nleft = n;
	while (nleft > 0)
	{
		if ((nread = read(fd, ptr, nleft)) < 0)
		{
			if (errno == EINTR) nread = 0;
			else return (-1);
		}
		else if (nread == 0) break;
		nleft -= nread;
		ptr += nread;
	}
	return (n - nleft);
}

//writen from pg 89 of textbook
ssize_t writen(int fd, const void *vptr, size_t n)
{
	size_t nleft;
	ssize_t nwritten;
	const char *ptr;
	ptr = vptr;
	nleft = n;
	while (nleft > 0)
	{
		if ((nwritten = write(fd, ptr, nleft)) <= 0)
		{
			if (nwritten < 0 && errno == EINTR) nwritten = 0;
			else return (-1);
		}
		nleft -= nwritten;
		ptr += nwritten;
	}
	return(n);
}

int checkOpCode(char* buffer, int byte_count)
{
	if (byte_count < 4) return -1; //2 bytes for opcode + 1 byte for EOS + 1byte for filename
	if ((short)buffer[0] != 0) return -1; //all opcodes start with 0
	return (int)buffer[1]; //there's no way this works, right?
}

int HandleRRQ(char* buffer, int byte_count, int socket, struct sockaddr* client, socklen_t fromlen)
{
	socklen_t tolen = fromlen;
	char filename[byte_count];
	char mode[byte_count];
	strcpy(filename, buffer+2);
	strcpy(mode, buffer+strlen(filename)+3);
	int fd = open(filename, O_RDONLY,0);
	struct stat fileinfo;
	if (fd < 0 || fstat(fd, &fileinfo) < 0) {
		close(fd);
		printf("[child %d] can't open file %s\n", getpid(), filename);
		char errPack[BLOCK_SIZE];
		short errCode = 05;
		short* errCodeBytes = (short*)&errCode;
		memcpy(errPack, errCodeBytes, 2);
		errCode = 01; //file not found
		errCodeBytes = (short*)&errCode;
		memcpy(errPack+2, errCodeBytes, 2);
		char msg[20];
		strcpy(msg, "Invalid filename");
		memcpy(errPack+2, msg, strlen(msg)+1);
		sendto(socket, errPack, strlen(msg)+5, 0, (struct sockaddr*) &client, tolen);
		return -1;
	}
	//check for normal file.
	if (!S_ISREG(fileinfo.st_mode)) {
		close(fd);
		printf("[child %d] requested abnormal file\n", getpid());
		char errPack[BLOCK_SIZE];
		short errCode = 05;
		short* errCodeBytes = (short*)&errCode;
		memcpy(errPack, errCodeBytes, 2);
		errCode = 02; //access violation
		errCodeBytes = (short*)&errCode;
		memcpy(errPack+2, errCodeBytes, 2);
		char msg[20];
		strcpy(msg, "Invalid file");
		memcpy(errPack+2, msg, strlen(msg)+1);
		sendto(socket, errPack, strlen(msg)+5, 0, (struct sockaddr*) &client, tolen);
		return -1;
	}
	//check for octet mode. Currently only checks "octet" and not things such as "ocTEt"
	if (strcmp(mode, "octet") != 0)
	{
		close(fd);
		printf("[child %d] received non-octet request\n", getpid());
		char errPack[BLOCK_SIZE];
		short errCode = 05;
		short* errCodeBytes = (short*)&errCode;
		memcpy(errPack, errCodeBytes, 2);
		errCode = 00; //not defined
		errCodeBytes = (short*)&errCode;
		memcpy(errPack+2, &errCodeBytes, 2);
		char msg[20];
		strcpy(msg, "Not octet mode");
		memcpy(errPack+2, msg, strlen(msg)+1);
		sendto(socket, errPack, strlen(msg)+5, 0, (struct sockaddr*) &client, tolen);
		return -1;
	}
	//otherwise...
	//set alarm
	signal(SIGALRM, recv_alarm);
	char content[512];
	int nbytes = readn(fd, content, 512);
	short blockn = 1;
	char packet[516];
	while(1)
	{
		short* blocknBytes = (short*)&blockn;
		short opCode = 03;
		short* opCodeBytes = (short*)&opCode;
		memcpy(packet, opCodeBytes, 2);
		memcpy(packet+2, blocknBytes, 2);
		memcpy(packet+4, content, nbytes);
		sendto(socket, packet, nbytes+4, 0, (struct sockaddr*) &client, tolen);
		//wait for ack for 10s.
		int tries = 0;
		while (1)
		{
			byte_count = 0;
			alarm(1);
			byte_count = recvfrom(socket, buffer, sizeof(buffer), 0, (struct sockaddr*) &client, &tolen);
			alarm(0); //we got something, turn it off.
			if (byte_count == 0) //we didn't get anything
			{
				if (tries == 10) //we waited 10 seconds.
				{
					close(fd);
					return -1;
				}
				tries = tries + 1;
				continue;
			}
			if (byte_count < 4) //invalid TFTP packet
			{
				close(fd);
				printf("[child %d] received invalid TFTP packet\n", getpid());
				char errPack[BLOCK_SIZE];
				short errCode = 05;
				short* errCodeBytes = (short*)&errCode;
				memcpy(errPack, errCodeBytes, 2);
				errCode = 04; //illegal TFTP operation
				errCodeBytes = (short*)&errCode;
				memcpy(errPack+2, errCodeBytes, 2);
				char msg[20];
				strcpy(msg, "invalid packet");
				memcpy(errPack+2, msg, strlen(msg)+1);
				sendto(socket, errPack, strlen(msg)+5, 0, (struct sockaddr*) &client, tolen);
				return -1;
			}
			if (checkOpCode(buffer, byte_count) != 4 && checkOpCode(buffer, byte_count) != 5)
			{
				tries = 0; //we technically heard something
				continue;
			}
			else if (checkOpCode(buffer, byte_count) == 5) //error means just close connection
			{
				return -1;
			}
			else //we know it's an ack
			{
				short* tempblock;
				memcpy(tempblock, buffer+2, 2); //get the block number
				if (*tempblock < blockn) //duplicate ack
				{
					tries = 0; //we technically heard something
					continue;
				}
				else if (*tempblock > blockn) //big mistake
				{
					close(fd);
					printf("[child %d] received ACK for future block\n", getpid());
					char errPack[BLOCK_SIZE];
					short errCode = 05;
					short* errCodeBytes = (short*)&errCode;
					memcpy(errPack, errCodeBytes, 2);
					errCode = 04; //illegal TFTP operation
					errCodeBytes = (short*)&errCode;
					memcpy(errPack+2, errCodeBytes, 2);
					char msg[20];
					strcpy(msg, "future block ACK");
					memcpy(errPack+2, msg, strlen(msg)+1);
					sendto(socket, errPack, strlen(msg)+5, 0, (struct sockaddr*) &client, tolen);
					return -1;
				}
				else //tempblock == blockn
				{
					break;
				}
			}
		}
		if (nbytes < 512) return 0; //end of file
		//otherwise read the next 512 bytes.
		nbytes = readn(fd, content, 512);
	}
}

int HandleWRQ(char* buffer, int byte_count, int socket, struct sockaddr* client, socklen_t fromlen)
{
	socklen_t tolen = fromlen;
	char filename[byte_count];
	char mode[byte_count];
	strcpy(filename, buffer+2);
	strcpy(mode, buffer+strlen(filename)+3);
	int fd = creat(filename, 0644);
	if (fd < 0) {
		close(fd);
		printf("[child %d] can't create file %s\n", getpid(), filename);
		char errPack[BLOCK_SIZE];
		short errCode = 05;
		short* errCodeBytes = (short*)&errCode;
		memcpy(errPack, errCodeBytes, 2);
		errCode = 06; //file already exists
		errCodeBytes = (short*)&errCode;
		memcpy(errPack+2, errCodeBytes, 2);
		char msg[20];
		strcpy(msg, "file already exists");
		memcpy(errPack+2, msg, strlen(msg)+1);
		sendto(socket, errPack, strlen(msg)+5, 0, (struct sockaddr*) &client, tolen);
		return -1;
	}
	
	//check for octet mode. Currently only checks "octet" and not things such as "ocTEt"
	if (strcmp(mode, "octet") != 0)
	{
		close(fd);
		printf("[child %d] received non-octet request\n", getpid());
		char errPack[BLOCK_SIZE];
		short errCode = 05;
		short* errCodeBytes = (short*)&errCode;
		memcpy(errPack, errCodeBytes, 2);
		errCode = 00; //not defined
		errCodeBytes = (short*)&errCode;
		memcpy(errPack+2, &errCodeBytes, 2);
		char msg[20];
		strcpy(msg, "Not octet mode");
		memcpy(errPack+2, msg, strlen(msg)+1);
		sendto(socket, errPack, strlen(msg)+5, 0, (struct sockaddr*) &client, tolen);
		return -1;
	}
	//otherwise...
	
	//send ACK
	char ackPack[4];
	short ackCode = 04;
	short* ackCodeBytes = (short*)&ackCode;
	memcpy(ackPack, ackCodeBytes, 2);
	short blockNum = 00;
	short* blockNumBytes = (short*)&blockNum;
	memcpy(ackPack+2, blockNumBytes, 2);
	sendto(socket, ackPack, 4, 0, (struct sockaddr*) &client, tolen);
	
	//set alarm
	signal(SIGALRM, recv_alarm);

	int tries = 0;
	blockNum++;
	while(1)
	{
		byte_count = 0;
		alarm(1);
		byte_count = recvfrom(socket, buffer, sizeof(buffer), 0, (struct sockaddr*) &client, &tolen);
		alarm(0); //we got something, turn it off.
		
		if (byte_count == 0) //we didn't get anything
		{
			if (tries == 10) //we waited 10 seconds.
			{
				close(fd);
				return -1;
			}
			tries = tries + 1;
			continue;
		}
		
		if (byte_count < 4) //invalid TFTP packet
		{
			close(fd);
			printf("[child %d] received invalid TFTP packet\n", getpid());
			char errPack[BLOCK_SIZE];
			short errCode = 05;
			short* errCodeBytes = (short*)&errCode;
			memcpy(errPack, errCodeBytes, 2);
			errCode = 04; //illegal TFTP operation
			errCodeBytes = (short*)&errCode;
			memcpy(errPack+2, errCodeBytes, 2);
			char msg[20];
			strcpy(msg, "invalid packet");
			memcpy(errPack+2, msg, strlen(msg)+1);
			sendto(socket, errPack, strlen(msg)+5, 0, (struct sockaddr*) &client, tolen);
			return -1;
		}
		else if (checkOpCode(buffer, byte_count) != 3 && checkOpCode(buffer, byte_count) != 5)
		{
			tries = 0; //we technically heard something
			continue;
		}
		else if (checkOpCode(buffer, byte_count) == 5) //error means just close connection
		{
			return -1;
		}
		else //we know it's a data pack
		{
			short* tempblock;
			memcpy(tempblock, buffer+2, 2); //get the block number
			if (*tempblock <= blockNum) //duplicate packet
			{
				tries = 0; //we technically heard something
				continue;
			}
			else if (*tempblock > blockNum+1) //big mistake
			{
				close(fd);
				printf("[child %d] received packet for future block\n", getpid());
				char errPack[BLOCK_SIZE];
				short errCode = 05;
				short* errCodeBytes = (short*)&errCode;
				memcpy(errPack, errCodeBytes, 2);
				errCode = 04; //illegal TFTP operation
				errCodeBytes = (short*)&errCode;
				memcpy(errPack+2, errCodeBytes, 2);
				char msg[20];
				strcpy(msg, "future block");
				memcpy(errPack+2, msg, strlen(msg)+1);
				sendto(socket, errPack, strlen(msg)+5, 0, (struct sockaddr*) &client, tolen);
				return -1;
			}
			else //tempblock = blockNum+1, write to file.
			{
				writen(fd, buffer+4, 512);
				//send ack
				memcpy(ackPack, ackCodeBytes, 2);
				blockNum = *tempblock;
				blockNumBytes = (short*)&blockNum;
				memcpy(ackPack+2, blockNumBytes, 2);
				sendto(socket, ackPack, 4, 0, (struct sockaddr*) &client, tolen);
				//if byte_count < 516 it's the end of the file
				return 0;
			}
		}
	}
}

int main()
{
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  
  if (sock < 0) {
    perror("socket() failed");
    exit(EXIT_FAILURE);
  }
	
  struct sockaddr_in server;

  server.sin_family = AF_INET;
  server.sin_addr.s_addr = INADDR_ANY;

  //choose arbitrary port
  //unsigned short port = 8765;

  server.sin_port = htons(0);
  int len = sizeof(server);

  if (bind(sock, (struct sockaddr *)&server, len) < 0) {
    perror("bind() failed");
    exit(EXIT_FAILURE);
  }

  struct sockaddr_in temp_addr;
  socklen_t sz = sizeof(temp_addr);
  int ret = getsockname(sock, (struct sockaddr*) &temp_addr, &sz);
  if (ret < 0) {
	  printf("problem!\n");
	  exit(-1);
  }
  int the_port = ntohs(temp_addr.sin_port);
  printf("Created port %d\n", the_port);

  //listen(sock, 10); // Maximum number of clients allowed is 10

  struct sockaddr_in client;
  int fromlen = sizeof(client);

  char buffer[BLOCK_SIZE];

  while (1) {
	// recvfrom(int sock, void* buf, size_t size_of_buf, int flags, struct sockaddr* from, socklen_t *fromlen)
    int byte_count = recvfrom(sock, (void*)buffer, sizeof(buffer), 0, (struct sockaddr *)&client, &fromlen);
    printf("Received %d bytes from incoming connection %s\n", byte_count, inet_ntoa((struct in_addr)client.sin_addr));

    int pid;
    pid = fork();

    if (pid < 0) {
      perror("fork() failed");
      exit(EXIT_FAILURE);
    }
    else if (pid == 0) {
		close(sock);
		//create/bind new socket for this connection.
		int newsock = socket(AF_INET, SOCK_DGRAM, 0);
  
    	if (newsock < 0) {
    		perror("socket() failed");
    		exit(EXIT_FAILURE);
  		}
	
  		struct sockaddr_in newserver;

  		newserver.sin_family = AF_INET;
  		newserver.sin_addr.s_addr = INADDR_ANY;
  		newserver.sin_port = htons(0);
  		int newlen = sizeof(newserver);

  		if (bind(newsock, (struct sockaddr *)&newserver, newlen) < 0) {
    		perror("bind() failed");
    		exit(EXIT_FAILURE);
  		}
		
		//now we check the request opCode, which i'm not 100% sure how to do
		int clientOpCode = checkOpCode(buffer, byte_count);
		int rtn = 0;
		if (clientOpCode == 1) {rtn = HandleRRQ(buffer, byte_count, newsock, (struct sockaddr *)&client, fromlen); }
		else if (clientOpCode == 2) {rtn = HandleWRQ(buffer, byte_count, newsock, (struct sockaddr *)&client, fromlen); }
		else {rtn = -1;} //a new client that isn't RRQ or WRQ is an error.
		
		//close the new socket
		close(newsock);
		printf("[child %d] Client closed its socket....terminating\n", getpid());
		if (rtn == 0) {	exit(EXIT_SUCCESS); }
		else { exit(EXIT_FAILURE); }
    }
    else { /*do nothing*/ }
  }

  close(sock);

  return EXIT_SUCCESS;
}
