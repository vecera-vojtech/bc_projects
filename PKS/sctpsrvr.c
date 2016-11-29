/*
 *  sctpsrvr.c
 *
 *  SCTP multi-stream server.
 *
 *  M. Tim Jones <mtj@mtjones.com>
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/sctp.h>
#include "common.h"

#include <errno.h>
int listenSock, connSock;
int loop = 1;
void sig_handler(int signo)
{
    if (signo == SIGINT)
	printf("received SIGINT\n");
    close(connSock);
    close(listenSock);
    loop = 0;
    exit(1);
}

int main()
{
  int ret;
  struct sockaddr_in servaddr;
  struct sctp_initmsg initmsg;
  char buffer[MAX_BUFFER+1] = {0};
  time_t currentTime;
  struct stat fsrv_stat, fclnt_stat;
  FILE* fsrv, *fclnt;

  signal(SIGINT, sig_handler);
  signal(SIGPIPE, SIG_IGN);
  perror("signal: ");
  /* Create SCTP TCP-Style Socket */
  listenSock = socket( AF_INET, SOCK_STREAM, IPPROTO_SCTP );

  /* Accept connections from any interface */
  bzero( (void *)&servaddr, sizeof(servaddr) );
  servaddr.sin_family = AF_INET;
  servaddr.sin_addr.s_addr = htonl( INADDR_ANY );
  servaddr.sin_port = htons(MY_PORT_NUM);

  int reuse = 1;
  if (setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0)
      perror("setsockopt(SO_REUSEADDR) failed");

#ifdef SO_REUSEPORT
  if (setsockopt(listenSock, SOL_SOCKET, SO_REUSEPORT, (const char*)&reuse, sizeof(reuse)) < 0) 
      perror("setsockopt(SO_REUSEPORT) failed");
#endif
  /* Specify that a maximum of 5 streams will be available per socket */
  memset( &initmsg, 0, sizeof(initmsg) );
  initmsg.sinit_num_ostreams = 5;
  initmsg.sinit_max_instreams = 5;
  initmsg.sinit_max_attempts = 4;

  if ((ret = setsockopt( listenSock, IPPROTO_SCTP, SCTP_INITMSG, 
                     &initmsg, sizeof(initmsg) )) < 0)
    perror("Setsockopt: ");
    

  if ((ret = bind( listenSock, (struct sockaddr *)&servaddr, sizeof(servaddr) )) < 0)
    perror("bind: ");


  /* Place the server socket into the listening state */
  listen( listenSock, 5 );

  if ((fsrv = fopen("sctpsrvr.c", "r")) < 0)
      printf("Couldnt open file: sctpsrvr.c");

  if (fstat(fileno(fsrv), &fsrv_stat) < 0)
      printf("Couldnt obtain file stats for file: sctpsrvr.c");
  printf("File size: %d bytes\n", (int)fsrv_stat.st_size);

  if ((fclnt = fopen("sctpclnt.c", "r")) < 0)
      printf("Couldnt open file: sctpclntr.c");

  if (fstat(fileno(fclnt), &fclnt_stat) < 0)
      printf("Couldnt obtain file stats for file: sctpclntr.c");
  printf("File size: %d bytes\n", (int)fclnt_stat.st_size);

  /* Server loop... */
  while( loop ) {

    /* Await a new client connection */
    printf("Awaiting a new connection\n");

    if ((connSock = accept( listenSock, (struct sockaddr *)NULL, (socklen_t *)NULL ))< 0)
	perror("accept(): ");

    /* New client socket has connected */


    /* Grab the current time */
    currentTime = time(NULL);

    /* Send local time on stream 0 (local time stream) */
    snprintf( buffer, MAX_BUFFER, "%s\n", ctime(&currentTime) );
    if ((ret = sctp_sendmsg( connSock, (void *)buffer, (size_t)strlen(buffer),
                         NULL, 0, 0, 0, LOCALTIME_STREAM, 0, 0 )) < 0)
	perror("sctp_sendmsg2: ");

    /* Send GMT on stream 1 (GMT stream) */
    snprintf( buffer, MAX_BUFFER, "%s\n", asctime( gmtime( &currentTime ) ) );
    if ((ret = sctp_sendmsg( connSock, (void *)buffer, (size_t)strlen(buffer),
                         NULL, 0, 0, 0, GMT_STREAM, 0, 0 )) < 0)
	perror("sctp_sendmsg3: ");


    /* Send file length */
    for (int i = 0; i < 10; i++){
    snprintf( buffer, MAX_BUFFER, "%d\n", (int)fsrv_stat.st_size);
    if ((ret = sctp_sendmsg( connSock, (void *)buffer, (size_t)strlen(buffer),
                        NULL, 0, 0, 0, 2, 0, 0 )) < 0)
	perror("sctp_sendmsg: ");
    
    snprintf( buffer, MAX_BUFFER, "%d\n", (int)fclnt_stat.st_size);
    if ((ret = sctp_sendmsg( connSock, (void *)buffer, (size_t)strlen(buffer),
                        NULL, 0, 0, 0, 3, 0, 0 )) < 0)
	perror("sctp_sendmsg: ");
    }
    /* Close the client connection */
    close( connSock );

  }
  return 0;
}

