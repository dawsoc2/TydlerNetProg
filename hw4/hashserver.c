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
#if defined(__APPLE__)
#  define COMMON_DIGEST_FOR_OPENSSL
#  include <CommonCrypto/CommonDigest.h>
#  define SHA1 CC_SHA1
#else
#  include <openssl/md5.h>
#endif

#define MAXBUF 1024
#define FILEBUF 1024*1024*4 // Max file size 4 MB

/*
	OPCODE GUIDE:
	01: File info packet, 2 bytes for opcode, 4 bytes for POSIX time, 32 bytes
		for MD5 hash, and up to 256 bytes for filename, including null char
	02: File request packet for last file info packet received, 2 bytes for
		opcode, 2 bytes of null chars
	03: File non-request packet for last file info packet received, 3 bytes for
		opcode, 2 bytes of null chars
	04: File packet, 2 bytes for opcode, arbitrary number of bytes for file
		contents
	05: Done sending files packet, 2 bytes for opcode, 2 bytes of null chars
*/

// converts 16 byte MD5 to 32 byte ASCII
char * bytesToStr(unsigned char * md5) {
	int i = 0;
	static char outmd5[32];
	for(i = 0; i < 16; i++) {
		sprintf(outmd5 + i * 2, "%02x", md5[i]);
	}
	return outmd5;
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
	if (byte_count < 4) return -1;
	if ((short)buffer[0] != 0) return -1;
	return (int)buffer[1];
}

int handleServer(unsigned short port) {
	// fd is for sockets, fd2 is for files
	int fd, fd2, f_read, n;
	FILE * listfile;

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
	char client_md5[33], server_md5[33];
	char fname[256], fullpath[256], listbuffer[292];

	short opcode;
	struct stat filestat;
	
	char template[] = "./.serverXXXXXX";
	char* temp_dir_name = mkdtemp(template);
	char listfile_path[36];
	strcpy(listfile_path, temp_dir_name);
	strcpy(listfile_path + 15, "/.4220_file_list.txt");

	strcpy(fullpath, temp_dir_name);
	strcpy(fullpath + 15, "/"); //15 from "./.serverXXXXXX"

	// creates a file if none exists, C has no good way to do this
	listfile = fopen(listfile_path, "a+");
	fclose(listfile);

    while (1) {
		// phase 1, receive files from client
		listfile = fopen(listfile_path, "r+");
		while (1) {
		    int length = recvfrom(fd, buffer, sizeof(buffer),
				0, &clientaddr, &len);
		    if (length < 0) {
		        perror("recvfrom() failed");
		        break;
		    }
			if (checkOpCode(buffer, 4) == 5) {
				// phase 1 done
				break;
			}
			memcpy(lastmodstr, buffer + 2, 4);
			client_lastmod = unpackTime(lastmodstr);

			memcpy(client_md5, buffer + 6, 32);
			client_md5[32] = '\0';
			memcpy(fname, buffer + 38, length - 38);

			strcpy(fullpath + 16, fname); //16 from "./.serverXXXXXX/"
			int fileexists = 0;
			long linepos = 0; // keep track of each line start position
			fseek(listfile, 0, SEEK_SET);
			while (fgets(listbuffer, 276, listfile) != NULL) {
				if (strlen(listbuffer) - 37 == strlen(fname)) {
		   			if (strncmp(listbuffer + 36, fname, strlen(fname)) == 0) {
						fileexists = 1;
						break;
					}
				}
				linepos = ftell(listfile);
			}

			if (fileexists == 0) {
				printf("[server] Detected new file: %s\n", fname);
				printf("[server] Downloading %s: %s\n", fname, client_md5);
				opcode = htons(2);
				memcpy(packet, &opcode, 2);
				sendto(fd, packet, 4, 0, (struct sockaddr *)&clientaddr, len);
				length = recvfrom(fd, filebuffer, sizeof(filebuffer),
					0, &clientaddr, &len);
				fd2 = open(fullpath, O_WRONLY | O_CREAT, 0644);
				pwrite(fd2, filebuffer + 2, length - 2, 0);
				close(fd2);
				// write new info to list file
				fwrite(client_md5, 1, 32, listfile);
				fprintf(listfile, "    ");
				fwrite(fname, 1, strlen(fname), listfile);
				fprintf(listfile, "\n");
			} else {
				fseek(listfile, linepos, SEEK_SET);
				fread(server_md5, 1, 32, listfile);	
				server_md5[32] = '\0';				

				if (strncmp(server_md5, client_md5, 32) != 0) {
					stat(fullpath, &filestat);
					server_lastmod = (int) filestat.st_mtime;
					if (client_lastmod > server_lastmod) {
						printf("[server] Detected different & newer file %s\n",
							fname);
						printf("[server] Downloading %s: %s\n",
							fname, client_md5);
						opcode = htons(2);
						memcpy(packet, &opcode, 2);
						sendto(fd, packet, 4,
							0, (struct sockaddr *)&clientaddr, len);
						length = recvfrom(fd, filebuffer, sizeof(filebuffer),
							0, &clientaddr, &len);
						fd2 = open(fullpath, O_WRONLY | O_TRUNC, 0644);
						pwrite(fd2, filebuffer + 2, length - 2, 0);
						close(fd2);
						// write new info to list file by overwriting old info
						fseek(listfile, linepos, SEEK_SET);
						fwrite(client_md5, 1, 32, listfile);
					} else {
						opcode = htons(3);
						memcpy(packet, &opcode, 2);
						sendto(fd, packet, 4,
							0, (struct sockaddr *)&clientaddr, len);
					}
				} else {
					opcode = htons(3);
					memcpy(packet, &opcode, 2);
					sendto(fd, packet, 4,
						0, (struct sockaddr *)&clientaddr, len);
				}
			}
		}

		// phase 2, send files to client
		fseek(listfile, 0, SEEK_SET);
		while (fgets(listbuffer, 276, listfile) != NULL) {
			opcode = htons(1);
			memcpy(buffer, &opcode, 2);

			strncpy(server_md5, listbuffer, 32);
			n = strlen(listbuffer) - 37;
			strncpy(fname, listbuffer + 36, n);
			fname[n] = '\0';

			strcpy(fullpath + 16, fname); //16 from "./.serverXXXXXX/"

			fd2 = open(fullpath, O_RDONLY, 0);

			fstat(fd2, &filestat);
			server_lastmod = htonl((int) filestat.st_mtime);

			memcpy(buffer + 2, &server_lastmod, 4);
			memcpy(buffer + 6, server_md5, 32);
			memcpy(buffer + 38, fname, n + 1);

			sendto(fd, buffer, n + 39, 0, &clientaddr, len);
			recvfrom(fd, packet, 4, 0, NULL, 0);

			if (checkOpCode(packet, 4) == 2) {
				opcode = htons(4);
				f_read = pread(fd2, filebuffer, FILEBUF, 0);
				char filepacket[f_read + 2];
				memcpy(filepacket, &opcode, 2);
				memcpy(filepacket + 2, filebuffer, f_read);
				sendto(fd, filepacket, f_read + 2, 0, &clientaddr, len);
			} // otherwise send nothing
			close(fd2);
		}

		opcode = htons(5);
		memset(packet, 0, 4);
		memcpy(packet, &opcode, 2);
		sendto(fd, packet, 4, 0, &clientaddr, len);

		fclose(listfile);
    }

    close(fd);
	return 0;
}

