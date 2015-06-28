#ifndef util_h
#define util_h

#include <sys/types.h>
#include <stdarg.h>
#include <stdint.h>

#include <c_types.h>
#include <osapi.h>
#include <ets_sys.h>

// prototypes missing

size_t strlcpy(char *, const char *, size_t);
size_t strlcat(char *, const char *, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);

unsigned long int strtoul(const char *, char **, int);
unsigned long long int strtoull(const char *, char **, int);

void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);

void *pvPortMalloc(size_t);
int ets_vsnprintf(char *, size_t, const char *, va_list);

void ets_isr_attach(int, void *, void *);
void ets_isr_mask(unsigned intr);
void ets_isr_unmask(unsigned intr);
void ets_timer_arm(ETSTimer *, uint32_t, bool);
void ets_timer_arm_new(ETSTimer *, uint32_t, bool, int);
void ets_timer_disarm(ETSTimer *);
void ets_timer_setfn(ETSTimer *, ETSTimerFunc *, void *);
void ets_install_putc1(void(*)(char));

// local utility functions missing from libc

int snprintf(char *, size_t, const char *, ...) __attribute__ ((format (printf, 3, 4)));
void *malloc(size_t);
int atoi(const char *);

// other handy functions

void reset(void);
const char *yesno(uint8_t value);
const char *onoff(uint8_t value);
int dprintf(const char *fmt, ...);

#endif
