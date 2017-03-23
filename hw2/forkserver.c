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

//used for debugging
void print_bytes(const void *object, size_t size)
{
	const unsigned char * const bytes = object;
	size_t i;

	printf("[ ");
	for(i = 0; i < size; i++)
	{
		printf("%02x ", bytes[i]);
	}
	printf("]\n");
}

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
	return (int)buffer[1];
}

int HandleRRQ(char* buffer, int byte_count, int socket, struct sockaddr* client, socklen_t fromlen)
{
	socklen_t tolen = fromlen;
	char filename[byte_count];
	char mode[byte_count];
	strcpy(filename, buffer+2);
	strcpy(mode, buffer+strlen(filename)+3);

	//check for octet mode.
	if (strcmp(mode, "octet") != 0)
	{
		printf("[child %d] received non-octet request\n", getpid());
		char errPack[BLOCK_SIZE];
		short errCode = htons(5);
		memcpy(errPack, &errCode, 2);
		errCode = htons(0); //file not found
		memcpy(errPack+2, &errCode, 2);
		char msg[20];
		strcpy(msg, "Not octet mode");
		memcpy(errPack+4, msg, strlen(msg)+1);
		sendto(socket, errPack, strlen(msg)+5, 0, client, tolen);
		return -1;
	}

	int fd = open(filename, O_RDONLY,0);
	struct stat fileinfo;
	if (fd < 0 || fstat(fd, &fileinfo) < 0)
	{
		close(fd);
		printf("[child %d] can't open file %s\n", getpid(), filename);
		char errPack[BLOCK_SIZE];
		short errCode = htons(5);
		memcpy(errPack, &errCode, 2);
		errCode = htons(1); //file not found
		memcpy(errPack+2, &errCode, 2);
		char msg[20];
		strcpy(msg, "Invalid filename");
		memcpy(errPack+4, msg, strlen(msg)+1);
		int result = sendto(socket, errPack, strlen(msg)+5, 0, client, tolen);
		printf("Result %d\n", result);
		return -1;
	}
	//check for normal file.
	if (!S_ISREG(fileinfo.st_mode))
	{
		close(fd);
		printf("[child %d] requested abnormal file\n", getpid());
		char errPack[BLOCK_SIZE];
		short errCode = htons(5);
		memcpy(errPack, &errCode, 2);
		errCode = htons(2); //file not found
		memcpy(errPack+2, &errCode, 2);
		char msg[20];
		strcpy(msg, "Invalid file");
		memcpy(errPack+4, msg, strlen(msg)+1);
		sendto(socket, errPack, strlen(msg)+5, 0, client, tolen);
		return -1;
	}

	//set alarm
	signal(SIGALRM, recv_alarm);
	int nbytes;
	char newBuffer[BLOCK_SIZE];
	char content[512];
	short blockn = 1;
	char packet[516];

	while(1)
	{
		nbytes = readn(fd, content, 512);
		short opCode = htons(3);
		memcpy(packet, &opCode, 2);
		short netBlockn = htons(blockn);
		memcpy(packet+2, &netBlockn, 2);
		memcpy(packet+4, content, nbytes);
		//send data packet
		sendto(socket, packet, nbytes+4, 0, client, tolen);
		printf("[child %d] sent data pack %d to client\n", getpid(), blockn);
		//wait for ack for 10s.
		int tries = 0;
		while (1)
		{
			byte_count = 0;
			alarm(1);
			byte_count = recvfrom(socket, newBuffer, sizeof(buffer), 0, client, &tolen);
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
				short errCode = htons(5);
				memcpy(errPack, &errCode, 2);
				errCode = htons(4); //illegal TFTP operation
				memcpy(errPack+2, &errCode, 2);
				char msg[20];
				strcpy(msg, "invalid packet");
				memcpy(errPack+4, msg, strlen(msg)+1);
				sendto(socket, errPack, strlen(msg)+5, 0, (struct sockaddr*) &client, tolen);
				return -1;
			}
			if (checkOpCode(newBuffer, byte_count) != 4 && checkOpCode(newBuffer, byte_count) != 5)
			{
				tries = 0; //we technically heard something
				continue;
			}
			else if (checkOpCode(newBuffer, byte_count) == 5) //error means just close connection
			{
				return -1;
			}
			else //we know it's an ack
			{
				char tempblockNet[2];
				memcpy(tempblockNet, newBuffer+2, 2); //get the block number
				unsigned char firstBit = tempblockNet[0];
				unsigned char secondBit = tempblockNet[1];
				unsigned short tempblock = (firstBit << 8) + secondBit; // convert chars to short

				if (tempblock < blockn) //duplicate ack
				{
					tries = 0; //we technically heard something
					continue;
				}
				else if (tempblock > blockn) //big mistake
				{
					close(fd);
					printf("[child %d] received ACK for future block\n", getpid());
					char errPack[BLOCK_SIZE];
					short errCode = htons(5);
					memcpy(errPack, &errCode, 2);
					errCode = htons(4); //illegal TFTP operation
					memcpy(errPack+2, &errCode, 2);
					char msg[20];
					strcpy(msg, "future block ACK");
					memcpy(errPack+4, msg, strlen(msg)+1);
					sendto(socket, errPack, strlen(msg)+5, 0, (struct sockaddr*) &client, tolen);
					return -1;
				}
				else //tempblock == blockn
				{
					//move on to next data packet
					blockn++;
					break;
				}
			}
		}
		if (nbytes < 512) return 0; //end of file
	}
}