int handleClient(unsigned short port) {
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
	unsigned char bytes_md5[16];
	char server_md5[33];
	char fname[256];

	short opcode;
	struct stat filestat;

	// phase 1, send files to server
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

				MD5((unsigned char *) filebuffer, f_read, bytes_md5);
				memcpy(buffer + 6, bytesToStr(bytes_md5), 32);

				n = strlen(dir->d_name);
				memcpy(fname, dir->d_name, n);
				fname[n] = '\0';
				memcpy(buffer + 38, fname, n + 1);

				sendto(fd, buffer, n + 39,
					0, (struct sockaddr *)&serveraddr, len);
				recvfrom(fd, packet, 4, 0, NULL, 0);

				if (checkOpCode(packet, 4) == 2) {
					opcode = htons(4);
					char filepacket[f_read + 2];
					memcpy(filepacket, &opcode, 2);
					memcpy(filepacket + 2, filebuffer, f_read);
					sendto(fd, filepacket, f_read + 2,
						0, (struct sockaddr *)&serveraddr, len);
				}
				close(fd2);
			}
		}
		closedir(d);
		opcode = htons(5);
		memset(packet, 0, 4);
		memcpy(packet, &opcode, 2);
		sendto(fd, packet, 4, 0, (struct sockaddr *)&serveraddr, len);
	}

	// phase 2, receive files from server
	while (1) {
	    int length = recvfrom(fd, buffer, sizeof(buffer),
			0, (struct sockaddr *)&serveraddr, &len);
	    if (length < 0) {
	        perror("recvfrom() failed");
	        break;
	    }
		if (checkOpCode(buffer, 4) == 5) {
			// phase 2 done
			break;
		}
		memcpy(lastmodstr, buffer + 2, 4);
		server_lastmod = unpackTime(lastmodstr);
		memcpy(server_md5, buffer + 6, 32);
		server_md5[32] = '\0';
		memcpy(fname, buffer + 38, length - 38);

		int res = stat(fname, &filestat);
		if (res == -1 && errno == ENOENT) {
			printf("[client] Detected new file: %s\n", fname);
			printf("[client] Downloading %s: %s\n", fname, server_md5);
			opcode = htons(2);
			memcpy(packet, &opcode, 2);
			sendto(fd, packet, 4, 0, (struct sockaddr *)&serveraddr, len);
			length = recvfrom(fd, filebuffer, sizeof(filebuffer),
				0, (struct sockaddr *)&serveraddr, &len);
			fd2 = open(fname, O_WRONLY | O_CREAT, 0644);
			pwrite(fd2, filebuffer + 2, length - 2, 0);
			close(fd2);
		} else {
			fd2 = open(fname, O_RDONLY, 0);
			f_read = pread(fd2, filebuffer, FILEBUF, 0);

			MD5((unsigned char *) filebuffer, f_read, bytes_md5);

			if (strncmp(server_md5, bytesToStr(bytes_md5), 32) != 0) {
				client_lastmod = (int) filestat.st_mtime;
				if (client_lastmod < server_lastmod) {
					printf("[client] Detected different & newer file %s\n",
						fname);
					printf("[client] Downloading %s: %s\n",
						fname, server_md5);
					opcode = htons(2);
					memcpy(packet, &opcode, 2);
					sendto(fd, packet, 4,
						0, (struct sockaddr *)&serveraddr, len);
					length = recvfrom(fd, filebuffer, sizeof(filebuffer),
						0, (struct sockaddr *)&serveraddr, &len);
					fd2 = open(fname, O_WRONLY | O_TRUNC, 0644);
					pwrite(fd2, filebuffer + 2, length - 2, 0);
					close(fd2);
				} else {
					opcode = htons(3);
					memcpy(packet, &opcode, 2);
					sendto(fd, packet, 4,
						0, (struct sockaddr *)&serveraddr, len);
				}
			} else {
				opcode = htons(3);
				memcpy(packet, &opcode, 2);
				sendto(fd, packet, 4, 0, (struct sockaddr *)&serveraddr, len);
			}
		}
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
