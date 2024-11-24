#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include "client.h"
#include "client.c"



int server_sock;
struct queue{
	int fd;
	struct queue* next;
};

int num_sockets = 0;
pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
struct queue* queue = NULL;
pthread_cond_t cond_queue = PTHREAD_COND_INITIALIZER;

void sigint_handler(int sig){
	printf("Closing socket\n");
	close(server_sock);
	exit(0);
}



char* get_local_ip() {
    char host[256];
    struct hostent *host_entry;
    static char ip[16];
    
    gethostname(host, sizeof(host));
    host_entry = gethostbyname(host);
    
    if (host_entry == NULL) {
        strcpy(ip, "127.0.0.1");
    } else {
        strcpy(ip, inet_ntoa(*((struct in_addr*)host_entry->h_addr_list[0])));
    }
    
    return ip;
}

int get_file_size(FILE *file) {
    if (file == NULL) {
        return -1;  
    }

    
    if (fseek(file, 0, SEEK_END) != 0) {
        return -1;  
    }

    
    long file_size = ftell(file);
    if (file_size == -1) {
        return -1;  
    }

    
    rewind(file);

    return file_size;
}

int send_file(const char *file, off_t start, off_t end, int client_sock, int chunk_num) {
    
    int file_fd = open(file, O_RDONLY);
    if (file_fd < 0) {
        perror("Error opening file");
        return -1;
    }

    
    if (lseek(file_fd, start, SEEK_SET) == (off_t) -1) {
        perror("Error seeking to start position");
        close(file_fd);
        return -1;
    }

    
    size_t bytes_to_send = end - start;
    char buffer[BUFFER_SIZE];
    ssize_t bytes_sent, total_sent = 0;

    
    char header[51];
    int header_len = snprintf(header, sizeof(header), "%d", chunk_num);
    if (header_len < 0 || header_len >= sizeof(header)) {
        fprintf(stderr, "Error creating header\n");
        close(file_fd);
        return -1;
    }
    header[50] = '\0';
    
    while (bytes_to_send > 0) {
        size_t bytes_in_this_packet = (bytes_to_send < BUFFER_SIZE) ? bytes_to_send : BUFFER_SIZE;

        ssize_t bytes_read = read(file_fd, buffer, bytes_in_this_packet);
        if (bytes_read < 0) {
            perror("Error reading from file");
            close(file_fd);
            return -1;
        } else if (bytes_read == 0) {
            break; 
        }

        
        size_t packet_size = header_len + bytes_read;
        char *packet = malloc(packet_size);
        if (!packet) {
            perror("Error allocating memory");
            close(file_fd);
            return -1;
        }

        
        memcpy(packet, header, header_len);
        memcpy(packet + header_len, buffer, bytes_read);

        
        size_t to_send = packet_size;
        printf("To send packet of size: %ld, chunk: %d\n",to_send, chunk_num);
        char *ptr = packet;
        while (to_send > 0) {
            bytes_sent = send(client_sock, ptr, to_send, 0);
            if (bytes_sent < 0) {
                perror("Error sending packet");
                free(packet);
                close(file_fd);
                return -1;
            }
            to_send -= bytes_sent;
            ptr += bytes_sent;
            total_sent += bytes_sent;
        }

        free(packet);
        bytes_to_send -= bytes_read;
    }

    close(file_fd);
    return 0;
}


void* handle_connection(void* arg) {
    while(1){
		pthread_mutex_lock(&queue_lock);
		while(num_sockets <= 0){
			pthread_cond_wait(&cond_queue, &queue_lock);
		}
		if(queue == NULL){
			printf("NULL\n");
		}
		int client_sock = queue->fd;
		struct queue* temp = queue;
		queue = queue->next;
		free(temp);
		num_sockets -= 1;
		pthread_mutex_unlock(&queue_lock);

        char buffer[MAX_LINE_LENGTH];

        int received = recv(client_sock, buffer, sizeof(buffer)-1, 0);
        if (received > 0) {
            buffer[received] = '\0';

            if (strcmp(buffer, "server") == 0) {
                
                send(client_sock, "Alive", 5, 0);
                printf("Responded to server health check\n");
            } else if (strcmp(buffer, "client") == 0) {
                send(client_sock,"File Name?",10,0);
                int len = recv(client_sock, buffer, sizeof(buffer)-1,0);
                buffer[len] = '\0';
                
                struct open_files* file;
                 
                if((file = file_is_receiving(buffer)) != NULL){
                    
                    file->num_peers += 1;
                    send(client_sock,"YES",3,0);
                    len = htonl(file->num_chunks);
                    recv(client_sock,buffer,sizeof(buffer),0);
                    
                    send(client_sock,(void *)&len, sizeof(int),0);
                    int limit[2];
                    recv(client_sock, limit, sizeof(limit), 0);

                    
                    int start_chunk = ntohl(limit[0]);
                    int num_chunks = ntohl(limit[1]);
                    int start, end;
                    for(int i=0;i<num_chunks;i++){
                        int chunk_num = start_chunk + i;
                        start = (chunk_num)*CHUNK_SIZE;
                        end = start + CHUNK_SIZE;
                        if(end > len){
                            end = len;
                        }
                        printf("requested from file %s bytes %d to %d", file->file_name,start,end);
                        if(start<0 || end < 0 || start > len || end > len || start > end){
                            close(client_sock);
                            continue;
                        }
                        if(check_chunk(file,chunk_num)){
                            int sent = send_file(file->file_name,start,end,client_sock, chunk_num);
                        }
                    }
                    send(client_sock,"Done",4,0);
                    file->num_peers -= 1;
                    if(file->client_recv == 0 && file->num_peers==0){
                        delete_file_from_list(file->file_name);
                    }
                }
                else{
                FILE* file = fopen(buffer, "r");
                if(!file){
                    send(client_sock,"NO",2,0);
                    close(client_sock);
                    fclose(file);
                    continue;
                }
                else{
                    
                    
                    
                    

                    const char *file_name = strdup(buffer);

                    send(client_sock,"YES",3,0);
                    len = get_file_size(file);
                    int num_file_chunks = len / CHUNK_SIZE;
                    num_file_chunks += 1;
                    int size_to_send = htonl(num_file_chunks);
                    send(client_sock, (void *)&size_to_send, sizeof(int), 0);
                    int limit[2];
                    recv(client_sock, limit, sizeof(limit), 0);

                    
                    int start_chunk = ntohl(limit[0]);
                    int num_chunks = ntohl(limit[1]);
                    int start, end;
                    for(int i=0;i<num_chunks;i++){
                        int chunk_num = start_chunk + i;
                        start = (chunk_num)*CHUNK_SIZE;
                        end = start + CHUNK_SIZE;
                        if(end > len){
                            end = len;
                        }
                        printf("requested from file %s bytes %d to %d", file_name,start,end);
                        if(start<0 || end < 0 || start > len || end > len || start > end){
                            close(client_sock);
                            continue;
                        }
                        int sent = send_file(file_name,start,end,client_sock, chunk_num);
                    }
                    send(client_sock,"Done",4,0);
                    free((void *)file_name);
                }
                }
            }
            
        }

        close(client_sock);
    }
    return NULL;
}

