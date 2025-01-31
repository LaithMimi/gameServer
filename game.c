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
    char *data; //a pointer to the message data
    size_t len; // length of the message
    size_t sent; // amount of data sent so far
} Message;

typedef struct {
    int fd; // file descriptor for the player socket
    int id; //a unique identifier for the player
    char rbuf[MAX_LINE]; // read buffer for incoming data
    size_t rpos; //current position in the read buffer
    Message *wqueue; //a queue of messages to be sent
    size_t wq_size; //current size of the write queue
    size_t wq_cap; //the capacity of the write queue
} Client;

typedef struct {
    int *fds; //an array of file descriptors for waiting players
    size_t count; // current number of waiting players
    size_t capacity; // capacity of the waiting queue
} WaitQueue;

//Global state
static volatile sig_atomic_t running = 1;//a flag to indicate if the server is running
static int welcome_fd = -1;
static int target; // the secret number to guess
static unsigned max_players;
static Client *clients; //players
static int *avail_ids; //available player ids
static size_t avail_count; //number of available player ids
static WaitQueue waitq = {NULL, 0, 0}; //waiting players queue

void cleanup() {
    close(welcome_fd);
    for (size_t i = 0; i < max_players; i++) {//to iterate over all players
        if (clients[i].fd != -1) { // if the player is connected
            close(clients[i].fd); // close the player socket
            free(clients[i].wqueue); // and free the message queue
        }
    }
    for (size_t i = 0; i < waitq.count; i++)
        close(waitq.fds[i]);//close all waiting player connections

    free(clients);//free the players array
    free(avail_ids);
    free(waitq.fds);//free the waiting players array
}

void sigint_handler(int sig) {
    running = 0;
}

void enqueue_message(Client *c, const char *msg, size_t len) {
    if (c->wq_size >= c->wq_cap) {//if the write queue is full
        size_t new_cap = c->wq_cap ? c->wq_cap * 2 : INITIAL_QUEUE_SIZE; //to double the capacity
        Message *newq = realloc(c->wqueue, new_cap * sizeof(Message));//to reallocate the write queue
        if (!newq) {
            perror("realloc");
            return;
        }
        c->wqueue = newq;//to update the write queue
        c->wq_cap = new_cap;//to update the capacity
    }

    char *copy = strndup(msg, len);//to duplicate the message
    if (!copy) {
        perror("strndup");
        return;
    }

    c->wqueue[c->wq_size] = (Message){copy, len, 0};//adding the message to the write queue
    c->wq_size++;//incrementing the write queue size
}

void broadcast(const char *msg, int exclude_id) {
    for (size_t i = 0; i < max_players; i++) {//to iterate over all players
        if (clients[i].fd != -1 && clients[i].id != exclude_id) {//if the player is connected and not excluded 
            enqueue_message(&clients[i], msg, strlen(msg));//to add the message to the player's write queue
        }
    }
}

