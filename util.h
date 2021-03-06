/*
 * Copyright (C) 2020  Edward LEI <edward_lei72@hotmail.com>
 *
 * The code is licensed under the MIT license
 */

#ifndef _UTIL_H_
#define _UTIL_H_


#include <stdint.h>


#define I2S_SIZE 25  /* 3*sizeof(size_t) + 1 */


char *uitos(size_t value, char dst[I2S_SIZE], size_t *len);
char *itos(ssize_t value, char dst[I2S_SIZE], size_t *len);

void u64tohex(char *s, uint64_t num, int lower_alpha);
void u64tohex2(char *s, uint64_t num, int lower_alpha);

char *split_kv(char *kv, char delim);
void gmt_date(char *date_gmt, long *tmgmt);
long mk_etag(char *etag, char *file);

char *find_ext(char *file);

int msleep(long tms);
long mstime();


#endif
