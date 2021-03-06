/* POSIX support for memb alloc / free and other functions needed to
   run the tinyDTSL applications */

#include "memb.h"
#include <stdlib.h>
#include "../support.h"
#include "../dtls_debug.h"
#include "../dtls_config.h"
#include "../dtls_time.h"

#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>

extern char *loglevels[];

/**
 * A length-safe strlen() fake.
 *
 * @param s      The string to count characters != 0.
 * @param maxlen The maximum length of @p s.
 *
 * @return The length of @p s.
 */
static inline size_t
dtls_strnlen(const char *s, size_t maxlen) {
  size_t n = 0;
  while(*s++ && n < maxlen)
    ++n;
  return n;
}

#ifdef HAVE_TIME_H

static inline size_t
print_timestamp(char *s, size_t len, time_t t) {
  struct tm *tmp;
  tmp = localtime(&t);
  return strftime(s, len, "%b %d %H:%M:%S", tmp);
}

#else /* alternative implementation: just print the timestamp */

static inline size_t
print_timestamp(char *s, size_t len, clock_time_t t) {
#ifdef HAVE_SNPRINTF
  return snprintf(s, len, "%u.%03u",
		  (unsigned int)(t / CLOCK_SECOND),
		  (unsigned int)(t % CLOCK_SECOND));
#else /* HAVE_SNPRINTF */
  /* @todo do manual conversion of timestamp */
  return 0;
#endif /* HAVE_SNPRINTF */
}

#endif /* HAVE_TIME_H */



void
memb_init(struct memb *m)
{
}


void *
memb_alloc(struct memb *m)
{
  return malloc(m->size);
}

char
memb_free(struct memb *m, void *ptr)
{
  free(ptr);
  return 1;
}


dtls_context_t *
malloc_context() {
  return (dtls_context_t *)malloc(sizeof(dtls_context_t));
}

void
free_context(dtls_context_t *context) {
  free(context);
}

#ifndef NDEBUG
size_t
dsrv_print_addr(const session_t *addr, char *buf, size_t len) {
  const void *addrptr = NULL;
  in_port_t port;
  char *p = buf;

  switch (addr->addr.sa.sa_family) {
  case AF_INET:
    if (len < INET_ADDRSTRLEN)
      return 0;

    addrptr = &addr->addr.sin.sin_addr;
    port = ntohs(addr->addr.sin.sin_port);
    break;
  case AF_INET6:
    if (len < INET6_ADDRSTRLEN + 2)
      return 0;

    *p++ = '[';

    addrptr = &addr->addr.sin6.sin6_addr;
    port = ntohs(addr->addr.sin6.sin6_port);

    break;
  default:
    memcpy(buf, "(unknown address type)", min(22, len));
    return min(22, len);
  }

  if (inet_ntop(addr->addr.sa.sa_family, addrptr, p, len) == 0) {
    perror("dsrv_print_addr");
    return 0;
  }

  p += dtls_strnlen(p, len);

  if (addr->addr.sa.sa_family == AF_INET6) {
    if (p < buf + len) {
      *p++ = ']';
    } else
      return 0;
  }

  p += snprintf(p, buf + len - p + 1, ":%d", port);

  return p - buf;
}

#endif /* NDEBUG */

void
dsrv_log(log_t level, char *format, ...) {
  static char timebuf[32];
  va_list ap;
  FILE *log_fd;

  if (dtls_get_log_level() < level)
    return;

  log_fd = level <= DTLS_LOG_CRIT ? stderr : stdout;

  if (print_timestamp(timebuf,sizeof(timebuf), time(NULL)))
    fprintf(log_fd, "%s ", timebuf);

  if (level <= DTLS_LOG_DEBUG) 
    fprintf(log_fd, "%s ", loglevels[level]);

  va_start(ap, format);
  vfprintf(log_fd, format, ap);
  va_end(ap);
  fflush(log_fd);
}

void
dtls_dsrv_hexdump_log(log_t level, const char *name, const unsigned char *buf, size_t length, int extend) {
  static char timebuf[32];
  FILE *log_fd;
  int n = 0;

  if (dtls_get_log_level() < level)
    return;

  log_fd = level <= DTLS_LOG_CRIT ? stderr : stdout;

  if (print_timestamp(timebuf, sizeof(timebuf), time(NULL)))
    fprintf(log_fd, "%s ", timebuf);

  if (level <= DTLS_LOG_DEBUG)
    fprintf(log_fd, "%s ", loglevels[level]);

  if (extend) {
    fprintf(log_fd, "%s: (%zu bytes):\n", name, length);

    while (length--) {
      if (n % 16 == 0)
	fprintf(log_fd, "%08X ", n);

      fprintf(log_fd, "%02X ", *buf++);

      n++;
      if (n % 8 == 0) {
	if (n % 16 == 0)
	  fprintf(log_fd, "\n");
	else
	  fprintf(log_fd, " ");
      }
    }
  } else {
    fprintf(log_fd, "%s: (%zu bytes): ", name, length);
    while (length--)
      fprintf(log_fd, "%02X", *buf++);
  }
  fprintf(log_fd, "\n");

  fflush(log_fd);
}

int
_dtls_address_equals_impl(const session_t *a,
			  const session_t *b) {
  if (a->ifindex != b->ifindex ||
      a->size != b->size || a->addr.sa.sa_family != b->addr.sa.sa_family)
    return 0;

  /* need to compare only relevant parts of sockaddr_in6 */
 switch (a->addr.sa.sa_family) {
 case AF_INET:
   return 
     a->addr.sin.sin_port == b->addr.sin.sin_port &&
     memcmp(&a->addr.sin.sin_addr, &b->addr.sin.sin_addr,
	    sizeof(struct in_addr)) == 0;
 case AF_INET6:
   return a->addr.sin6.sin6_port == b->addr.sin6.sin6_port &&
     memcmp(&a->addr.sin6.sin6_addr, &b->addr.sin6.sin6_addr,
	    sizeof(struct in6_addr)) == 0;
 default: /* fall through and signal error */
   ;
 }
 return 0;
}

/* --------- time support ----------- */

time_t dtls_clock_offset;

void
dtls_clock_init(void) {
#ifdef HAVE_TIME_H
  dtls_clock_offset = time(NULL);
#else
#  ifdef __GNUC__
  /* Issue a warning when using gcc. Other prepropressors do 
   *  not seem to have a similar feature. */ 
#   warning "cannot initialize clock"
#  endif
  dtls_clock_offset = 0;
#endif
}

void dtls_ticks(dtls_tick_t *t) {
#ifdef HAVE_SYS_TIME_H
  struct timeval tv;
  gettimeofday(&tv, NULL);
  *t = (tv.tv_sec - dtls_clock_offset) * DTLS_TICKS_PER_SECOND 
    + (tv.tv_usec * DTLS_TICKS_PER_SECOND / 1000000);
#else
#error "clock not implemented"
#endif
}

int
dtls_get_random(unsigned long *rand)
{
  FILE *urandom = fopen("/dev/urandom", "r");
  unsigned char buf[sizeof(unsigned long)];

  if (!urandom) {
    dtls_emerg("cannot initialize PRNG\n");
    return 0;
  }

  if (fread(buf, 1, sizeof(buf), urandom) != sizeof(buf)) {
    dtls_emerg("cannot initialize PRNG\n");
    return 0;
  }

  fclose(urandom);

  *rand = (unsigned long)*buf;
  return 1;
}

void
dtls_set_retransmit_timer(dtls_context_t *ctx, unsigned int timeout)
{
/* Do nothing for now ... */
}
