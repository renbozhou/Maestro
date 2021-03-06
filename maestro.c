/*
 * Copyright (C) 2020  Edward LEI <edward_lei72@hotmail.com>
 *
 * The code is licensed under the MIT license
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/sysinfo.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "util.h"
#include "linkedlist.h"
#include "thpool.h"
#include "http_conn.h"
#include "debug.h"


#define THREADS_PER_CORE 64
#define MAXEVENTS 2048

#define HTTP_KEEPALIVE_TIME 10000
#define PORT 9000


static void
_set_nonblocking(int fd)
{
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    perror("fcntl()");
    return;
  }
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
    perror("fcntl()");
}

static void
_expire_timers(list_t *timers)
{
  httpconn_t *conn;
  int sockfd;

  node_t *timer;
  long cur_time;
  long stamp;

  timer = list_first(timers);
  if (timer) {
    cur_time = mstime();
    do {
      stamp = list_node_stamp(timer);

      if (cur_time - stamp >= HTTP_KEEPALIVE_TIME) {
        conn = (httpconn_t *)list_node_data(timer);
        sockfd = httpconn_sockfd(conn);
        printf("[CONN] socket closed [%d]\n", sockfd);
        DEBSI("[CONN] server disconnected", sockfd);
        close(sockfd);

        list_del(timers, stamp);
      }

      timer = list_next(timers);
    } while (timer);
  }
}

static volatile int svc_running = 1;

static void
_svc_stopper(int dummy)
{
  svc_running = 0;
}


int
main(int argc, char** argv)
{
  int srvfd;
  int clifd;
  int sockfd;
  int opt = 1;
  int rc, i;

  int epfd;
  int nevents;
  struct epoll_event event;
  struct epoll_event *events;

  struct sockaddr_in srvaddr;
  struct sockaddr cliaddr;
  socklen_t len_cliaddr = sizeof(struct sockaddr);
  char *cli_ip;

  httpconn_t *srvconn;
  httpconn_t *cliconn;
  httpconn_t *conn;

  int np;
  thpool_t *taskpool;

  list_t *timers;

  /*
   * install signal handle for SIGPIPE
   * when a fd is closed by remote, writing to this fd will cause system send
   * SIGPIPE to this process, which exit the program
   */
  struct sigaction sa;
  memset(&sa, '\0', sizeof(struct sigaction));
  sa.sa_handler = SIG_IGN;
  sa.sa_flags = 0;
  if (sigaction(SIGPIPE, &sa, 0)) {
    DEBS("install sigal handler for SIGPIPE failed");
    return 0;
  }

  /* ctrl-c handler */
  signal(SIGINT, _svc_stopper);


  /* detect number of cpu cores and use it for thread pool */
  np = get_nprocs();
  taskpool = thpool_init(np * THREADS_PER_CORE);
  /* list of timers */
  timers = list_new();


  /* create the server socket */
  srvfd = socket(AF_INET, SOCK_STREAM, 0);
  if (srvfd == -1) {
    perror("socket()");
    return 1;
  }

  rc = setsockopt(srvfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  if (rc == -1) {
    perror("setsockopt()");
    return 1;
  }

  /* bind */
  memset(&srvaddr, 0, sizeof(struct sockaddr_in));
  srvaddr.sin_family = AF_INET;
  srvaddr.sin_addr.s_addr = htonl(INADDR_ANY);
  srvaddr.sin_port = htons(PORT);
  rc = bind(srvfd, (struct sockaddr*)&srvaddr, sizeof(struct sockaddr_in));
  if (rc < 0) {
    perror("bind()");
    return 1;
  }

  /* make it nonblocking, and then listen */
  _set_nonblocking(srvfd);
  if (listen(srvfd, SOMAXCONN) < 0) {
    perror("listen()");
    return 1;
  }
  printf("listening on port [%d]\n", PORT);

  /* create the epoll socket */
  epfd = epoll_create1(0);
  if (epfd == -1) {
    perror("epoll_create1()");
    return 1;
  }

  /* mark the server socket for reading, and become edge-triggered */
  memset(&event, 0, sizeof(struct epoll_event));
  srvconn = httpconn_new(srvfd, epfd, NULL);
  event.data.ptr = (void *)srvconn;
  event.events = EPOLLIN | EPOLLET;
  rc = epoll_ctl(epfd, EPOLL_CTL_ADD, srvfd, &event);
  if (rc == -1) {
    perror("epoll_ctl()");
    return 1;
  }

  events = calloc(MAXEVENTS, sizeof(struct epoll_event));

  do {
    nevents = epoll_wait(epfd, events, MAXEVENTS, HTTP_KEEPALIVE_TIME);
    if (nevents == -1) perror("epoll_wait()");

    /* expire the timers */
    _expire_timers(timers);


    /* loop through events */
    for (i = 0; i < nevents; i++) {
      conn = (httpconn_t *)events[i].data.ptr;
      sockfd = httpconn_sockfd(conn);

      /* error case */
      if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP) ||
          (!(events[i].events & EPOLLIN))) {
        perror("EPOLL ERR|HUP|OUT");
        list_update(timers, conn, mstime());
        break;
      }

      else if (sockfd == srvfd) {
        /* server socket; accept connections */
        do {
          clifd = accept(srvfd, &cliaddr, &len_cliaddr);

          if (clifd == -1) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
              /* we processed all of the connections */
              break;
            }
            perror("accept()");
            close(clifd);
            break;
          }

          cli_ip = inet_ntoa(((struct sockaddr_in *)&cliaddr)->sin_addr);
          printf("[%s] connected on socket [%d]\n", cli_ip, clifd);

          _set_nonblocking(clifd);
          cliconn = httpconn_new(clifd, epfd, timers);
          event.data.ptr = (void *)cliconn;
          /*
           * With the use of EPOLLONESHOT, it is guaranteed that
           * a client file descriptor is only used by one thread
           * at a time
           */
          event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
          rc = epoll_ctl(epfd, EPOLL_CTL_ADD, clifd, &event);
          if (rc == -1) {
            perror("epoll_ctl()");
            return 1;
          }
        } while (1);
      }

      else {
        /* client socket; read client data and process it */
        thpool_add_task(taskpool, httpconn_task, conn);
      }
    }
  } while (svc_running);

  thpool_wait(taskpool);
  thpool_destroy(taskpool);

  list_destroy(timers);
  free(srvconn);
  close(epfd);
  puts("Exit gracefully...");

  return 0;
}
