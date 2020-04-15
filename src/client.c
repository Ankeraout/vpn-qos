#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include <pthread.h>

#include <libtun/libtun.h>
#include <tunnel.h>

int overhead;
int waitScale;
const char *hostname;
int port;
char tunDeviceName[16];
int tun_fd;
tunnel_t tunnel;
struct sockaddr serverAddress;
int downloadBandwidth;
int uploadBandwidth;

int checkCommandLineParameters(int argc, const char *argv[]);
int connectToTheServer();

int resolveHostname(const char *hostname, in_addr_t *address) {
    struct hostent *hostEntry = gethostbyname(hostname);

    if(hostEntry == NULL) {
        return -1;
    }

    memcpy(address, hostEntry->h_addr_list[0], sizeof(in_addr_t));

    return 0;
}

int main(int argc, const char *argv[]) {
    if(checkCommandLineParameters(argc, argv)) {
        fprintf(stderr, "Command-line parameters analysis failed.\n");
        return EXIT_FAILURE;
    }

    printf("Download bandwidth: %d Bps\n", downloadBandwidth);
    printf("Upload bandwidth: %d Bps\n", uploadBandwidth);
    printf("Overhead: %d B\n", overhead);

    // Create the socket to the server
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if(sock < 0) {
        perror("Failed to create socket");
        return EXIT_FAILURE;
    }

    // Resolve server address
    struct sockaddr_in *serverSocketAddress = (struct sockaddr_in *)&serverAddress;

    memset(serverSocketAddress, 0, sizeof(struct sockaddr_in));
    
    if(resolveHostname(hostname, &serverSocketAddress->sin_addr.s_addr)) {
        fprintf(stderr, "Failed to resolve server hostname.\n");
        return EXIT_FAILURE;
    }

    serverSocketAddress->sin_family = AF_INET;
    serverSocketAddress->sin_port = htons(port);

    // Create the tun device
    tun_fd = libtun_open(tunDeviceName);

    if(tun_fd < 0) {
        fprintf(stderr, "Failed to open tun device.\n");
        return EXIT_FAILURE;
    }

    if(tunnel_init(&tunnel, sock, tun_fd, uploadBandwidth / 10, overhead, uploadBandwidth, &serverAddress)) {
        fprintf(stderr, "tunnel_init() failed.\n");
        return EXIT_FAILURE;
    }

    while(true) {
        // Connect to the server and negociate connection parameters
        if(connectToTheServer()) {
            fprintf(stderr, "Connection to the server failed.\n");
            continue;
        }

        // TODO: Create routes

        // Loop
        tunnel_mainLoop(&tunnel);

        // TODO: Remove routes
    }

    // Close tun device
    libtun_close(tun_fd);

    return EXIT_SUCCESS;
}

int checkCommandLineParameters(int argc, const char *argv[]) {
    bool flag_overhead = false;
    bool flag_downloadBandwidth = false;
    bool flag_uploadBandwidth = false;
    bool flag_hostname = false;
    bool flag_port = false;

    bool flag_set_overhead = false;
    bool flag_set_downloadBandwidth = false;
    bool flag_set_uploadBandwidth = false;
    bool flag_set_hostname = false;
    bool flag_set_port = false;

    for(int i = 1; i < argc; i++) {
        if(flag_overhead) {
            flag_overhead = false;

            if(sscanf(argv[i], "%d", &overhead) == EOF) {
                fprintf(stderr, "Failed to parse overhead value.\n");
                return -1;
            }

            if(overhead < 0 || overhead > 255) {
                fprintf(stderr, "Bad overhead value. Expected an integer between 0 and 128.");
                return -1;
            }

            flag_set_overhead = true;
        } else if(flag_downloadBandwidth) {
            flag_downloadBandwidth = false;

            if(sscanf(argv[i], "%d", &downloadBandwidth) == EOF) {
                fprintf(stderr, "Failed to parse download bandwidth value.\n");
                return -1;
            }

            if(downloadBandwidth <= 0) {
                fprintf(stderr, "Bad download bandwidth value. Expected a strictly positive integer.\n");
                return -1;
            }

            flag_set_downloadBandwidth = true;
        } else if(flag_uploadBandwidth) {
            flag_uploadBandwidth = false;

            if(sscanf(argv[i], "%d", &uploadBandwidth) == EOF) {
                fprintf(stderr, "Failed to parse upload bandwidth value.\n");
                return -1;
            }

            if(uploadBandwidth <= 0) {
                fprintf(stderr, "Bad upload bandwidth value. Expected a strictly positive integer.\n");
                return -1;
            }

            flag_set_uploadBandwidth = true;
        } else if(flag_hostname) {
            flag_hostname = false;
            hostname = argv[i];
            flag_set_hostname = true;
        } else if(flag_port) {
            flag_port = false;

            if(sscanf(argv[i], "%d", &port) == EOF) {
                fprintf(stderr, "Failed to parse port value.\n");
                return -1;
            }

            if(port < 0 || port > 65535) {
                fprintf(stderr, "Bad port value. Expected an integer between 0 and 65535.\n");
                return -1;
            }

            flag_set_port = true;
        } else if(strcmp(argv[i], "--overhead") == 0) {
            flag_overhead = true;
        } else if(strcmp(argv[i], "--download-bandwidth") == 0) {
            flag_downloadBandwidth = true;
        } else if(strcmp(argv[i], "--hostname") == 0) {
            flag_hostname = true;
        } else if(strcmp(argv[i], "--port") == 0) {
            flag_port = true;
        } else if(strcmp(argv[i], "--upload-bandwidth") == 0) {
            flag_uploadBandwidth = true;
        } else {
            fprintf(stderr, "Failed to parse argument \"%s\".", argv[i]);
            return 1;
        }
    }

    if(!flag_set_overhead) {
        fprintf(stderr, "The overhead value was not specified.\n");
        return -1;
    }

    if(!flag_set_downloadBandwidth) {
        fprintf(stderr, "The bandwidth value was not specified.\n");
        return -1;
    }

    if(!flag_set_uploadBandwidth) {
        fprintf(stderr, "The upload bandwidth value was not specified.\n");
        return -1;
    }

    if(!flag_set_hostname) {
        fprintf(stderr, "The hostname value was not specified.\n");
        return -1;
    }

    if(!flag_set_port) {
        fprintf(stderr, "The port value was not specified.\n");
        return -1;
    }

    return 0;
}

int attemptConnection() {
    uint8_t buffer[5];

    *(uint32_t *)buffer = htonl(downloadBandwidth);
    buffer[4] = overhead;

    if(sendto(tunnel.sock_fd, buffer, 5, 0, &serverAddress, sizeof(struct sockaddr_in)) == -1) {
        perror("sendto() failed while logging in.\n");
        return 1;
    }

    struct sockaddr socketAddress;
    socklen_t socklen;
    if(recvfrom(tunnel.sock_fd, buffer, 5, 0, &socketAddress, &socklen) == -1) {
        perror("recvfrom() failed while logging in.\n");
        return 1;
    }

    return 0;
}

int connectToTheServer() {
    for(int i = 0; i < 3; i++) {
        if(!attemptConnection()) {
            printf("Connected to the server.\n");
            return 0;
        }
    }

    return -1;
}
