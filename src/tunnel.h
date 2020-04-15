#ifndef __TUNNEL_H_INCLUDED__
#define __TUNNEL_H_INCLUDED__

#include <stdint.h>
#include <pthread.h>
#include <semaphore.h>

#define TUNNEL_MAX_PACKET_SIZE 1500
#define TUNNEL_QUEUE_COUNT 2

typedef struct {
    uint16_t packetSize;
    uint8_t buffer[TUNNEL_MAX_PACKET_SIZE];
} packet_t;

struct queue_element_s;

typedef struct queue_element_s {
    packet_t packet;
    struct queue_element_s *next;
} queue_element_t;

typedef struct {
    queue_element_t *freeQueueElements;
    queue_element_t *queues[TUNNEL_QUEUE_COUNT];
    int capacity;
    int size;
    pthread_mutex_t mutex;
    pthread_mutexattr_t mutexAttributes;
    sem_t dequeueSemaphore;
} queue_t;

typedef struct {
    int sock_fd;
    int tun_fd;
    int overhead;
    int bandwidth;
    pthread_t tunEnqueueThread;
    pthread_t tunReceivingThread;
    pthread_t tunDequeueThread;
    pthread_attr_t tunEnqueueThreadAttributes;
    pthread_attr_t tunReceivingThreadAttributes;
    pthread_attr_t tunDequeueThreadAttributes;
    struct sockaddr otherEndSocketAddress;
    queue_t queue;
} tunnel_t;

int tunnel_init(tunnel_t *tunnel, int sock_fd, int tun_fd, int queueCapacity, int overhead, int bandwidth, const struct sockaddr *otherEndSocketAddress);
void tunnel_mainLoop(tunnel_t *tunnel);

#endif
