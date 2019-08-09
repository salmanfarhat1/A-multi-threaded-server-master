#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <assert.h>
#include <pthread.h>

#include "babble_server.h"
#include "babble_types.h"
#include "babble_utils.h"
#include "babble_communication.h"
#include "babble_server_answer.h"

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex2 = PTHREAD_MUTEX_INITIALIZER;

pthread_cond_t cond_empty = PTHREAD_COND_INITIALIZER; // for command producer
pthread_cond_t cond_full = PTHREAD_COND_INITIALIZER; // for command consumer

pthread_cond_t cond_producer_answers = PTHREAD_COND_INITIALIZER;
pthread_cond_t cond_consumer_answers = PTHREAD_COND_INITIALIZER;

command_t *command_buffer[BABBLE_PRODCONS_SIZE];
command_t *answer_buffer[BABBLE_PRODCONS_SIZE];

int buf_count , buf_answer_count;

static void display_help(char *exec)
{
    printf("Usage: %s -p port_number\n", exec);
}


static int parse_command(char* str, command_t *cmd)
{
    char *name = NULL;

    /* start by cleaning the input */
    str_clean(str);

    /* get command id */
    cmd->cid=str_to_command(str, &cmd->answer_expected);

    switch(cmd->cid){
    case LOGIN:
        if(str_to_payload(str, cmd->msg, BABBLE_ID_SIZE)){
            name = get_name_from_key(cmd->key);
            fprintf(stderr,"Error from [%s]-- invalid LOGIN -> %s\n", name, str);
            free(name);
            return -1;
        }
        break;
    case PUBLISH:
        if(str_to_payload(str, cmd->msg, BABBLE_SIZE)){
            name = get_name_from_key(cmd->key);
            fprintf(stderr,"Warning from [%s]-- invalid PUBLISH -> %s\n", name, str);
            free(name);
            return -1;
        }
        break;
    case FOLLOW:
        if(str_to_payload(str, cmd->msg, BABBLE_ID_SIZE)){
            name = get_name_from_key(cmd->key);
            fprintf(stderr,"Warning from [%s]-- invalid FOLLOW -> %s\n", name, str);
            free(name);
            return -1;
        }
        break;
    case TIMELINE:
        cmd->msg[0]='\0';
        break;
    case FOLLOW_COUNT:
        cmd->msg[0]='\0';
        break;
    case RDV:
        cmd->msg[0]='\0';
        break;
    default:
        name = get_name_from_key(cmd->key);
        fprintf(stderr,"Error from [%s]-- invalid client command -> %s\n", name, str);
        free(name);
        return -1;
    }

    return 0;
}

/* processes the command and eventually generates an answer */
static int process_command(command_t *cmd, answer_t **answer)
{
    int res=0;

    switch(cmd->cid){
    case LOGIN:
        res = run_login_command(cmd, answer);
        break;
    case PUBLISH:
        res = run_publish_command(cmd, answer);
        break;
    case FOLLOW:
        res = run_follow_command(cmd, answer);
        break;
    case TIMELINE:
        res = run_timeline_command(cmd, answer);
        break;
    case FOLLOW_COUNT:
        res = run_fcount_command(cmd, answer);
        break;
    case RDV:
        res = run_rdv_command(cmd, answer);
        break;
    default:
        fprintf(stderr,"Error -- Unknown command id\n");
        return -1;
    }

    if(res){
        fprintf(stderr,"Error -- Failed to run command ");
        display_command(cmd, stderr);
    }
    return res;
}

void * comm_routine(int vargp){

  int newsockfd = vargp;
  char* recv_buff=NULL;
  int recv_size=0;

  unsigned long client_key=0;
  char client_name[BABBLE_ID_SIZE+1];

  command_t *cmd;
  answer_t *answer=NULL;

  /*Recv login mesage*/
  memset(client_name, 0, BABBLE_ID_SIZE+1);
  if((recv_size = network_recv(newsockfd, (void**)&recv_buff)) < 0){
      fprintf(stderr, "Error -- recv from client\n");
      close(newsockfd);

  }

  /*Parse the message to get the command*/
  cmd = new_command(0);
  if(parse_command(recv_buff, cmd) == -1 || cmd->cid != LOGIN){
      fprintf(stderr, "Error -- in LOGIN message\n");
      close(newsockfd);
      free(cmd);

  }
  /* before processing the command, we should register the
   * socket associated with the new client; this is to be done only
   * for the LOGIN command*/

  /*Process the login cmd*/
  cmd->sock = newsockfd;
  if(process_command(cmd, &answer) == -1){
      fprintf(stderr, "Error -- in LOGIN\n");
      close(newsockfd);
      free(cmd);

  }

  /* ack client of registration */
  if(send_answer_to_client(answer) == -1){
      fprintf(stderr, "Error -- in LOGIN ack\n");
      close(newsockfd);
      free(cmd);
      free_answer(answer);
  }
  else{
      free_answer(answer);
  }

  /* let's store the key locally */
  client_key = cmd->key;
  strncpy(client_name, cmd->msg, BABBLE_ID_SIZE);
  free(recv_buff);
  free(cmd);

  /* looping on client commands */
  /* the network_recv will be blocked when there is no clients and return more than zero when is there */
  while((recv_size=network_recv(newsockfd, (void**) &recv_buff)) > 0){
  		cmd = new_command(client_key);
      pthread_mutex_lock(&mutex);

      /* put the cmd in the command_buffer*/
  		while(buf_count == BABBLE_PRODCONS_SIZE ){
  			pthread_cond_wait(&cond_empty, &mutex);
  		}

  		if(parse_command(recv_buff, cmd) == -1){
                  fprintf(stderr, "Warning: unable to parse message from client %s\n", client_name);
                  notify_parse_error(cmd, recv_buff, &answer);
                  send_answer_to_client(answer);
                  free_answer(answer);
                  free(cmd);
      }

      command_buffer[buf_count] = cmd;
      buf_count = buf_count+1;

      pthread_cond_signal(&cond_full);
      pthread_mutex_unlock(&mutex);

  } /*END WHILE*/

  /* Unregister the client */
	if(client_name[0] != 0 ){
    cmd = new_command(client_key);
    cmd->cid= UNREGISTER;

    if(unregisted_client(cmd)){
        fprintf(stderr,"Warning -- failed to unregister client %s\n",client_name);
    }
    free(cmd);
  }
  return 0;
}

