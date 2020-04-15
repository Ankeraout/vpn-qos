#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <tunnel.h>

static void *tunEnqueueThreadMain(void *arg);
static void *tunDequeueThreadMain(void *arg);
static void *tunReceivingThreadMain(void *arg);

static inline unsigned int getMicroseconds() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000 + tv.tv_usec;
}

static inline int getQueueBacklog(queue_element_t *queue) {
    if(queue == NULL) {
        return 0;
    } else {
        return 1 + getQueueBacklog(queue->next);
    }
}

static inline queue_element_t *getLastQueueElement(queue_element_t *queue) {
    if(queue) {
        if(queue->next) {
            return getLastQueueElement(queue->next);
        } else {
            return queue;
        }
    } else {
        return NULL;
    }
}

void tunnel_mainLoop(tunnel_t *tunnel) {
    // Create tun enqueue thread
    if(pthread_attr_init(&tunnel->tunEnqueueThreadAttributes)) {
        fprintf(stderr, "pthread_attr_init() failed for tun enqueue thread.\n");
        return;
    }

    if(pthread_create(&tunnel->tunEnqueueThread, &tunnel->tunEnqueueThreadAttributes, &tunEnqueueThreadMain, tunnel)) {
        fprintf(stderr, "pthread_create() failed while creating tun enqueue thread.\n");
        pthread_attr_destroy(&tunnel->tunEnqueueThreadAttributes);
        return;
    }

    // Create tun dequeue thread
    if(pthread_attr_init(&tunnel->tunDequeueThreadAttributes)) {
        fprintf(stderr, "pthread_attr_init() failed for tun dequeue thread.\n");
        pthread_attr_destroy(&tunnel->tunEnqueueThreadAttributes);
        pthread_cancel(tunnel->tunEnqueueThread);
        return;
    }

    if(pthread_create(&tunnel->tunDequeueThread, &tunnel->tunDequeueThreadAttributes, &tunDequeueThreadMain, tunnel)) {
        fprintf(stderr, "pthread_create() failed while creating tun dequeue thread.\n");
        pthread_attr_destroy(&tunnel->tunEnqueueThreadAttributes);
        pthread_cancel(tunnel->tunEnqueueThread);
        pthread_attr_destroy(&tunnel->tunDequeueThreadAttributes);
        return;
    }

    // Create receiver thread
    if(pthread_attr_init(&tunnel->tunReceivingThreadAttributes)) {
        fprintf(stderr, "pthread_attr_init() failed to tun receiving thread.\n");
        pthread_attr_destroy(&tunnel->tunEnqueueThreadAttributes);
        pthread_cancel(tunnel->tunEnqueueThread);
        pthread_attr_destroy(&tunnel->tunDequeueThreadAttributes);
        pthread_cancel(tunnel->tunDequeueThread);
        return;
    }

    if(pthread_create(&tunnel->tunReceivingThread, &tunnel->tunReceivingThreadAttributes, &tunReceivingThreadMain, tunnel)) {
        fprintf(stderr, "pthread_create() failed while creating receiving thread.\n");
        pthread_attr_destroy(&tunnel->tunEnqueueThreadAttributes);
        pthread_cancel(tunnel->tunEnqueueThread);
        pthread_attr_destroy(&tunnel->tunDequeueThreadAttributes);
        pthread_cancel(tunnel->tunDequeueThread);
        pthread_attr_destroy(&tunnel->tunReceivingThreadAttributes);
        return;
    }

    pthread_join(tunnel->tunEnqueueThread, NULL);
    pthread_join(tunnel->tunDequeueThread, NULL);
    pthread_join(tunnel->tunReceivingThread, NULL);
}

