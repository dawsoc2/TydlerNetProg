#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <openssl/md5.h>

#define MAXBUF 1024
#define FILEBUF 1024*1024

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

int unpackTime(char* buffer) {
	// assume time arrives in big-endian
	return
		((unsigned char) buffer[0] << 24) +
		((unsigned char) buffer[1] << 16) +
		((unsigned char) buffer[2] << 8) +
		((unsigned char) buffer[3]);
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

int handleServer(unsigned short port) {
	printf("Handling server on port %d\n", port);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket() failed");
        return 1;
    }

    struct sockaddr_in serveraddr;
    memset(&serveraddr, 0, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(port);
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0) {
        perror("bind() failed");
        return 1;
    }

	char buffer[MAXBUF];

	char lastmodstr[4];
	int client_lastmod;
	int server_lastmod;
	unsigned char md5[16];
	char fname[256];
	char fullpath[256];

	struct stat filestat;

    while (1) {
        int length = recvfrom(fd, buffer, sizeof(buffer) - 1, 0, NULL, 0);
        if (length < 0) {
            perror("recvfrom() failed");
            break;
        }
		memcpy(lastmodstr, buffer + 2, 4);
		client_lastmod = unpackTime(lastmodstr);
		memcpy(md5, buffer + 6, 16);
		memcpy(fname, buffer + 22, length - 22);
		printf("File %s was last modified %d\n", fname, server_lastmod);

		// this directory is temporary, will be fixed later
		strcpy(fullpath, "./folder/");
		strcpy(fullpath + 9, fname);
		printf("Attempting to access %s\n", fullpath);
		int res = stat(fullpath, &filestat);
		if (res == -1 && errno == ENOENT) {
			printf("%s doesn't exist on server\n", fname);
		} else {
			server_lastmod = (int) filestat.st_mtime;
			printf("%s exists on server and was last modified %d\n", fname, server_lastmod);
			if (client_lastmod > server_lastmod) {
				printf("I will request client for the file\n");
			} else {
				printf("My file is more recent, do nothing\n");
			}
		}

		printf("\n");
        
		/*
		if file is not on server
			request file back from client
			wait for response
			read in file
		if file is on server
			if hashes are not equal
				if client lastmod > server lastmod
					request file back from client
					wait for response
					read in file
			else
				send "don't need file" message back
				server will send its files to client in second phase
		*/
    }

	// after this is done, send file info to the client

    close(fd);
	return 0;
}

int handleClient(unsigned short port) {
	printf("Handling client on port %d\n", port);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) {
        perror("socket() failed");
        return 1;
    }

    struct sockaddr_in serveraddr;
    memset( &serveraddr, 0, sizeof(serveraddr) );
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(port);              
    serveraddr.sin_addr.s_addr = htonl(0x7f000001); // connect to localhost

    int len, n, n_sent, fd2;
	ssize_t f_read;
	short opcode;
    char buffer[MAXBUF];
	char buffile[FILEBUF];
	struct stat filestat;

	int lastmod;
	unsigned char md5[16];
	char fname[256];

	len = sizeof(serveraddr);

	DIR *d;
	struct dirent *dir;
	d = opendir(".");
	if (d) {
		while ((dir = readdir(d)) != NULL) {
			if (dir->d_type == DT_REG) {

				opcode = htons(1);
				memcpy(buffer, &opcode, 2);

				fd2 = open(dir->d_name, O_RDONLY, 0);
				f_read = (fd2, buffile, FILEBUF);

				fstat(fd2, &filestat);
				lastmod = htonl((int) filestat.st_mtime);
				memcpy(buffer + 2, &lastmod, 4);

				//MD5((unsigned char *) buffile, f_read, md5);
				memcpy(buffer + 6, md5, 16);

				n = strlen(dir->d_name);
				memcpy(fname, dir->d_name, n);
				fname[n] = '\0';
				memcpy(buffer + 22, fname, n + 1);

				n_sent = sendto(fd, buffer, n + 23, 0, (struct sockaddr *)&serveraddr, len);
				printf("Sent %d bytes for file %s, last mod %d\n", n_sent, dir->d_name, (int) filestat.st_mtime);

				/*
				wait for a message back
				if the client sends a "don't need file" message
					continue
				else
					send the file
				*/

				close(fd2);
			}
		}
		closedir(d);
	}

	// after this is done, wait for file info from the server 

	close(fd);
	return 0;
}

int main (int argc, char *argv[]) {
    if (argc != 3) {
        printf("ERROR: invalid arguments\n");
        printf("USAGE: ./a.out <client/server> <port>\n");
        return EXIT_FAILURE;
    }
	
	if (strcmp(argv[1], "server") == 0) {
		handleServer(atoi(argv[2]));
		return EXIT_SUCCESS;
	} else if (strcmp(argv[1], "client") == 0) {
		handleClient(atoi(argv[2]));
		return EXIT_SUCCESS;
	} else {
		printf("ERROR: invalid option\n");
		return EXIT_FAILURE;
	}
}
