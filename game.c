#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <signal.h>
#include <errno.h>

#define MAX_LINE 1024
#define INITIAL_QUEUE_SIZE 4
#define BACKLOG 10
#define SECRET_NUM_MAX 9999

typedef struct {
    char *data;
    size_t len;
    size_t sent;
} Message;

typedef struct {
    int fd;
    int id;
    char rbuf[MAX_LINE];
    size_t rpos;
    Message *wqueue;
    size_t wq_size;
    size_t wq_cap;
} Client;

typedef struct {
    int *fds;
    size_t count;
    size_t capacity;
} WaitQueue;

// Global state
static volatile sig_atomic_t running = 1;
static int welcome_fd = -1;
static int target;
static unsigned max_players;
static Client *clients;
static int *avail_ids;
static size_t avail_count;
static WaitQueue waitq = {NULL, 0, 0};

void cleanup() {
    close(welcome_fd);
    for (size_t i = 0; i < max_players; i++) {
        if (clients[i].fd != -1) {
            close(clients[i].fd);
            free(clients[i].wqueue);
        }
    }
    for (size_t i = 0; i < waitq.count; i++) close(waitq.fds[i]);
    free(clients);
    free(avail_ids);
    free(waitq.fds);
}

void sigint_handler(int sig) {
    running = 0;
}

void enqueue_message(Client *c, const char *msg, size_t len) {
    if (c->wq_size >= c->wq_cap) {
        size_t new_cap = c->wq_cap ? c->wq_cap * 2 : INITIAL_QUEUE_SIZE;
        Message *newq = realloc(c->wqueue, new_cap * sizeof(Message));
        if (!newq) {
            perror("realloc");
            return;
        }
        c->wqueue = newq;
        c->wq_cap = new_cap;
    }

    char *copy = strndup(msg, len);
    if (!copy) {
        perror("strndup");
        return;
    }

    c->wqueue[c->wq_size] = (Message){copy, len, 0};
    c->wq_size++;
}

void broadcast(const char *msg, int exclude_id) {
    for (size_t i = 0; i < max_players; i++) {
        if (clients[i].fd != -1 && clients[i].id != exclude_id) {
            enqueue_message(&clients[i], msg, strlen(msg));
        }
    }
}

void reset_game() {
    for (size_t i = 0; i < max_players; i++) {
        if (clients[i].fd != -1) {
            close(clients[i].fd);
            clients[i].fd = -1;
            free(clients[i].wqueue);
            clients[i].wqueue = NULL;
            clients[i].wq_size = clients[i].wq_cap = 0;
        }
    }
    
    avail_count = max_players;
    for (size_t i = 0; i < max_players; i++) 
        avail_ids[i] = i + 1;

    target = (rand() % SECRET_NUM_MAX) + 1;
    printf("DEBUG: Server started. Secret number is %d\n", target);

}

void promote_waiting() {
    while (avail_count > 0 && waitq.count > 0) {
        int new_fd = waitq.fds[0];
        memmove(waitq.fds, waitq.fds + 1, (waitq.count - 1) * sizeof(int));
        waitq.count--;

        int id = avail_ids[--avail_count];
        size_t slot = id - 1;

        clients[slot].fd = new_fd;
        clients[slot].id = id;
        clients[slot].rpos = 0;
        clients[slot].wqueue = NULL;
        clients[slot].wq_size = clients[slot].wq_cap = 0;

        char welcome[64];
        snprintf(welcome, sizeof(welcome), "Welcome to the game, your id is %d\n", id);
        enqueue_message(&clients[slot], welcome, strlen(welcome));

        char notify[64];
        snprintf(notify, sizeof(notify), "Player %d joined the game\n", id);
        broadcast(notify, id);
    }
}

