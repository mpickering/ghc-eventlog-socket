// For POLLRDHUP
#define _GNU_SOURCE

#include <assert.h>
#include <stdbool.h>
#include <poll.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>

#include <Rts.h>

static bool initialized = false;

static pthread_t listen_thread;
static pthread_cond_t new_conn_cond;

static pthread_mutex_t mutex;
static int listen_fd = -1;
static volatile int client_fd = -1;

#define LISTEN_BACKLOG 5

#define PRINT_ERR(...) \
  fprintf(stderr, "ghc-eventlog-socket: " __VA_ARGS__)

/*********************************************************************************
 * EventLogWriter
 *********************************************************************************/

static void writer_init(void)
{
  // no-op
}

static bool writer_write(void *eventlog, size_t sz)
{
  pthread_mutex_lock(&mutex);
  int fd = client_fd;
  if (fd < 0) {
    pthread_mutex_unlock(&mutex);
    return true;
  }

  while (sz > 0) {
    int ret = write(fd, eventlog, sz);
    if (ret == -1) {
      PRINT_ERR("failed to write: %s\n", strerror(errno));
      pthread_mutex_unlock(&mutex);
      // N.B. we still claim that the write finished since it is expected that
      // consumers come and go freely.
      return true;
    }

    sz -= ret;
  }

  pthread_mutex_unlock(&mutex);
  return true;
}

static void writer_flush(void)
{
  // no-op
}

static void writer_stop(void)
{
  pthread_mutex_lock(&mutex);
  if (client_fd >= 0)
    close(client_fd);

  client_fd = -1;
  pthread_mutex_unlock(&mutex);
}

const EventLogWriter socket_writer = {
  .initEventLogWriter = writer_init,
  .writeEventLog = writer_write,
  .flushEventLog = writer_flush,
  .stopEventLogWriter = writer_stop
};

/*********************************************************************************
 * Initialization
 *********************************************************************************/

static void *listen_socket(void * _unused)
{
  while (true) {
    if (listen(listen_fd, LISTEN_BACKLOG) == -1) {
      PRINT_ERR("listen() failed: %s\n", strerror(errno));
      abort();
    }

    struct sockaddr_un remote;
    int len;
    int fd = accept(listen_fd, (struct sockaddr *) &remote, &len);
    pthread_mutex_lock(&mutex);
    client_fd = fd;
    // Drop lock to allow initial batch of events to be written.
    pthread_mutex_unlock(&mutex);
    startEventLogging(&socket_writer);

    // Announce new connection
    pthread_cond_broadcast(&new_conn_cond);

    // Wait for socket to disconnect before listening again.
    struct pollfd pfd = {
      .fd = client_fd,
      .events = POLLRDHUP,
      .revents = 0,
    };
    while (true) {
      int ret = poll(&pfd, 1, -1);
      if (ret == -1 && errno != EAGAIN) {
        // error
        PRINT_ERR("poll() failed: %s\n", strerror(errno));
        break;
      } else if (ret > 0) {
        // disconnected
        break;
      }
    }
    client_fd = -1;
    endEventLogging();
  }

  return NULL;
}

static void open_socket(const char *sock_path)
{
  listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);

  struct sockaddr_un local;
  local.sun_family = AF_UNIX;
  strncpy(local.sun_path, sock_path, sizeof(local.sun_path) - 1);
  unlink(sock_path);
  if (bind(listen_fd, (struct sockaddr *) &local,
           sizeof(struct sockaddr_un)) == -1) {
    PRINT_ERR("failed to bind socket %s: %s\n", sock_path, strerror(errno));
    abort();
  }

  int ret = pthread_create(&listen_thread, NULL, listen_socket, NULL);
  if (ret != 0) {
    PRINT_ERR("failed to spawn thread: %s\n", strerror(ret));
  }
}


static void wait_for_connection(void)
{
  pthread_mutex_lock(&mutex);
  while (client_fd == -1) {
    assert(pthread_cond_wait(&new_conn_cond, &mutex) == 0);
  }
  pthread_mutex_unlock(&mutex);
}

/*********************************************************************************
 * Entrypoint
 *********************************************************************************/

// Copied from GHC, for now.
static void read_trace_flags(const char *arg)
{
    const char *c;
    bool enabled = true;
    /* Syntax for tracing flags currently looks like:
     *
     *   -l    To turn on eventlog tracing with default trace classes
     *   -lx   Turn on class 'x' (for some class listed below)
     *   -l-x  Turn off class 'x'
     *   -la   Turn on all classes
     *   -l-a  Turn off all classes
     *
     * This lets users say things like:
     *   -la-p    "all but sparks"
     *   -l-ap    "only sparks"
     */

    /* Start by turning on the default tracing flags.
     *
     * Currently this is all the trace classes, except full-detail sparks.
     * Similarly, in future we might default to slightly less verbose
     * scheduler or GC tracing.
     */
    RtsFlags.TraceFlags.scheduler      = true;
    RtsFlags.TraceFlags.gc             = true;
    RtsFlags.TraceFlags.sparks_sampled = true;
    RtsFlags.TraceFlags.user           = true;

    for (c  = arg; *c != '\0'; c++) {
        switch(*c) {
        case '\0':
            break;
        case '-':
            enabled = false;
            break;
        case 'a':
            RtsFlags.TraceFlags.scheduler      = enabled;
            RtsFlags.TraceFlags.gc             = enabled;
            RtsFlags.TraceFlags.sparks_sampled = enabled;
            RtsFlags.TraceFlags.sparks_full    = enabled;
            RtsFlags.TraceFlags.user           = enabled;
            enabled = true;
            break;

        case 's':
            RtsFlags.TraceFlags.scheduler = enabled;
            enabled = true;
            break;
        case 'p':
            RtsFlags.TraceFlags.sparks_sampled = enabled;
            enabled = true;
            break;
        case 'f':
            RtsFlags.TraceFlags.sparks_full = enabled;
            enabled = true;
            break;
        case 't':
            RtsFlags.TraceFlags.timestamp = enabled;
            enabled = true;
            break;
        case 'g':
            RtsFlags.TraceFlags.gc        = enabled;
            enabled = true;
            break;
        case 'n':
            RtsFlags.TraceFlags.nonmoving_gc = enabled;
            enabled = true;
            break;
        case 'u':
            RtsFlags.TraceFlags.user      = enabled;
            enabled = true;
            break;
        case 'T':
#if defined(TICKY_TICKY)
            RtsFlags.TraceFlags.ticky     = enabled;
            enabled = true;
            break;
#else
            errorBelch("Program not compiled with ticky-ticky support");
            break;
#endif
        default:
            errorBelch("unknown trace option: %c",*c);
            break;
        }
    }
}

void eventlog_socket_start(const char *sock_path, bool wait)
{
  if (!initialized) {
    pthread_mutex_init(&mutex, NULL);
    pthread_cond_init(&new_conn_cond, NULL);
    initialized = true;
  }

  if (!sock_path) {
    sock_path = getenv("GHC_EVENTLOG_SOCKET");
  }
  if (!sock_path)
    return;

  if (eventLogStatus() == EVENTLOG_NOT_SUPPORTED) {
    PRINT_ERR("eventlog is not supported.\n");
    return;
  }

  if (eventLogStatus() == EVENTLOG_RUNNING) {
    endEventLogging();
  }

  RtsFlags.TraceFlags.tracing = TRACE_EVENTLOG;
  read_trace_flags("");

  open_socket(sock_path);
  if (wait) {
    printf("ghc-eventlog-socket: Waiting for connection to %s...\n", sock_path);
    wait_for_connection();
  }
}

