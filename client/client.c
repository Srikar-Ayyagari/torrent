#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include <errno.h>
#include "client.h"
#include <asm-generic/socket.h>
#include <stdbool.h>


int num_peers = 0,t_peers=0;
char **peers,**tpeers;
int *peer_sockets; 


void handle_signal(int sig) {
    printf("\nReceived signal %d, cleaning up...\n", sig);
    
    
    for (int i = 0; i < num_peers; i++) {
        if (peer_sockets[i] != -1) {
            close(peer_sockets[i]);
            printf("Closed connection to peer socket %d\n", peer_sockets[i]);
        }
    }
    
    
    if (peers) {
        for (int i = 0; i < num_peers; i++) {
            free(peers[i]);
        }
        free(peers);
    }
        if (tpeers) {
        for (int i = 0; i < t_peers; i++) {
            free(tpeers[i]);
        }
        free(peers);
    }
    if (peer_sockets) free(peer_sockets);

    exit(0);  
}


int connect_to_server(const char *ip, int port) {
    struct sockaddr_in server_addr;
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        exit(1);
    }
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid address");
        exit(1);
    }
    
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        exit(1);
    }

    
    struct timeval timeout;
    timeout.tv_sec = TIMEOUT_SEC;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    
    return sock;
}


void receive_peers_from_tracker(int tracker_sock) {
    char buffer[MAX_LINE_LENGTH];
    ssize_t received;

    
    received = recv(tracker_sock, buffer, sizeof(buffer)-1, 0);
    if (received <= 0) {
        perror("Error receiving peer count");
        exit(1);
    }
    buffer[received] = '\0';
    num_peers = atoi(buffer);
    printf("%d",num_peers);
    
    if(num_peers > 0){
        send(tracker_sock,"OK",2,0);
    }
    else{
        printf("No peers");
        return;
    }
    peers = malloc(num_peers * sizeof(char *));
    tpeers = malloc(num_peers * sizeof(char *));
    peer_sockets = malloc(num_peers * sizeof(int));
    if (!peers || !peer_sockets || !tpeers) {
        perror("Memory allocation failed");
        exit(1);
    }

    for (int i = 0; i < num_peers; i++) peer_sockets[i] = -1;  

    
    for (int i = 0; i < num_peers; i++) {
        received = recv(tracker_sock, buffer, sizeof(buffer)-1, 0);
        if (received <= 0) {
            perror("Error receiving peer address");
            exit(1);
        }
        send(tracker_sock,"OK",2,0);
        buffer[received] = '\0';
        peers[i] = strdup(buffer);
        printf("%s\n",peers[i]);
    }
}

