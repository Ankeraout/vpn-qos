#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <pthread.h>

#include <libtun/libtun.h>
#include <tunnel.h>

int tun_fd;
char tunDeviceName[16];
tunnel_t tunnel;

int main() {
    // Create the socket to the server
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if(sock < 0) {
        perror("Failed to create socket");
        return EXIT_FAILURE;
    }

    struct sockaddr_in socketAddress;
    memset(&socketAddress, 0, sizeof(struct sockaddr_in));
    socketAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    socketAddress.sin_family = AF_INET;
    socketAddress.sin_port = htons(5976);

    if(bind(sock, (const struct sockaddr *)&socketAddress, sizeof(struct sockaddr_in))) {
        perror("Failed to bind socket");
        return EXIT_FAILURE;
    }

    // Create the tun device
    tun_fd = libtun_open(tunDeviceName);

    printf("tun_fd=%d\n", tun_fd);

    if(tun_fd < 0) {
        fprintf(stderr, "Failed to open tun device.\n");
        return EXIT_FAILURE;
    }

    socklen_t socklen = sizeof(struct sockaddr);
    uint8_t buffer[5];
    ssize_t packetSize = recvfrom(sock, buffer, 5, 0, (struct sockaddr *)&socketAddress, &socklen);

    if(packetSize < 0) {
        perror("recvfrom() failed.\n");
        return EXIT_FAILURE;
    } else if(packetSize != 5) {
        fprintf(stderr, "Wrong packet size, expected 5, got %d.\n", (int)packetSize);
        return 0;
    }

    int bandwidth = ntohl(*(uint32_t *)buffer);
    int overhead = buffer[4];

    printf("Bandwidth: %d bps\n", bandwidth);
    printf("Overhead: %d bytes\n", overhead);

    if(tunnel_init(&tunnel, sock, tun_fd, 16384, overhead, bandwidth, (const struct sockaddr *)&socketAddress)) {
        fprintf(stderr, "tunnel_init() failed.\n");
        return EXIT_FAILURE;
    }

    ssize_t result = sendto(sock, buffer, 5, 0, (const struct sockaddr *)&socketAddress, sizeof(struct sockaddr_in));

    if(result == -1) {
        perror("sendto() failed.\n");
        return EXIT_FAILURE;
    }

    tunnel_mainLoop(&tunnel);

    return EXIT_SUCCESS;
}
