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
#define FILEBUF 1024*1024*4 // Max file size 4 MB

/*
	OPCODE GUIDE:
	01: File info packet, 2 bytes for opcode, 4 bytes for POSIX time, 16 bytes
		for MD5 hash, and up to 256 bytes for filename, including null char
	02: File request packet for last file info packet received, 2 bytes for
		opcode, 2 bytes of null chars
	03: File non-request packet for last file info packet received, 3 bytes for
		opcode, 2 bytes of null chars
	04: File packet, 2 bytes for opcode, arbitrary number of bytes for file
		contents
	05: Done sending files packet, 2 bytes for opcode, 2 bytes of null chars
*/

// Debugging function
void print_bytes(const void *object, size_t size) {
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
	// assume time arrives in network order
	return
		((unsigned char) buffer[0] << 24) +
		((unsigned char) buffer[1] << 16) +
		((unsigned char) buffer[2] << 8) +
		((unsigned char) buffer[3]);
}

int checkOpCode(char* buffer, int byte_count) {
	if (byte_count < 4) return -1; //2 bytes for opcode + 1 byte for EOS + 1byte for filename
	if ((short)buffer[0] != 0) return -1; //all opcodes start with 0
	return (int)buffer[1];
}

int handleServer(unsigned short port) {
	printf("Handling server on port %d\n", port);
	
	// fd is for sockets, fd2 is for files
	int fd, fd2, f_read, n;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
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

	struct sockaddr clientaddr;
	socklen_t len = sizeof(clientaddr);

	// buffer is for incoming messages, packet is for outgoing messages
	// filebuffer is for reading files
	char buffer[MAXBUF], packet[MAXBUF], filebuffer[FILEBUF];
	memset(&packet, 0, 4);

	char lastmodstr[4];
	int client_lastmod, server_lastmod;
	unsigned char server_md5[16], client_md5[16];
	char fname[256], fullpath[256];

	short opcode;
	struct stat filestat;

	//create the temporary directory
	char template[] = "./.serverXXXXXX";
	char* temp_dir_name = mkdtemp(template);
	printf("Created directory %s\n", temp_dir_name);
	
	
    while (1) {
		// Phase 1, receive files from client
		while (1) {
		    int length = recvfrom(fd, buffer, sizeof(buffer), 0, &clientaddr, &len);
		    if (length < 0) {
		        perror("recvfrom() failed");
		        break;
		    }
			if (checkOpCode(buffer, 4) == 5) {
				printf("Phase 1 done!\n");
				break;
			}
			memcpy(lastmodstr, buffer + 2, 4);
			client_lastmod = unpackTime(lastmodstr);
			memcpy(client_md5, buffer + 6, 16);
			memcpy(fname, buffer + 22, length - 22);
			printf("File %s was last modified %d\n", fname, server_lastmod);

			// this directory is temporary, will be fixed later
			strcpy(fullpath, temp_dir_name);
			strcpy(fullpath + 15, "/"); //15 from "./.serverXXXXXX"
			strcpy(fullpath + 16, fname); //16 from "./.serverXXXXXX/"
			printf("Attempting to access %s\n", fullpath);
			int res = stat(fullpath, &filestat);
			if (res == -1 && errno == ENOENT) {
				printf("%s doesn't exist on server\n", fname);
				opcode = htons(2);
				memcpy(packet, &opcode, 2);
				sendto(fd, packet, 4, 0, (struct sockaddr *)&clientaddr, len);
				length = recvfrom(fd, filebuffer, sizeof(filebuffer), 0, &clientaddr, &len);
				fd2 = open(fullpath, O_WRONLY | O_CREAT, 0644);
				pwrite(fd2, filebuffer + 2, length - 2, 0);
				close(fd2);
			} else {
				fd2 = open(fullpath, O_RDONLY, 0);
				f_read = pread(fd2, filebuffer, FILEBUF, 0);

				MD5((unsigned char *) filebuffer, f_read, server_md5);

				if (memcmp(server_md5, client_md5, 16) != 0) {
					printf("Hash between client and server are unequal\n");
					server_lastmod = (int) filestat.st_mtime;
					printf("%s exists on server and was last modified %d\n", fname, server_lastmod);
					if (client_lastmod > server_lastmod) {
						printf("I will request client for the file\n");
						opcode = htons(2);
						memcpy(packet, &opcode, 2);
						sendto(fd, packet, 4, 0, (struct sockaddr *)&clientaddr, len);
						length = recvfrom(fd, filebuffer, sizeof(filebuffer), 0, &clientaddr, &len);
						fd2 = open(fullpath, O_WRONLY | O_CREAT, 0644);
						pwrite(fd2, filebuffer + 2, length - 2, 0);
						close(fd2);
					} else {
						printf("My file is more recent, do nothing\n");
						opcode = htons(3);
						memcpy(packet, &opcode, 2);
						sendto(fd, packet, 4, 0, (struct sockaddr *)&clientaddr, len);
					}
				} else {
					printf("Hashes are equal, do nothing\n");
					opcode = htons(3);
					memcpy(packet, &opcode, 2);
					sendto(fd, packet, 4, 0, (struct sockaddr *)&clientaddr, len);
				}
			}
			printf("\n");
		}

		DIR *d;
		struct dirent *dir;
		d = opendir(temp_dir_name);
		if (d) {
			while ((dir = readdir(d)) != NULL) {
				if (dir->d_type == DT_REG) {

					opcode = htons(1);
					memcpy(buffer, &opcode, 2);

					strcpy(fullpath, temp_dir_name);
					strcpy(fullpath + 15, "/");
					strcpy(fullpath + 16, dir->d_name);

					fd2 = open(fullpath, O_RDONLY, 0);
					f_read = pread(fd2, filebuffer, FILEBUF, 0);

					fstat(fd2, &filestat);
					client_lastmod = htonl((int) filestat.st_mtime);
					memcpy(buffer + 2, &server_lastmod, 4);

					MD5((unsigned char *) filebuffer, f_read, server_md5);
					memcpy(buffer + 6, server_md5, 16);

					n = strlen(dir->d_name);
					memcpy(fname, dir->d_name, n);
					fname[n] = '\0';
					memcpy(buffer + 22, fname, n + 1);

					sendto(fd, buffer, n + 23, 0, &clientaddr, len);
					recvfrom(fd, packet, 4, 0, NULL, 0);

					if (checkOpCode(packet, 4) == 2) {
						opcode = htons(4);
						char filepacket[f_read + 2];
						memcpy(filepacket, &opcode, 2);
						memcpy(filepacket + 2, filebuffer, f_read);
						sendto(fd, filepacket, f_read + 2, 0, &clientaddr, len);
					}
					close(fd2);
				}
			}
			closedir(d);
			opcode = htons(5);
			memset(packet, 0, 4);
			memcpy(packet, &opcode, 2);
			sendto(fd, packet, 4, 0, &clientaddr, len);
		}

		printf("Phase 2 done!\n");
    }

    close(fd);
	return 0;
}