void *file_transfer_from_peer(void *peer_sock_ptr) {
    int peer_sock = ((struct send_peer *)peer_sock_ptr)->peer_sock;
        int opt =1;
    if ((setsockopt(peer_sock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT , &opt, sizeof(opt)))==-1){
        printf("setsockopt Failed\n");
        return 0;       
    }

    char *file_name = ((struct send_peer *)peer_sock_ptr)->file_name;

    char buffer[MAX_LINE_LENGTH];
    ssize_t received;

    printf("Sending 'client' message to peer\n");
    send(peer_sock, "client", 6, 0);

    
    received = recv(peer_sock, buffer, sizeof(buffer)-1, 0);
    if (received <= 0) {
        perror("Error receiving 'File name?' from peer");
        close(peer_sock);
        free(peer_sock_ptr);  
        return NULL;
    }
    buffer[received] = '\0';
    printf("Received from peer: %s\n", buffer);
    
    if (strcmp(buffer, "File Name?") == 0) {
        
        printf("Sending file name '%s' to peer %d\n", file_name,peer_sock);
        send(peer_sock, file_name, strlen(file_name), 0);

        
        received = recv(peer_sock, buffer, sizeof(buffer)-1, 0);
        if (received <= 0) {
            perror("Error receiving file availability response from peer");
            close(peer_sock);
            free(peer_sock_ptr);  
            return NULL;
        }
        buffer[received] = '\0';
        printf("Peer responded with: %s\n", buffer);

        if (strcmp(buffer, "NO") == 0) {
            printf("Peer does not have the file.\n");
            close(peer_sock);
            free(peer_sock_ptr);  
            return NULL;
        } else if (strcmp(buffer, "YES") == 0) {
            
            int num_file_chunk;
            send(peer_sock,"got_chunks?",12,0);
            received = recv(peer_sock, &num_file_chunk, sizeof(num_file_chunk), 0);
            if (received <= 0) {
                perror("Error receiving file length");
                close(peer_sock);
                free(peer_sock_ptr);  
                return NULL;
            }
            num_file_chunk = ntohl(num_file_chunk);
            printf("File size: %d chunks\n", num_file_chunk);

            
            FILE *output_file = fopen(file_name, "wb");
            if (!output_file) {
                perror("Error opening file for writing");
                close(peer_sock);
                free(peer_sock_ptr);
                return NULL;
            }
            int num_chunks_received = 0;
            int start_chunk = ((struct send_peer *)peer_sock_ptr)->start_chunk;
            int num_chunk = ((struct send_peer *)peer_sock_ptr)->num_chunk;
            
            
            
            
            
            int range[2] = {start_chunk, num_chunk};
            range[0] = htonl(range[0]);
            range[1] = htonl(range[1]); 
            printf("Requesting chunks from %d to %d\n", start_chunk, start_chunk + num_chunk);
            send(peer_sock, range, sizeof(range), 0);
            while (num_chunks_received < num_chunk) {
                int bytes_to_request = CHUNK_SIZE;

                received = recv(peer_sock, buffer, sizeof(buffer) - 1, 0);
                if (received <= 0) {
                    perror("Error receiving file chunk");
                    fclose(output_file);  
                    close(peer_sock);
                    free(peer_sock_ptr);  
                    return NULL;
                }
                printf("Received :%ld bytes\n",received);
                
                buffer[received] = '\0';

                
                size_t header_length = 50; 
                if (received < header_length) {
                    fprintf(stderr, "Packet too short to contain a header\n");
                    fclose(output_file);  
                    close(peer_sock);
                    free(peer_sock_ptr);  
                    return NULL;
                }
                int header_null = strcspn(buffer, "\0");
                
                char header[header_length + 1];
                memcpy(header, buffer, header_length);
                header[header_length] = '\0'; 
                
                printf("header details %s\n",header);
                int chunk_num;
                sscanf(header,"%d",&chunk_num);

                printf("Received chunk %d with %zu bytes of file data\n", chunk_num, received - header_length);

                size_t data_length = received - header_length;
                if (data_length > 0) {
                    fseek(output_file, chunk_num * CHUNK_SIZE, SEEK_SET);
                    fwrite(buffer + header_length, 1, data_length, output_file);
                }
                printf("Received chunk: %d\n", start_chunk);
                start_chunk += 1;
                num_chunks_received += 1;
            }

            
            received = recv(peer_sock, buffer, sizeof(buffer)-1, 0);
            if (received > 0) {
                buffer[received] = '\0';
                if (strcmp(buffer, "Done") == 0) {
                    printf("File transfer completed.\n");
                } else {
                    printf("Unexpected message during file transfer: %s\n", buffer);
                }
            }

            
            fclose(output_file);
            close(peer_sock);

        }
    }

    free(peer_sock_ptr);  
    return NULL;
}


void connect_to_tracker_and_get_peers() {
    int tracker_sock = connect_to_server(SERVER_IP, SERVER_PORT);
    printf("Connected to tracker at %s:%d\n", SERVER_IP, SERVER_PORT);

    send(tracker_sock, "client", 6, 0);
    receive_peers_from_tracker(tracker_sock);
    printf("Got from tracker");
    close(tracker_sock);
}


