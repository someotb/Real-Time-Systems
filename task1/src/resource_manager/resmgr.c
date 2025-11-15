/*
 *  Менеджер ресурсов с буфером, командами и правами доступа
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define EXAMPLE_SOCK_PATH "/tmp/example_resmgr.sock"
#define DEVICE_BUF_SIZE 4096

// Права доступа
#define PERM_RW 0
#define PERM_RO 1
#define PERM_WO 2

// Флаги устройства
#define DEV_OPEN 1
#define DEV_BUSY 2
#define DEV_ERROR 4

static const char *progname = "example";
static int optv = 0;
static int listen_fd = -1;

// Структура устройства
typedef struct device
{
  char buffer[DEVICE_BUF_SIZE];
  size_t size;
  int flags;
  int permissions;
  pthread_mutex_t lock;
} device_t;

static device_t dev = {
    .size = 0,
    .flags = 0,
    .permissions = PERM_RW,
    .lock = PTHREAD_MUTEX_INITIALIZER};

static void options(int argc, char *argv[]);
static void install_signals(void);
static void on_signal(int signo);
static void *client_thread(void *arg);
static void handle_command(int fd, const char *cmd);

int main(int argc, char *argv[])
{
  setvbuf(stdout, NULL, _IOLBF, 0);
  printf("%s: starting...\n", progname);
  options(argc, argv);
  install_signals();

  listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd == -1)
  {
    perror("socket");
    return EXIT_FAILURE;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, EXAMPLE_SOCK_PATH, sizeof(addr.sun_path) - 1);

  unlink(EXAMPLE_SOCK_PATH);

  if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
  {
    perror("bind");
    close(listen_fd);
    return EXIT_FAILURE;
  }

  if (listen(listen_fd, 8) == -1)
  {
    perror("listen");
    close(listen_fd);
    unlink(EXAMPLE_SOCK_PATH);
    return EXIT_FAILURE;
  }

  printf("%s: listening on %s\n", progname, EXAMPLE_SOCK_PATH);
  printf("Подключитесь клиентом (например: `nc -U %s`) и отправьте команды.\n", EXAMPLE_SOCK_PATH);

  while (1)
  {
    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd == -1)
    {
      if (errno == EINTR)
        continue;
      perror("accept");
      break;
    }

    if (optv)
      printf("%s: io_open — новое подключение (fd=%d)\n", progname, client_fd);

    pthread_t th;
    if (pthread_create(&th, NULL, client_thread, (void *)(long)client_fd) != 0)
    {
      perror("pthread_create");
      close(client_fd);
      continue;
    }
    pthread_detach(th);
  }

  if (listen_fd != -1)
    close(listen_fd);
  unlink(EXAMPLE_SOCK_PATH);
  return EXIT_SUCCESS;
}

static void *client_thread(void *arg)
{
  int fd = (int)(long)arg;
  char buf[1024];

  // Отмечаем устройство как занятое
  pthread_mutex_lock(&dev.lock);
  dev.flags |= DEV_OPEN | DEV_BUSY;
  pthread_mutex_unlock(&dev.lock);

  for (;;)
  {
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0)
      break;
    buf[n] = '\0'; // гарантируем строку

    handle_command(fd, buf);
  }

  pthread_mutex_lock(&dev.lock);
  dev.flags &= ~DEV_BUSY;
  pthread_mutex_unlock(&dev.lock);

  close(fd);
  if (optv)
    printf("%s: клиент отключился (fd=%d)\n", progname, fd);
  return NULL;
}

static void handle_command(int fd, const char *cmd)
{
    char resp[1024];

    // Игнорируем аргументы для READ, всегда читаем весь буфер
    if (strncmp(cmd, "READ", 4) == 0) {
        pthread_mutex_lock(&dev.lock);
        if (dev.permissions == PERM_WO) {  // проверка на право чтения
            pthread_mutex_unlock(&dev.lock);
            write(fd, "Permission denied\n", 18);
            return;
        }
        if (dev.size > 0) {
            if (write(fd, dev.buffer, dev.size) < 0) perror("write");
        } else {
            write(fd, "<buffer empty>\n", 15);
        }
        pthread_mutex_unlock(&dev.lock);
        return;
    }

    if (strncmp(cmd, "WRITE ", 6) == 0) {
        pthread_mutex_lock(&dev.lock);
        if (dev.permissions == PERM_RO) {  // проверка на право записи
            pthread_mutex_unlock(&dev.lock);
            write(fd, "Permission denied\n", 18);
            return;
        }
        const char *data = cmd + 6;
        size_t len = strlen(data);
        if (len + dev.size > DEVICE_BUF_SIZE) len = DEVICE_BUF_SIZE - dev.size;
        memcpy(dev.buffer + dev.size, data, len);
        dev.size += len;
        pthread_mutex_unlock(&dev.lock);
        snprintf(resp, sizeof(resp), "OK, %zu bytes written\n", len);
        write(fd, resp, strlen(resp));
        return;
    }

    if (strcmp(cmd, "INFO\n") == 0) {
        pthread_mutex_lock(&dev.lock);
        snprintf(resp, sizeof(resp), "Size=%zu, Flags=%d, Permissions=%d\n",
                 dev.size, dev.flags, dev.permissions);
        pthread_mutex_unlock(&dev.lock);
        write(fd, resp, strlen(resp));
        return;
    }

    if (strcmp(cmd, "CLEAR\n") == 0) {
        pthread_mutex_lock(&dev.lock);
        if (dev.permissions == PERM_RO) {  // нельзя очищать если только чтение
            pthread_mutex_unlock(&dev.lock);
            write(fd, "Permission denied\n", 18);
            return;
        }
        dev.size = 0;
        pthread_mutex_unlock(&dev.lock);
        write(fd, "Buffer cleared\n", 15);
        return;
    }

    if (strncmp(cmd, "SETPERM ", 8) == 0) {
        pthread_mutex_lock(&dev.lock);
        if (strncmp(cmd + 8, "rw", 2) == 0) dev.permissions = PERM_RW;
        else if (strncmp(cmd + 8, "ro", 2) == 0) dev.permissions = PERM_RO;
        else if (strncmp(cmd + 8, "wo", 2) == 0) dev.permissions = PERM_WO;
        else {
            pthread_mutex_unlock(&dev.lock);
            write(fd, "Unknown permission\n", 19);
            return;
        }
        pthread_mutex_unlock(&dev.lock);
        write(fd, "Permission set\n", 15);
        return;
    }

    write(fd, "Unknown command\n", 16);
}

static void options(int argc, char *argv[])
{
  int opt;
  optv = 0;
  while ((opt = getopt(argc, argv, "v")) != -1)
  {
    switch (opt)
    {
    case 'v':
      optv++;
      break;
    }
  }
}

static void install_signals(void)
{
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
}

static void on_signal(int signo)
{
  (void)signo;
  if (listen_fd != -1)
    close(listen_fd);
  unlink(EXAMPLE_SOCK_PATH);
  fprintf(stderr, "\n%s: завершение по сигналу\n", progname);
  _exit(0);
}