int handleClient(unsigned short port) {
	printf("Handling client on port %d\n", port);

	int n, fd, fd2, f_read;
    fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) {
        perror("socket() failed");
        return 1;
    }

    struct sockaddr_in serveraddr;
    memset( &serveraddr, 0, sizeof(serveraddr) );
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(port);              
    serveraddr.sin_addr.s_addr = htonl(0x7f000001); // connect to localhost
	socklen_t len = sizeof(serveraddr);
	
    char buffer[MAXBUF], packet[MAXBUF], filebuffer[FILEBUF];
	memset(&packet, 0, 4);

	char lastmodstr[4];
	int client_lastmod, server_lastmod;
	unsigned char client_md5[16], server_md5[16];
	char fname[256];

	short opcode;
	struct stat filestat;

	DIR *d;
	struct dirent *dir;
	d = opendir(".");
	if (d) {
		while ((dir = readdir(d)) != NULL) {
			if (dir->d_type == DT_REG) {

				opcode = htons(1);
				memcpy(buffer, &opcode, 2);

				fd2 = open(dir->d_name, O_RDONLY, 0);
				f_read = pread(fd2, filebuffer, FILEBUF, 0);

				fstat(fd2, &filestat);
				client_lastmod = htonl((int) filestat.st_mtime);
				memcpy(buffer + 2, &client_lastmod, 4);

				MD5((unsigned char *) filebuffer, f_read, client_md5);
				memcpy(buffer + 6, client_md5, 16);

				n = strlen(dir->d_name);
				memcpy(fname, dir->d_name, n);
				fname[n] = '\0';
				memcpy(buffer + 22, fname, n + 1);

				sendto(fd, buffer, n + 23, 0, (struct sockaddr *)&serveraddr, len);
				recvfrom(fd, packet, 4, 0, NULL, 0);

				if (checkOpCode(packet, 4) == 2) {
					opcode = htons(4);
					char filepacket[f_read + 2];
					memcpy(filepacket, &opcode, 2);
					memcpy(filepacket + 2, filebuffer, f_read);
					sendto(fd, filepacket, f_read + 2, 0, (struct sockaddr *)&serveraddr, len);
				} // otherwise send nothing
				close(fd2);
			}
		}
		closedir(d);
		opcode = htons(5);
		memset(packet, 0, 4);
		memcpy(packet, &opcode, 2);
		sendto(fd, packet, 4, 0, (struct sockaddr *)&serveraddr, len);
	}
	
	printf("Phase 1 done!\n");

	while (1) {
	    int length = recvfrom(fd, buffer, sizeof(buffer), 0, (struct sockaddr *)&serveraddr, &len);
	    if (length < 0) {
	        perror("recvfrom() failed");
	        break;
	    }
		if (checkOpCode(buffer, 4) == 5) {
			printf("Phase 2 done!\n");
			break;
		}
		memcpy(lastmodstr, buffer + 2, 4);
		client_lastmod = unpackTime(lastmodstr);
		memcpy(client_md5, buffer + 6, 16);
		memcpy(fname, buffer + 22, length - 22);
		printf("File %s was last modified %d\n", fname, server_lastmod);

		printf("Attempting to access %s\n", fname);
		int res = stat(fname, &filestat);
		if (res == -1 && errno == ENOENT) {
			printf("%s doesn't exist on client\n", fname);
			opcode = htons(2);
			memcpy(packet, &opcode, 2);
			sendto(fd, packet, 4, 0, (struct sockaddr *)&serveraddr, len);
			length = recvfrom(fd, filebuffer, sizeof(filebuffer), 0, (struct sockaddr *)&serveraddr, &len);
			fd2 = open(fname, O_WRONLY | O_CREAT, 0644);
			pwrite(fd2, filebuffer + 2, length - 2, 0);
			close(fd2);
		} else {
			fd2 = open(fname, O_RDONLY, 0);
			f_read = pread(fd2, filebuffer, FILEBUF, 0);

			MD5((unsigned char *) filebuffer, f_read, server_md5);

			if (memcmp(server_md5, client_md5, 16) != 0) {
				printf("Hash between client and server are unequal\n");
				server_lastmod = (int) filestat.st_mtime;
				printf("%s exists on client and was last modified %d\n", fname, server_lastmod);
				if (client_lastmod < server_lastmod) {
					printf("I will request server for the file\n");
					opcode = htons(2);
					memcpy(packet, &opcode, 2);
					sendto(fd, packet, 4, 0, (struct sockaddr *)&serveraddr, len);
					length = recvfrom(fd, filebuffer, sizeof(filebuffer), 0, (struct sockaddr *)&serveraddr, &len);
					fd2 = open(fname, O_WRONLY | O_CREAT, 0644);
					pwrite(fd2, filebuffer + 2, length - 2, 0);
					close(fd2);
				} else {
					printf("My file is more recent, do nothing\n");
					opcode = htons(3);
					memcpy(packet, &opcode, 2);
					sendto(fd, packet, 4, 0, (struct sockaddr *)&serveraddr, len);
				}
			} else {
				printf("Hashes are equal, do nothing\n");
				opcode = htons(3);
				memcpy(packet, &opcode, 2);
				sendto(fd, packet, 4, 0, (struct sockaddr *)&serveraddr, len);
			}
		}
		printf("\n");
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
