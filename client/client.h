#ifndef HEADER
#define HEADER

#include <pthread.h>
#include <stdbool.h>


#define MAX_LINE_LENGTH 4096+51
#define BUFFER_SIZE 4096
#define CHUNK_SIZE 4096
#define SERVER_PORT 6880
#define SERVER_IP "127.0.0.1"
#define PEER_PORT 6899
#define MY_PEER_PORT 6891
#define TIMEOUT_SEC 5  
#define NUM_THREADS 8

pthread_mutex_t open_file_mtx;

struct open_files
{
    char* file_name;
    int num_chunks;
    int *chunks;
    struct open_files* next;
    int client_recv;
    int num_peers;
};


struct open_files* front=NULL;
struct open_files* rear=NULL;


struct send_peer{
    int peer_sock;
    char *file_name;
    int start_chunk;
    int num_chunk;
};

struct open_files * file_is_receiving(char* filename);
int delete_file_from_list(char *filename);

struct open_files * file_is_receiving(char* filename){
    pthread_mutex_lock(&open_file_mtx);

    struct open_files* temp=front;
    while(temp!=NULL){
        if(strcmp(temp->file_name,filename)==0){
            return temp;
        }
        temp = temp->next;
    }
    pthread_mutex_unlock(&open_file_mtx);
    return NULL;
}

void addfile(struct open_files* nfile){

    pthread_mutex_lock(&open_file_mtx);

    if(front==NULL && rear==NULL){
        front=rear=nfile;
    }
    else{
        rear->next=nfile;
        rear=nfile;
    }
    pthread_mutex_unlock(&open_file_mtx);

    printf("File added to the list successfully\n");

}

int delete_file_from_list(char *filename){

    pthread_mutex_lock(&open_file_mtx);
    if(front==NULL){
        printf("file not present already closed\n");
        return 1;
    }
    if(strcmp(front->file_name,filename)==0 && front->next==NULL){
        free(front);
        front=rear=NULL;
        return 1;
    }
    struct open_files * temp,*prev= front;

    while(temp!=NULL){

        if(strcmp(temp->file_name,filename)==0){
            if(temp==prev){
                front=temp->next;
                free(temp);
                temp=prev=NULL;
            }
            else if(temp->next==NULL){
                prev->next=NULL;
                free(temp);
                temp=NULL;
            }
            else{
                prev->next=temp->next;
                free(temp);
                temp=NULL;
            }
            printf("file removed from the list\n");
            return 1;
        }
        else{
            prev=temp;
            temp=temp->next;
        }
    }
    pthread_mutex_unlock(&open_file_mtx);
    printf("File not in the list\n");
    return 0;
}

int check_chunk(struct open_files* cfile, int i){
    return cfile->chunks[i];
}

bool check_chunk_complete(struct open_files* cfile){

    for(int i=0;i<cfile->num_chunks;i++){
        if(cfile->chunks[i]==0)
            return false;
    }
    return true;
}

#endif