int queue_init(queue_t *queue, int capacity, int backlog) {
    if(capacity <= 0) {
        fprintf(stderr, "queue_init() failed because the specified queue capacity (%d) was invalid.\n", capacity);
        return 1;
    }
    
    if(backlog <= 0) {
        fprintf(stderr, "queue_init() failed because the specified backlog value (%d) was invalid.\n", backlog);
        return 1;
    }

    if(sem_init(&queue->dequeueSemaphore, 0, 0)) {
        fprintf(stderr, "sem_init() failed while creating queue semaphore.\n");
        return 1;
    }

    if(pthread_mutexattr_init(&queue->mutexAttributes)) {
        fprintf(stderr, "pthread_mutexattr_init() failed while creating queue mutex.\n");
        sem_destroy(&queue->dequeueSemaphore);
        return 1;
    }

    if(pthread_mutex_init(&queue->mutex, &queue->mutexAttributes)) {
        fprintf(stderr, "pthread_mutex_init() failed while creating queue mutex.\n");
        sem_destroy(&queue->dequeueSemaphore);
        pthread_mutexattr_destroy(&queue->mutexAttributes);
        return 1;
    }

    // Create queue backlog
    for(int i = 0; i < backlog; i++) {
        queue_element_t *element = malloc(sizeof(queue_element_t));

        if(!element) {
            perror("An error occurred while allocating memory for queue backlog");

            while(queue->freeQueueElements) {
                queue_element_t *next = queue->freeQueueElements->next;
                free(queue->freeQueueElements);
                queue->freeQueueElements = next;
            }
            
            sem_destroy(&queue->dequeueSemaphore);
            pthread_mutexattr_destroy(&queue->mutexAttributes);
            pthread_mutex_destroy(&queue->mutex);

            return 1;
        }

        element->next = queue->freeQueueElements;
        queue->freeQueueElements = element;
    }

    queue->capacity = capacity;
    queue->size = 0;

    return 0;
}

void queue_destroy(queue_t *queue) {
    sem_destroy(&queue->dequeueSemaphore);
    pthread_mutexattr_destroy(&queue->mutexAttributes);
    pthread_mutex_destroy(&queue->mutex);

    while(queue->freeQueueElements) {
        queue_element_t *next = queue->freeQueueElements->next;
        free(queue->freeQueueElements);
        queue->freeQueueElements = next;
    }

    queue->size = 0;
    queue->capacity = 0;
}

int queue_enqueue_tryReject(queue_t *queue, int priority) {
    printf("queue_enqueue_tryReject() called\n");

    for(int p = TUNNEL_QUEUE_COUNT - 1; p > priority; p--) {
        if(queue->queues[p]) {
            queue_element_t *e = queue->queues[p];
            queue->queues[p] = e->next;
            e->next = queue->freeQueueElements;
            queue->freeQueueElements = e;

            packet_t *packet = &e->packet;

            queue->size -= packet->packetSize;

            printf("Dropped 1 packet from queue %d for a packet in queue %d\n", p, priority);

            sem_wait(&queue->dequeueSemaphore);

            return 0;
        }
    }
    printf("no packets were dropped.\n");

    return 1;
}

int queue_enqueue(queue_t *queue, packet_t *packet, int priority) {
    pthread_mutex_lock(&queue->mutex);

    if(!queue->freeQueueElements) {
        if(queue_enqueue_tryReject(queue, priority)) {
            printf("Failed to enqueue packet with priority %d (no remaining backlog).\n", priority);
            printf("Queue 0: %d\nQueue 1: %d\nFree: %d\n", getQueueBacklog(queue->queues[0]), getQueueBacklog(queue->queues[1]), getQueueBacklog(queue->freeQueueElements));
            pthread_mutex_unlock(&queue->mutex);
            return 1;
        }
    }

    while(packet->packetSize + queue->size > queue->capacity) {
        if(queue_enqueue_tryReject(queue, priority)) {
            printf("Failed to enqueue packet with priority %d (queue is saturated).\n", priority);
            pthread_mutex_unlock(&queue->mutex);
            return 1;
        }
    }

    queue_element_t *temporaryElement = queue->freeQueueElements;
    queue->freeQueueElements = queue->freeQueueElements->next;
    memcpy(&temporaryElement->packet, packet, sizeof(packet_t));
    temporaryElement->next = NULL;

    queue_element_t *previousElement = getLastQueueElement(queue->queues[priority]);

    if(previousElement) {
        previousElement->next = temporaryElement;
    } else {
        queue->queues[priority] = temporaryElement;
    }

    queue->size += packet->packetSize;

    sem_post(&queue->dequeueSemaphore);

    pthread_mutex_unlock(&queue->mutex);

    return 0;
}