int HandleWRQ(char* buffer, int byte_count, int socket, struct sockaddr* client, socklen_t fromlen)
{
	socklen_t tolen = fromlen;
	char filename[byte_count];
	char mode[byte_count];
	strcpy(filename, buffer+2);
	strcpy(mode, buffer+strlen(filename)+3);

	//check for octet mode.
	if (strcmp(mode, "octet") != 0)
	{
		printf("[child %d] received non-octet request\n", getpid());
		char errPack[BLOCK_SIZE];
		short errCode = htons(5);
		memcpy(errPack, &errCode, 2);
		errCode = htons(0); //not defined
		memcpy(errPack+2, &errCode, 2);
		char msg[20];
		strcpy(msg, "Not octet mode");
		memcpy(errPack+4, msg, strlen(msg)+1);
		sendto(socket, errPack, strlen(msg)+5, 0, client, tolen);
		return -1;
	}

	int fd = creat(filename, 0644);
	if (fd < 0)
	{
		close(fd);
		printf("[child %d] can't create file %s\n", getpid(), filename);
		char errPack[BLOCK_SIZE];
		short errCode = htons(5);
		memcpy(errPack, &errCode, 2);
		errCode = htons(6); //file already exists
		memcpy(errPack+2, &errCode, 2);
		char msg[20];
		strcpy(msg, "file already exists");
		memcpy(errPack+2, msg, strlen(msg)+1);
		sendto(socket, errPack, strlen(msg)+5, 0, client, tolen);
		return -1;
	}
	
	//send ACK
	char ackPack[4];
	short ackCode = htons(4);
	memcpy(ackPack, &ackCode, 2);
	short blockNum = htons(0);
	memcpy(ackPack+2, &blockNum, 2);
	sendto(socket, ackPack, 4, 0, client, tolen);
	
	//set alarm
	signal(SIGALRM, recv_alarm);
	char newBuffer[BLOCK_SIZE];
	int tries = 0;
	blockNum++;

	while(1)
	{
		alarm(1);
		byte_count = recvfrom(socket, (void *)newBuffer, sizeof(newBuffer), 0, client, &fromlen);
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
			short errCode = htons(5);
			memcpy(errPack, &errCode, 2);
			errCode = htons(4); //illegal TFTP operation
			memcpy(errPack+2, &errCode, 2);
			char msg[20];
			strcpy(msg, "invalid packet");
			memcpy(errPack+4, msg, strlen(msg)+1);
			sendto(socket, errPack, strlen(msg)+5, 0, client, tolen);
			return -1;
		}
		else if (checkOpCode(newBuffer, byte_count) != 3 && checkOpCode(newBuffer, byte_count) != 5)
		{
			tries = 0; //we technically heard something
			continue;
		}
		else if (checkOpCode(newBuffer, byte_count) == 5) //error means just close connection
		{
			return -1;
		}
		else //we know it's a data pack
		{
			char tempblockNet[2];
			memcpy(tempblockNet, newBuffer+2, 2); //get the block number
			unsigned char firstBit = tempblockNet[0];
			unsigned char secondBit = tempblockNet[1];
			unsigned short tempblock = (firstBit << 8) + secondBit; // convert chars to short

			if (tempblock < blockNum) //duplicate packet
			{
				printf("[child %d] received duplicate packet\n", getpid());
				tries = 0; //we technically heard something
				continue;
			}
			else if (tempblock > blockNum) //big mistake
			{
				close(fd);
				printf("[child %d] received packet for future block\n", getpid());
				char errPack[BLOCK_SIZE];
				short errCode = htons(5);
				memcpy(errPack, &errCode, 2);
				errCode = htons(4); //illegal TFTP operation
				memcpy(errPack+2, &errCode, 2);
				char msg[20];
				strcpy(msg, "future block");
				memcpy(errPack+4, msg, strlen(msg)+1);
				sendto(socket, errPack, strlen(msg)+5, 0, client, tolen);
				return -1;
			}
			else //tempblock = blockNum, write to file.
			{
				printf("[child %d] wrote data pack %d to disk\n", getpid(), blockNum);
				writen(fd, newBuffer+4, byte_count-4);
				//send ack
				short blockNumNet = htons(blockNum);
				memcpy(ackPack, &ackCode, 2);
				memcpy(ackPack+2, &blockNumNet, 2);
				sendto(socket, ackPack, 4, 0, client, tolen);
				blockNum++;
				if (byte_count < 512) return 0;
			}
		}
	}
}

int main()
{
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	  
	if (sock < 0)
	{
		perror("socket() failed");
		exit(EXIT_FAILURE);
	}
	
	struct sockaddr_in server;

	server.sin_family = AF_INET;
	server.sin_addr.s_addr = INADDR_ANY;
	server.sin_port = htons(0);
	int len = sizeof(server);

	if (bind(sock, (struct sockaddr *)&server, len) < 0)
	{
		perror("bind() failed");
		exit(EXIT_FAILURE);
	}

	struct sockaddr_in temp_addr;
	socklen_t sz = sizeof(temp_addr);
	int ret = getsockname(sock, (struct sockaddr*) &temp_addr, &sz);
	if (ret < 0)
	{
		printf("problem!\n");
		exit(-1);
	}
	int the_port = ntohs(temp_addr.sin_port);
	printf("Created port %d\n", the_port);

	struct sockaddr_in client;
	unsigned int fromlen = sizeof(client);

	char buffer[BLOCK_SIZE];

	while (1)
	{
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
		else { /*parent process, do nothing*/ }
	}

	close(sock);

	return EXIT_SUCCESS;
}