void * executor_routine(){
	command_t *cmd;
	answer_t *answer=NULL;

  	while(1){
  		pthread_mutex_lock(&mutex);

      /*Take a cmd from the command buffer*/
  		while(buf_count == 0 ){
  			pthread_cond_wait(&cond_full, &mutex);
  		}
      buf_count--;
      cmd = command_buffer[buf_count];
      /*Process the cmd and generate an answer*/
  		if(process_command(cmd, &answer) == -1){
        fprintf(stderr, "Warning: unable to process command from client %lu\n", cmd->key);
    	}
    	free(cmd);
      pthread_mutex_unlock(&mutex);
      pthread_cond_broadcast(&cond_empty);

      pthread_mutex_lock(&mutex2);
      while(buf_answer_count == BABBLE_PRODCONS_SIZE){
        pthread_cond_wait(&cond_producer_answers , &mutex2);
      }
      answer_buffer[buf_answer_count] = (command_t *)answer;
      buf_answer_count++;

      pthread_cond_broadcast(&cond_consumer_answers);
  		pthread_mutex_unlock(&mutex2);
  	}
}

void *answer_routine(){
  answer_t *answer=NULL;

  while(1){
    pthread_mutex_lock(&mutex2);
    while(buf_answer_count == 0){
      pthread_cond_wait(&cond_consumer_answers , &mutex2);
    }
    buf_answer_count--;
    answer = (answer_t *)answer_buffer[buf_answer_count];

    /*send the answer to the client*/
    if(send_answer_to_client(answer) == -1){
          fprintf(stderr, "Warning: unable to answer command from client %lu\n", answer->key);
    }

    pthread_cond_broadcast(&cond_producer_answers);
    pthread_mutex_unlock(&mutex2);
  }

}

/******************************** Main Thraed ********************************/
int main(int argc, char *argv[])
{
    /*local variable declarations*/
    int sockfd, newsockfd;
    int portno=BABBLE_PORT;

    buf_count =0;
    buf_answer_count =0;

    int opt;
    int nb_args=1;

    pthread_t tid[MAX_CLIENT] , exe_tid[BABBLE_EXECUTOR_THREADS];
    pthread_t ans_tid[BABBLE_ANSWER_THREADS];
    int client_counter = 0 ;
    int exe_counter =0;
    int ans_counter =0;
    /*END declarations*/

    /*Parse input arguments*/
    while ((opt = getopt (argc, argv, "+p:")) != -1){
        switch (opt){
        case 'p':
            portno = atoi(optarg);
            nb_args+=2;
            break;
        case 'h':
        case '?':
        default:
            display_help(argv[0]);
            return -1;
        }
    }

    if(nb_args != argc){
        display_help(argv[0]);
        return -1;
    }
    /*END INPUT Parsing*/

    /*Init server data structure*/
    server_data_init();
    /*END Server INIT*/

    /*Init and open the server socket*/
    if((sockfd = server_connection_init(portno)) == -1){
        return -1;
    }
    printf("Babble server bound to port %d\n", portno);
    /*END SOCKET INIT*/

    /*create executor threads*/
    while(exe_counter < BABBLE_EXECUTOR_THREADS){
        if(pthread_create (&exe_tid[exe_counter], NULL, (void *)executor_routine, NULL) != 0){
            fprintf(stderr,"Failed to create the using threadkkk\n");
            return EXIT_FAILURE;
        }
        exe_counter++;
    }

    /*create answer threads*/
    while(ans_counter < BABBLE_ANSWER_THREADS){
        if(pthread_create (&ans_tid[ans_counter], NULL, (void *) answer_routine, NULL)!= 0){
            fprintf(stderr,"Failed to create the Answer Threads thread\n");
            return EXIT_FAILURE;
        }
        ans_counter++;
    }

    /*main Server loop*/
    while(1){
        if((newsockfd= server_connection_accept(sockfd))==-1){
            return -1;
        }

        /*create a communication thread for each client*/
        if(pthread_create (&tid[client_counter], NULL, (void *) comm_routine, (void*)(intptr_t) newsockfd) != 0){
            fprintf(stderr,"Failed to create the generating thread\n");
            return EXIT_FAILURE;
        }
        client_counter++;
    }
    /*END Server loop*/
    close(sockfd);
    return 0;
}