int main(int argc, char* argv[]) {
    int  tracker_sock;
    
	signal(SIGINT,sigint_handler);
    struct sockaddr_in server_addr, tracker_addr, client_addr;
    char buffer[MAX_LINE_LENGTH];
    int peer_port = PEER_PORT;
    if(argc == 1){
        printf("Usage ./peer <port_num>\n");
        return 0;
    }

    peer_port = atoi(argv[1]);
    
    pthread_mutex_init(&open_file_mtx,NULL);

    if(argc > 2){
        for(int i=2; i<argc; i++){
        pthread_t client_thread;
        pthread_create(&client_thread, NULL, client_main, (void *)argv[i]);
        pthread_detach(client_thread);
        }
    }
    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("Socket creation failed");
        exit(1);
    }
    
    int opt = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        exit(1);
    }
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(peer_port);
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf("Binding to %d\n",peer_port);
        perror("Bind failed");
        exit(1);
    }
    if (listen(server_sock, 5) < 0) {
        perror("Listen failed");
        exit(1);
    }
    
    
    tracker_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (tracker_sock < 0) {
        perror("Tracker socket creation failed");
        exit(1);
    }
    
    memset(&tracker_addr, 0, sizeof(tracker_addr));
    tracker_addr.sin_family = AF_INET;
    tracker_addr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &tracker_addr.sin_addr) <= 0) {
        perror("Invalid tracker address");
        exit(1);
    }
    
    if (connect(tracker_sock, (struct sockaddr*)&tracker_addr, sizeof(tracker_addr)) < 0) {
        perror("Tracker connection failed");
        exit(1);
    }
    
    send(tracker_sock, "peer", 5, 0);
    char* local_ip = get_local_ip();
    memset(buffer, 0, sizeof(buffer));
    recv(tracker_sock, buffer, sizeof(buffer), 0);
    printf("%s\n",buffer);
    if(strcmp(buffer,"IP?") == 0){
        snprintf(buffer, sizeof(buffer), "%s:%d", local_ip, peer_port);
        send(tracker_sock, buffer, strlen(buffer), 0);
        
        memset(buffer, 0, sizeof(buffer));
        recv(tracker_sock, buffer, sizeof(buffer), 0);
        printf("%s\n",buffer);
        if (strcmp(buffer, "OK") == 0) {
            printf("Successfully registered with tracker\n");
            printf("Listening on %s:%d\n", local_ip, peer_port);
        }
        else {
            printf("Registration failed1\n");
            exit(1);
        }
    }
     else {
        printf("Registration failed2\n");
        exit(1);
    }
    
    close(tracker_sock);
	pthread_t thread_pool[NUM_THREADS];
	for(int i=0;i<NUM_THREADS;i++){
		pthread_create(thread_pool+i, NULL, handle_connection, NULL);
	}
    
    socklen_t client_len = sizeof(client_addr);
    while (1) {
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
        if(client_sock < 0){
            continue;
        }
		struct queue* temp = (struct queue*)malloc(sizeof(struct queue));
		temp->fd = client_sock;
		temp->next = NULL;
		pthread_mutex_lock(&queue_lock);
		if(queue == NULL){
			queue = temp;
			num_sockets = 1;
		}
		else{
			temp->next = queue;
			queue = temp;
			num_sockets += 1;
		}
		pthread_mutex_unlock(&queue_lock);
		pthread_cond_signal(&cond_queue);

        printf("New connection from %s:%d\n", 
               inet_ntoa(client_addr.sin_addr), 
               ntohs(client_addr.sin_port));
                
    }
    
    close(server_sock);
     pthread_mutex_destroy(&open_file_mtx);
    return 0;
}
