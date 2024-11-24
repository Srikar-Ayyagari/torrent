#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <signal.h>
#include <sys/time.h>

#define MAX_LINE_LENGTH 256
#define MAX_CLIENTS 10
#define PEERS_FILE "peers.txt"
#define PORT 6880
#define PING_INTERVAL 10
#define MAX_FAILURES 3
#define TIMEOUT_SECONDS 2

int server_sock;
void sigint_handler(int sig){
	printf("Closing socket\n");
	close(server_sock);
	exit(0);
}

pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    char address[MAX_LINE_LENGTH];
    int failures;
} PeerHealth;

void trim(char *str) {
    char *end = str + strlen(str) - 1;
    while (end > str && (*end == '\n' || *end == '\r' || *end == ' ')) *end-- = '\0';
}


int count_lines(const char* filename) {
    FILE* file = fopen(filename, "r");
    if (!file) return 0;
    
    int count = 0;
    char buffer[MAX_LINE_LENGTH];
    while (fgets(buffer, sizeof(buffer), file)) count++;
    fclose(file);
    return count;
}

void remove_peer(const char* peer_to_remove) {
    FILE *file = fopen(PEERS_FILE, "r");
    FILE *temp = fopen("temp.txt", "w");
    char line[MAX_LINE_LENGTH];
    
    while (fgets(line, sizeof(line), file)) {
        trim(line);
        if (strcmp(line, peer_to_remove) != 0) {
            fprintf(temp, "%s\n", line);
        }
    }
    
    fclose(file);
    fclose(temp);
    remove(PEERS_FILE);
    rename("temp.txt", PEERS_FILE);
}

int check_peer_health(const char* peer_addr) {
    int sock;
    struct sockaddr_in peer;
    char ip[16];
    int port;
    
    sscanf(peer_addr, "%[^:]:%d", ip, &port);
    
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 0;
    
    struct timeval timeout;
    timeout.tv_sec = TIMEOUT_SECONDS;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = inet_addr(ip);
    peer.sin_port = htons(port);
    
    if (connect(sock, (struct sockaddr*)&peer, sizeof(peer)) < 0) {
        close(sock);
        return 0;
    }
    
    send(sock, "server", 6, 0);
    
    char response[32] = {0};
    int received = recv(sock, response, sizeof(response)-1, 0);
    close(sock);
    
    return (received > 0 && strcmp(response, "Alive") == 0);
}

void* health_checker(void* arg) {
    PeerHealth* peers = NULL;
    int peer_count = 0;
    
    while (1) {
        sleep(PING_INTERVAL);
        
        pthread_mutex_lock(&file_mutex);
        FILE* file = fopen(PEERS_FILE, "r");
        if (!file) {
            pthread_mutex_unlock(&file_mutex);
            continue;
        }
        
        char line[MAX_LINE_LENGTH];
        peer_count = 0;
        while (fgets(line, sizeof(line), file)) peer_count++;
        rewind(file);
        
        peers = realloc(peers, peer_count * sizeof(PeerHealth));
        
        int i = 0;
        while (fgets(line, sizeof(line), file)) {
            trim(line);
            strcpy(peers[i].address, line);
            i++;
        }
        fclose(file);
        pthread_mutex_unlock(&file_mutex);
        
        for (i = 0; i < peer_count; i++) {
            if (!check_peer_health(peers[i].address)) {
                peers[i].failures++;
                printf("Peer %s failed health check (%d/3)\n", peers[i].address, peers[i].failures);
                
                if (peers[i].failures >= MAX_FAILURES) {
                    printf("Removing unresponsive peer: %s\n", peers[i].address);
                    pthread_mutex_lock(&file_mutex);
                    remove_peer(peers[i].address);
                    pthread_mutex_unlock(&file_mutex);
                    peers[i].failures = 0;
                }
            } else {
                peers[i].failures = 0;
            }
        }
    }
    
    free(peers);
    return NULL;
}