void connect_and_transfer_file(char * file_name) {
    pthread_t threads[num_peers];
    struct open_files * nfile = (struct open_files *)malloc(sizeof(struct open_files));
    int num_file_chunk;
    int i=0;
    printf("Got %d peers\n",num_peers);
    for (i=0; i < num_peers; i++) {
        char * temp =(char *)malloc(sizeof(peers[i]));
        strcpy(temp,peers[i]);
        int received;
        char buffer[MAX_LINE_LENGTH]={'\0'};
        char *peer_ip = strtok(temp, ":");
        int peer_port = atoi(strtok(NULL, ":"));
        int peer_sock = connect_to_server(peer_ip, peer_port);
        send(peer_sock,"client_check",13,0);
        received = recv(peer_sock, buffer, sizeof(buffer)-1, 0);
    if (received <= 0) {
        perror("Error receiving 'File name?' from peer");
        close(peer_sock);
    }
    printf("Received from peer: %s\n", buffer);
    if (strcmp(buffer, "File Name?") == 0) {
        printf("Sending file name '%s' to peer\n", file_name);
        send(peer_sock, file_name, strlen(file_name), 0);

        
        memset(buffer,0,MAX_LINE_LENGTH);
        received = recv(peer_sock, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            perror("Error receiving file availability response from peer");
            close(peer_sock);
        }
        printf("Peer responded with: %s\n", buffer);
        if (strcmp(buffer, "NO") == 0) {
            printf("Peer does not have the file.\n");
            close(peer_sock);
            continue;
        }
        else if(strcmp(buffer, "YES") == 0){
            tpeers[t_peers]=strdup(peers[i]);
            t_peers ++;
            send(peer_sock,"num_chunks?",12,0);
            printf("sent num chunk\n");
            
            received = recv(peer_sock,(void *) &num_file_chunk, sizeof(num_file_chunk), 0);
            if (received <= 0) {
                perror("Error receiving file length");
                close(peer_sock);
                continue;
            }
            num_file_chunk = ntohl(num_file_chunk);
            printf("Received file chunk size success %d\n",++num_file_chunk);
                nfile->file_name = (char *)malloc(100 * sizeof(char));
                if (nfile->file_name == NULL) {
                    perror("Failed to allocate memory for file_name");
                    free(nfile);  
                    return ;
                }
                strncpy(nfile->file_name,file_name,100);
                nfile->num_chunks=num_file_chunk;
                nfile->chunks = (int *)malloc(num_file_chunk*sizeof(int));
                if (nfile->chunks== NULL) {
                    perror("Failed to allocate memory for file_name");
                    free(nfile);  
                    return ;
                }
                memset(nfile->chunks,0,sizeof(nfile->chunks));
                nfile->next=NULL;
                nfile->client_recv=1;
                nfile->num_peers=0;
                addfile(nfile);
        }
    }
        free(temp);
    }

    if(t_peers==0){
        printf("File is not with any of the given peers by the tracker\n");
        return;
    }

    int num_chunk = 0;
    if(num_file_chunk < t_peers){
        num_chunk = 1;
    }
    else{
        num_chunk = num_file_chunk/t_peers;
    }
    int chunk_req = 0;
    for (int i = 0; i < t_peers , chunk_req < num_file_chunk; i++) {
        char * temp =(char *)malloc(sizeof(tpeers[i]));
        strcpy(temp,tpeers[i]);
        char *peer_ip = strtok(temp, ":");
        int peer_port = atoi(strtok(NULL, ":"));
        int peer_sock = connect_to_server(peer_ip, peer_port);
        peer_sockets[i] = peer_sock;  

        printf("Connected to peer %s:%d\n", peer_ip, peer_port);

        struct send_peer *peer_sock_ptr = malloc(sizeof(struct send_peer));
        if (!peer_sock_ptr) {
            perror("Memory allocation failed");
            exit(1);
        }
        peer_sock_ptr->peer_sock=peer_sock;
        peer_sock_ptr->file_name=file_name;
        peer_sock_ptr->start_chunk = chunk_req;

        if((i < t_peers-1)){
            peer_sock_ptr->num_chunk = num_chunk;
            chunk_req += num_chunk;
            nfile->num_peers;
            pthread_create(&threads[i], NULL, file_transfer_from_peer, (void *)peer_sock_ptr);
        }
        else{
            peer_sock_ptr->num_chunk = num_file_chunk - chunk_req;
            chunk_req = num_file_chunk;
            nfile->num_peers;
            pthread_create(&threads[i], NULL, file_transfer_from_peer, (void *)peer_sock_ptr);
            free(temp);
            break;
        }
        free(temp);
    }

    for (int i = 0; i < t_peers; i++) {
        pthread_join(threads[i], NULL);
    }
    
    nfile->client_recv = 0;
    if(nfile->client_recv==0 && nfile->num_peers == 0){
        delete_file_from_list(nfile->file_name);
    }

}


void* client_main(void* arg) {
    char* file_name = (char *)arg;

    
    signal(SIGINT, handle_signal);

    connect_to_tracker_and_get_peers();
    connect_and_transfer_file(file_name);
    return 0;
}

    
    
    
    
    
    
    