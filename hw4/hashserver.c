#include <dirent.h>
#include <errno.h>
#include <netdb.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h> /* INET constants and stuff */
#include <arpa/inet.h>  /* IP address conversion stuff */

#define MAXBUF 1024

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
    while (1) {
        int length = recvfrom(fd, buffer, sizeof(buffer) - 1, 0, NULL, 0);
        if (length < 0) {
            perror("recvfrom() failed");
            break;
        }
        buffer[length] = '\0';
        printf("%d bytes: '%s'\n", length, buffer);
    }

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

    int len, n, n_sent;
    char bufin[MAXBUF];
	len = sizeof(serveraddr);

	DIR *d;
	struct dirent *dir;
	d = opendir(".");
	if (d) {
		while ((dir = readdir(d)) != NULL) {
			if (dir->d_type == DT_REG) {
				n = strlen(dir->d_name);
				memcpy(bufin, dir->d_name, n);
				n_sent = sendto(fd, bufin, n, 0, (struct sockaddr *)&serveraddr, len);
				printf("Sent \"%s\"\n", dir->d_name);
				if (n_sent != n) {
					printf("sendto() failed\n");
				}
			}
		}
		closedir(d);
	}
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