void handle_client_request(int client_sock) {
    pthread_mutex_lock(&file_mutex);
    
    int num_peers = count_lines(PEERS_FILE);
    char num_str[16];
    sprintf(num_str, "%d", num_peers);
    
    send(client_sock, num_str, strlen(num_str), 0);
    
    char ack[2];
    recv(client_sock, ack, 2, 0);
    
    if (num_peers > 0) {
        FILE* file = fopen(PEERS_FILE, "r");
        if (file) {
            char line[MAX_LINE_LENGTH];
            while (fgets(line, sizeof(line), file)) {
                line[strcspn(line, "\n")] = 0;
                send(client_sock, line, strlen(line), 0);
                recv(client_sock, ack, 2, 0);
            }
            fclose(file);
        }
    }
    
    pthread_mutex_unlock(&file_mutex);
}

void handle_peer_registration(int client_sock) {
    char peer_info[MAX_LINE_LENGTH];
    send(client_sock, "IP?", 3, 0);
    ssize_t bytes_received = recv(client_sock, peer_info, sizeof(peer_info) - 1, 0);
    printf("Got peer\n");

    if (bytes_received > 0) {
        peer_info[bytes_received] = '\0';

        pthread_mutex_lock(&file_mutex);
        FILE* file = fopen(PEERS_FILE, "r+");  
        if (file) {
            char line[MAX_LINE_LENGTH];
            int exists = 0;

            
            while (fgets(line, sizeof(line), file)) {
                
                line[strcspn(line, "\n")] = '\0';
                if (strcmp(line, peer_info) == 0) {
                    exists = 1;
                    break;
                }
            }

            
            if (!exists) {
                fprintf(file, "%s\n", peer_info);
                send(client_sock, "OK", 2, 0);
                printf("Sent OK\n");
            } else {
                send(client_sock, "ALREADY_EXISTS", 14, 0);
                printf("Peer already registered\n");
            }
            fclose(file);
        }
        pthread_mutex_unlock(&file_mutex);
    }
}

void* handle_client(void* arg) {
    int client_sock = *((int*)arg);
    free(arg);
    printf("Insode handle\n");
    char msg_type[32];
    ssize_t bytes_received = recv(client_sock, msg_type, sizeof(msg_type) - 1, 0);
    printf("%s\n", msg_type);
    if (bytes_received > 0) {
        msg_type[bytes_received] = '\0';
        
        if (strcmp(msg_type, "client") == 0) {
            handle_client_request(client_sock);
        }
        else if (strcmp(msg_type, "peer") == 0) {
            handle_peer_registration(client_sock);
        }
    }
    
    close(client_sock);
    return NULL;
}

int main() {
    int *client_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    pthread_t thread_id;
	signal(SIGINT,sigint_handler);
    
    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("Socket creation failed");
        exit(1);
    }
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);
    
    int opt=1;
	
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        	printf("setsockopt(SO_REUSEADDR) failed\n");
	        return 0;
    }

    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        exit(1);
    }
    
    if (listen(server_sock, MAX_CLIENTS) < 0) {
        perror("Listen failed");
        exit(1);
    }
    
    printf("Tracker server listening on port %d\n", PORT);
    
    FILE* file = fopen(PEERS_FILE, "w");
    if (file) fclose(file);
    
    pthread_t thread_1;
    if (pthread_create(&thread_1, NULL, health_checker, NULL) != 0) {
        exit(1);
    }
    
    while (1) {
        client_sock = malloc(sizeof(int));
        *client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
        
        if (*client_sock < 0) {
            printf("Accept failed");
            free(client_sock);
            continue;
        }
        
        printf("New connection from %s:%d\n", 
               inet_ntoa(client_addr.sin_addr), 
               ntohs(client_addr.sin_port));
        printf("Socket %d\n",*client_sock);
        if (pthread_create(&thread_id, NULL, handle_client, (void*)client_sock) < 0) {
            printf("Could not create thread");
            free(client_sock);
            continue;
        }
        
        pthread_detach(thread_id);
    }
    
    close(server_sock);
    return 0;
}