packet_t *queue_dequeue(queue_t *queue) {
    sem_wait(&queue->dequeueSemaphore);

    pthread_mutex_lock(&queue->mutex);

    int i = 0;
    bool found = false;
    packet_t *packet = NULL;

    while(!found && i < TUNNEL_QUEUE_COUNT) {
        if(queue->queues[i]) {
            found = true;
            
            queue_element_t *e = queue->queues[i];
            queue->queues[i] = e->next;
            e->next = queue->freeQueueElements;
            queue->freeQueueElements = e;

            packet = &e->packet;

            queue->size -= packet->packetSize;
        }

        i++;
    }

    pthread_mutex_unlock(&queue->mutex);

    return packet;
}

int tunnel_init(tunnel_t *tunnel, int sock_fd, int tun_fd, int queueCapacity, int overhead, int bandwidth, const struct sockaddr *otherEndSocketAddress) {
    tunnel->sock_fd = sock_fd;
    tunnel->tun_fd = tun_fd;
    tunnel->overhead = overhead;
    tunnel->bandwidth = bandwidth;

    memcpy(&tunnel->otherEndSocketAddress, otherEndSocketAddress, sizeof(struct sockaddr));
    
    if(queue_init(&tunnel->queue, queueCapacity, 100)) {
        fprintf(stderr, "Queue initialization failed.\n");
        return 1;
    }

    return 0;
}

static void *tunEnqueueThreadMain(void *arg) {
    tunnel_t *tunnel = (tunnel_t *)arg;
    packet_t packet;

    while(true) {
        ssize_t size = read(tunnel->tun_fd, packet.buffer, TUNNEL_MAX_PACKET_SIZE);

        if(size == -1) {
            perror("An error occurred while reading from tun device");
            break;
        } else if(size == 0) {
            fprintf(stderr, "Exiting enqueue thread because EOF was received from tun device.\n");
            break;
        }

        packet.packetSize = size;

        // Determine IP version
        int ipVersion = packet.buffer[4] >> 4;
        int protocol;

        if(ipVersion == 4) {
            protocol = packet.buffer[13];
        } else if(ipVersion == 6) {
            protocol = packet.buffer[10];
        } else {
            printf("Unknown IP version!\n");
            continue;
        }

        int priority = protocol == 6;

        printf("Enqueuing paquet with type %d and priority %d.\n", protocol, priority);

        queue_enqueue(&tunnel->queue, &packet, protocol == 6);
    }

    return NULL;
}

static void *tunDequeueThreadMain(void *arg) {
    tunnel_t *tunnel = (tunnel_t *)arg;

    while(true) {
        packet_t *packet = queue_dequeue(&tunnel->queue);

        if(packet) {
            unsigned int totalSize = packet->packetSize + tunnel->overhead;
            unsigned int transmitTime = (totalSize * 1000000) / tunnel->bandwidth;
            unsigned int currentTimestamp = getMicroseconds();

            if(sendto(tunnel->sock_fd, packet->buffer, packet->packetSize, 0, &tunnel->otherEndSocketAddress, sizeof(struct sockaddr_in)) == -1) {
                perror("An error occurred sending data through the socket");
                break;
            }

            // Wait until the packet is received
            while(getMicroseconds() - currentTimestamp < transmitTime);
        }
    }

    return NULL;
}

static void *tunReceivingThreadMain(void *arg) {
    tunnel_t *tunnel = (tunnel_t *)arg;
    uint8_t packetBuffer[TUNNEL_MAX_PACKET_SIZE];
    struct sockaddr address;
    socklen_t addressLength = sizeof(struct sockaddr_in);

    while(true) {
        ssize_t size = recvfrom(tunnel->sock_fd, packetBuffer, TUNNEL_MAX_PACKET_SIZE, 0, &address, &addressLength);

        if(size == -1) {
            perror("An error occurred while reading from socket");
            break;
        } else if(size == 0) {
            fprintf(stderr, "Exiting receiving thread because EOF was received from socket.\n");
            break;
        }

        if(memcmp(&address, &tunnel->otherEndSocketAddress, addressLength) == 0) {
            if(write(tunnel->tun_fd, packetBuffer, size) == -1) {
                perror("write() failed on tun device");
                //break;
            } else {
                printf("Successfully received packet.\n");
            }
        } else {
            printf("Ignored packet with wrong socket address.\n");
        }
    }

    return NULL;
}