void handle_disconnect(size_t idx) {
    char msg[64];
    snprintf(msg, sizeof(msg), "Player %d disconnected\n", clients[idx].id);
    broadcast(msg, clients[idx].id);

    close(clients[idx].fd);
    clients[idx].fd = -1;
    avail_ids[avail_count++] = clients[idx].id;
    
    free(clients[idx].wqueue);
    clients[idx].wqueue = NULL;
    clients[idx].wq_size = clients[idx].wq_cap = 0;

    promote_waiting();
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <port> <seed> <max-players>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int port = atoi(argv[1]);
    if (port < 1 || port > 65535) {
        fprintf(stderr, "Invalid port number\n");
        return EXIT_FAILURE;
    }

    unsigned seed = atoi(argv[2]);
    max_players = atoi(argv[3]);
    if (max_players <= 1) {
        fprintf(stderr, "Max players must be >1\n");
        return EXIT_FAILURE;
    }

    srand(seed);
    target = (rand() % SECRET_NUM_MAX) + 1;

    // Initialize client structures
    clients = calloc(max_players, sizeof(Client));
    avail_ids = malloc(max_players * sizeof(int));
    for (size_t i = 0; i < max_players; i++) {
        clients[i].fd = -1;
        avail_ids[i] = i + 1;
    }
    avail_count = max_players;

    // Setup server socket
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = INADDR_ANY
    };

    if ((welcome_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int opt = 1;
    if (setsockopt(welcome_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        return EXIT_FAILURE;
    }

    if (bind(welcome_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return EXIT_FAILURE;
    }

    if (listen(welcome_fd, BACKLOG) < 0) {
        perror("listen");
        return EXIT_FAILURE;
    }

    printf("Server is ready to read from welcome socket %d\n", welcome_fd);
    signal(SIGINT, sigint_handler);
    printf("DEBUG: Server started. Secret number is %d\n", target);

    fd_set readfds, writefds;
    while (running) {
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);
        int max_fd = welcome_fd;

        FD_SET(welcome_fd, &readfds);

        // Active clients
        for (size_t i = 0; i < max_players; i++) {
            if (clients[i].fd != -1) {
                FD_SET(clients[i].fd, &readfds);
                if (clients[i].wq_size > 0)
                    FD_SET(clients[i].fd, &writefds);
                if (clients[i].fd > max_fd)
                    max_fd = clients[i].fd;
            }
        }

        // Waiting connections
        for (size_t i = 0; i < waitq.count; i++) {
            FD_SET(waitq.fds[i], &readfds);
            if (waitq.fds[i] > max_fd)
                max_fd = waitq.fds[i];
        }

        if (select(max_fd + 1, &readfds, &writefds, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        // Handle new connections
        if (FD_ISSET(welcome_fd, &readfds)) {
            
            int new_fd = accept(welcome_fd, NULL, NULL);
            if (new_fd < 0) {
                perror("accept");
                continue;
            }

            if (avail_count > 0) {
                int id = avail_ids[--avail_count];
                size_t slot = id - 1;

                clients[slot].fd = new_fd;
                clients[slot].id = id;
                clients[slot].rpos = 0;

                char welcome[64];
                snprintf(welcome, sizeof(welcome), "Welcome to the game, your id is %d\n", id);
                enqueue_message(&clients[slot], welcome, strlen(welcome));

                char notify[64];
                snprintf(notify, sizeof(notify), "Player %d joined the game\n", id);
                broadcast(notify, id);
            } else {
                if (waitq.count >= waitq.capacity) {
                    waitq.capacity = waitq.capacity ? waitq.capacity * 2 : INITIAL_QUEUE_SIZE;
                    waitq.fds = realloc(waitq.fds, waitq.capacity * sizeof(int));
                }
                waitq.fds[waitq.count++] = new_fd;
            }
        }

        // Process clients
        for (size_t i = 0; i < max_players; i++) {
            if (clients[i].fd == -1) continue;

            // Read handling
            if (FD_ISSET(clients[i].fd, &readfds)) {
                printf("Server is ready to read from player %d on socket %d\n",clients[i].id, clients[i].fd);
                ssize_t n = recv(clients[i].fd, 
                               clients[i].rbuf + clients[i].rpos,
                               MAX_LINE - clients[i].rpos, 0);
                if (n <= 0) {
                    handle_disconnect(i);
                    continue;
                }

                clients[i].rpos += n;
                clients[i].rbuf[clients[i].rpos] = '\0';

                char *ptr = clients[i].rbuf;
                char *line;
                while ((line = strsep(&ptr, "\n")) != NULL) {
                    if (strlen(line) == 0) continue;

                    int guess;
                    if (sscanf(line, "%d", &guess) != 1) continue;

                    char msg[64];
                    snprintf(msg, sizeof(msg), "Player %d guessed %d\n", clients[i].id, guess);
                    broadcast(msg, -1);

                    if (guess == target) {
                        char win[128];
                        snprintf(win, sizeof(win), 
                               "Player %d wins\nThe correct guessing is %d\n",
                               clients[i].id, guess);
                        
                        // Immediately send to all players
                        for (size_t j = 0; j < max_players; j++) {
                            if (clients[j].fd != -1) {
                                send(clients[j].fd, win, strlen(win), 0);
                            }
                        }

                        reset_game();
                        break;
                    } else {
                        const char *feedback = guess > target ? 
                            "The guess %d is too high\n" : "The guess %d is too low\n";
                        char response[64];
                        snprintf(response, sizeof(response), feedback, guess);
                        enqueue_message(&clients[i], response, strlen(response));
                    }
                }
                if (ptr) {
                    size_t remaining = clients[i].rpos - (ptr - clients[i].rbuf);
                    memmove(clients[i].rbuf, ptr, remaining);
                    clients[i].rpos = remaining;
                    clients[i].rbuf[clients[i].rpos] = '\0'; // Ensure null termination
                } else {
                    clients[i].rpos = 0;
                }
            }

            // Write handling
            if (FD_ISSET(clients[i].fd, &writefds) && clients[i].wq_size > 0) {
                printf("Server is ready to write to player %d on socket %d\n",clients[i].id, clients[i].fd);
                Message *msg = &clients[i].wqueue[0];
                ssize_t sent = send(clients[i].fd, 
                                  msg->data + msg->sent, 
                                  msg->len - msg->sent, 0);

                if (sent < 0) {
                    if (errno != EAGAIN) handle_disconnect(i);
                    continue;
                }

                msg->sent += sent;
                if (msg->sent == msg->len) {
                    free(msg->data);
                    memmove(clients[i].wqueue, 
                           clients[i].wqueue + 1, 
                           (clients[i].wq_size - 1) * sizeof(Message));
                    clients[i].wq_size--;
                }
            }
        }

        // Check waiting connections for disconnects
        for (size_t i = 0; i < waitq.count;) {
            if (FD_ISSET(waitq.fds[i], &readfds)) {
                char buf;
                if (recv(waitq.fds[i], &buf, 1, MSG_PEEK) <= 0) {
                    close(waitq.fds[i]);
                    memmove(waitq.fds + i, 
                           waitq.fds + i + 1, 
                           (waitq.count - i - 1) * sizeof(int));
                    waitq.count--;
                } else {
                    i++;
                }
            } else {
                i++;
            }
        }
    }

    cleanup();
    return EXIT_SUCCESS;
}