void reset_game() {
    for (size_t i = 0; i < max_players; i++) {//to iterate over all players
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
    //printf("DEBUG: Server started. Secret number is %d\n", target);

}

void promote_waiting() {
    while (avail_count > 0 && waitq.count > 0) {//while there are available player ids and waiting players
        int new_fd = waitq.fds[0];//to get the first waiting player
        memmove(waitq.fds, waitq.fds + 1, (waitq.count - 1) * sizeof(int));//to remove the first waiting player
        waitq.count--;

        int id = avail_ids[--avail_count];//to get the next available player id
        size_t slot = id - 1;//to calculate the player's slot

        clients[slot].fd = new_fd;//to assign the player's socket
        clients[slot].id = id;
        clients[slot].rpos = 0;
        clients[slot].wqueue = NULL;
        clients[slot].wq_size = clients[slot].wq_cap = 0;

        char welcome[64];//to create a welcome message
        snprintf(welcome, sizeof(welcome), "Welcome to the game, your id is %d\n", id);
        enqueue_message(&clients[slot], welcome, strlen(welcome));//to add the welcome message to the player's write queue

        char notify[64];
        snprintf(notify, sizeof(notify), "Player %d joined the game\n", id);
        broadcast(notify, id);
    }
}

void handle_disconnect(size_t idx) {
    char msg[64];//to create a disconnect message
    snprintf(msg, sizeof(msg), "Player %d disconnected\n", clients[idx].id);
    broadcast(msg, clients[idx].id);

    close(clients[idx].fd);//to close the player's socket
    clients[idx].fd = -1;
    avail_ids[avail_count++] = clients[idx].id;//to add the player's id to the available ids
    
    free(clients[idx].wqueue);//to free the player's write queue
    clients[idx].wqueue = NULL;
    clients[idx].wq_size = clients[idx].wq_cap = 0;

    promote_waiting();//to promote waiting players
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
    target = (rand() % SECRET_NUM_MAX) + 1;//to generate a random secret number

    //initializing client structures
    clients = calloc(max_players, sizeof(Client));//to allocate memory for the players
    avail_ids = malloc(max_players * sizeof(int));//to allocate memory for the available player ids
    for (size_t i = 0; i < max_players; i++) {
        clients[i].fd = -1;
        avail_ids[i] = i + 1;
    }
    avail_count = max_players;

    //setup server socket
    struct sockaddr_in addr = {
        .sin_family = AF_INET,//for IPv4
        .sin_port = htons(port),//converting the port number to network byte order
        .sin_addr.s_addr = INADDR_ANY//to bind to all interfaces
    };

    if ((welcome_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {//create a socket
        perror("socket");
        return EXIT_FAILURE;
    }

    int opt = 1;//to enable address reuse
    if (setsockopt(welcome_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {//setting the socket options
        perror("setsockopt");
        return EXIT_FAILURE;
    }

    if (bind(welcome_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {//binding the socket to the address
        perror("bind");
        return EXIT_FAILURE;
    }

    if (listen(welcome_fd, BACKLOG) < 0) {//to listen for incoming connections
        perror("listen");
        return EXIT_FAILURE;
    }

    printf("Server is ready to read from welcome socket %d\n", welcome_fd);
    signal(SIGINT, sigint_handler);//to handle the SIGINT signal
   // printf("DEBUG: Server started. Secret number is %d\n", target);

    fd_set readfds, writefds;//file descriptor sets for select
    while (running) {//main server loop
        FD_ZERO(&readfds);//to clear the read file descriptor set
        FD_ZERO(&writefds);//to clear the write file descriptor set
        int max_fd = welcome_fd;

        FD_SET(welcome_fd, &readfds);//adding the welcome socket to the read file descriptor set

        //active clients
        for (size_t i = 0; i < max_players; i++) {//to iterate over all players
            if (clients[i].fd != -1) {
                FD_SET(clients[i].fd, &readfds);//to add the player's socket to the read file descriptor set
                if (clients[i].wq_size > 0)
                    FD_SET(clients[i].fd, &writefds);//to add the player's socket to the write file descriptor set
                if (clients[i].fd > max_fd)
                    max_fd = clients[i].fd;//to update the maximum file descriptor
            }
        }

        //waiting connections
        for (size_t i = 0; i < waitq.count; i++) {//to iterate over all waiting players
            FD_SET(waitq.fds[i], &readfds);//to add the waiting player's socket to the read file descriptor set
            if (waitq.fds[i] > max_fd)
                max_fd = waitq.fds[i];
        }

        if (select(max_fd + 1, &readfds, &writefds, NULL, NULL) < 0) {//to wait for file descriptor activity
            if (errno == EINTR) //if interrupted by signal
                continue;
            perror("select");
            break;
        }

        // Handle new connections
        if (FD_ISSET(welcome_fd, &readfds)) {//if there is activity on the welcome socket
            
            int new_fd = accept(welcome_fd, NULL, NULL);//to accept the new connection
            if (new_fd < 0) {
                perror("accept");
                continue;
            }

            if (avail_count > 0) {//if there are available player ids
                int id = avail_ids[--avail_count];//to get the next available player id
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
            } 
            else {//if there are no available player ids
                if (waitq.count >= waitq.capacity) {
                    waitq.capacity = waitq.capacity ? waitq.capacity * 2 : INITIAL_QUEUE_SIZE;
                    waitq.fds = realloc(waitq.fds, waitq.capacity * sizeof(int));
                }
                waitq.fds[waitq.count++] = new_fd;
            }
        }

        // Process players
        for (size_t i = 0; i < max_players; i++) {//to iterate over all players
            if (clients[i].fd == -1) //if the player is not connected
                continue;

            // Read handling
            if (FD_ISSET(clients[i].fd, &readfds)) {//if there is activity on the player's socket
                
                printf("Server is ready to read from player %d on socket %d\n",clients[i].id, clients[i].fd);
                
                ssize_t n = recv(clients[i].fd, //to read data from the player's socket
                               clients[i].rbuf + clients[i].rpos,//to read data into the read buffer
                               MAX_LINE - clients[i].rpos, 0);//to read up to MAX_LINE bytes
                
                if (n <= 0) {//if the player is disconnected
                    handle_disconnect(i);
                    continue;
                }

                clients[i].rpos += n;//to update the read position
                clients[i].rbuf[clients[i].rpos] = '\0';//ensuring null termination

                char *ptr = clients[i].rbuf;//to parse the read buffer
                char *line;
                while ((line = strsep(&ptr, "\n")) != NULL) {//splitting the read buffer by newline
                    if (strlen(line) == 0) 
                        continue;

                    int guess;
                    if (sscanf(line, "%d", &guess) != 1) //if the input is not a number
                        continue;

                    char msg[64];
                    snprintf(msg, sizeof(msg), "Player %d guessed %d\n", clients[i].id, guess);
                    broadcast(msg, -1);

                    if (guess == target) {//if the guess is correct
                        char win[128];
                        snprintf(win, sizeof(win), "Player %d wins\nThe correct guessing is %d\n",clients[i].id, guess);
                        
                        //immediately send to all players
                        for (size_t j = 0; j < max_players; j++) {
                            if (clients[j].fd != -1) {
                                send(clients[j].fd, win, strlen(win), 0);
                            }
                        }

                        reset_game();
                        break;
                    } 
                    else {
                        const char *feedback = guess > target ? "The guess %d is too high\n" : "The guess %d is too low\n";
                        char response[64];
                        snprintf(response, sizeof(response), feedback, guess);
                        enqueue_message(&clients[i], response, strlen(response));
                    }
                }
                if (ptr) {//if there is remaining data

                    size_t remaining = clients[i].rpos - (ptr - clients[i].rbuf);//to calculate the remaining data
                    memmove(clients[i].rbuf, ptr, remaining);//to move the remaining data to the beginning of the read buffer
                    clients[i].rpos = remaining;//to update the read position
                    clients[i].rbuf[clients[i].rpos] = '\0'; //ensuring null termination
                }
                 else {//if there is no remaining data

                    clients[i].rpos = 0;
                }
            }

            // Write handling
            if (FD_ISSET(clients[i].fd, &writefds) && clients[i].wq_size > 0) {//if there is activity on the player's socket and there are messages to send
                printf("Server is ready to write to player %d on socket %d\n",clients[i].id, clients[i].fd);
                Message *msg = &clients[i].wqueue[0];
                ssize_t sent = send(clients[i].fd, 
                                  msg->data + msg->sent, 
                                  msg->len - msg->sent, 0);

                if (sent < 0) {//if there is an error
                    if (errno != EAGAIN) 
                        handle_disconnect(i);
                    continue;
                }

                msg->sent += sent;//to update the amount of data sent
                if (msg->sent == msg->len) {//if the message is fully sent
                    free(msg->data);
                    memmove(clients[i].wqueue, clients[i].wqueue + 1,(clients[i].wq_size - 1) * sizeof(Message));//remove the message from the write queue
                    clients[i].wq_size--;
                }
            }
        }

        // Check waiting connections for disconnects
        for (size_t i = 0; i < waitq.count;) {
            if (FD_ISSET(waitq.fds[i], &readfds)) {
                char buf;
                if (recv(waitq.fds[i], &buf, 1, MSG_PEEK) <= 0) {//to check if the connection is closed
                    close(waitq.fds[i]);
                    memmove(waitq.fds + i, waitq.fds + i + 1, (waitq.count - i - 1) * sizeof(int));//to remove the waiting player
                    waitq.count--;
                } else {//if the connection is still open then move to the next waiting player
                    i++;
                }
            } else {//if there is no activity on the waiting player's socket then move to the next waiting player
                i++;
            }
        }
    }

    cleanup();
    return EXIT_SUCCESS;
}