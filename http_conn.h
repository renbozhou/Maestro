/*
 * Copyright (C) 2020  Edward LEI <edward_lei72@hotmail.com>
 *
 * The code is licensed under the MIT license
 */

#ifndef _HTTPCONN_H_
#define _HTTPCONN_H_


typedef struct _httpconn httpconn_t;


httpconn_t *httpconn_new(int sockfd, int epfd, void *timers);
void httpconn_set_timer(httpconn_t *conn, void *timerpool);

int httpconn_sockfd(httpconn_t *conn);

void httpconn_task(void *arg);


#endif
