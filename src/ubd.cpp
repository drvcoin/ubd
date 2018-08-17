#include "ubd.h"

#include <arpa/inet.h>
#include <asm/byteorder.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <thread>

static int ubd_read(int fd, void * buffer, size_t size)
{
  size_t offset = 0;
  while (offset < size)
  {
    size_t bytesRead = read(fd, ((uint8_t*)buffer) + offset, size - offset);
    if (bytesRead == (size_t)-1)
    {
      return -1;
    }
    offset += bytesRead;
  }

  return 0;
}

static int ubd_write(int fd, void * buffer, size_t size)
{
  size_t offset = 0;
  while (offset < size)
  {
    size_t bytesWritten = write(fd, ((uint8_t*)buffer) + offset, size - offset);
    if (bytesWritten == (size_t)-1)
    {
      return -1;
    }
    offset += bytesWritten;
  }

  return 0;
}

bool ubd_disconnect(const char * nbdPath)
{
  int nbd = open(nbdPath, O_WRONLY);

  if (nbd == -1)
  {
    fprintf(stderr, "Failed to open \"%s\": %s\n", nbdPath, strerror(errno));
    return false;
  }

  int err = ioctl(nbd, NBD_DISCONNECT);
  fprintf(stderr, "nbd device '%s' disconnected with code %d\n",nbdPath, err);

  if (err == -1)
  {
    fprintf(stderr, "%s\n", strerror(errno));
  }

  close(nbd);
  return true;
}

int ubd_register(const char * nbdPath, size_t size, uint32_t timeout, struct ubd_operations * operations, void * context)
{
  if (nbdPath == NULL)
  {
    fprintf(stderr, "Invalid argument: nbdPath\n");
    return -1;
  }

  if (size == 0)
  {
    fprintf(stderr, "Invalid argument: size\n");
    return -1;
  }

  if (operations == NULL || operations->read == NULL || operations->write == NULL)
  {
    fprintf(stderr, "Invalid argument: operations\n");
    return -1;
  }

  uint64_t blockSize = 514*1024;

  printf("Initializing %s with size=%ld blsize=%ld blocks=%ld\n", nbdPath, size, blockSize, size/blockSize);
  int sv[2] = {0};

  int err = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);

  int parentfd = sv[1];
  int childfd = sv[0];

  int nbd = open(nbdPath, O_RDWR);
  if (nbd == -1)
  {
    fprintf(stderr, "Failed to open \"%s\": %s\n", nbdPath, strerror(errno));
    return 1;
  }

  blockSize = 4096;

  // TODO: pass timeout from caller
  if (timeout > 1000)
  {
    err = ioctl(nbd, NBD_SET_TIMEOUT, (timeout - 1) / 1000 + 1);
    printf("NBD_SET_TIMEOUT(%d)=%d\n", (timeout - 1) / 1000 + 1, err);
  }

  err = ioctl(nbd, NBD_SET_BLKSIZE, blockSize);
  printf("NBD_SET_BLKSIZE(%ld)=%d\n", blockSize, err);
  err = ioctl(nbd, NBD_SET_SIZE_BLOCKS, size / blockSize);
  printf("NBD_SET_SIZE_BLOCKS(%ld)=%d\n", size / blockSize, err);
  err = ioctl(nbd, NBD_CLEAR_SOCK);

  std::thread th1([nbd, parentfd]{
    int err;
    if(ioctl(nbd, NBD_SET_SOCK, parentfd) == -1)
    {
      fprintf(stderr, "Failed to set socket handle: %s\n", strerror(errno));
    }
#if defined NBD_SET_FLAGS && defined NBD_FLAG_SEND_TRIM
    else if(ioctl(nbd, NBD_SET_FLAGS, NBD_FLAG_SEND_TRIM) == -1)
    {
      fprintf(stderr, "Failed to set trim flags: %s\n", strerror(errno));
    }
#endif
    else
    {
      err = ioctl(nbd, NBD_DO_IT);
      fprintf(stderr, "nbd device terminated with code %d\n", err);
      if (err == -1)
      {
        fprintf(stderr, "%s\n", strerror(errno));
      }
    }

    ioctl(nbd, NBD_CLEAR_QUE);
    ioctl(nbd, NBD_CLEAR_SOCK);
    close(parentfd);
    close(nbd);
  });
  th1.detach();

  std::thread th2([childfd, nbdPath, size, operations, context = std::move(context)]() mutable {
    struct nbd_request request = {0};
    struct nbd_reply reply = {0};
    int bytesRead;

    reply.magic = __be32_to_cpu(NBD_REPLY_MAGIC);

    while ((bytesRead = read(childfd, &request, sizeof(request))) > 0)
    {
      memcpy(reply.handle, request.handle, sizeof(reply.handle));
      reply.error = 0;

      size_t len = __be32_to_cpu(request.len);
      size_t from = __be64_to_cpu(request.from);

      switch(__be32_to_cpu(request.type))
      {
        case NBD_CMD_READ:
        {
          void * buffer = malloc(len);
          reply.error = __cpu_to_be32(operations->read(buffer, len, from, context));
          ubd_write(childfd, &reply, sizeof(struct nbd_reply));
          ubd_write(childfd, buffer, len);
          free(buffer);
          break;
        }
        case NBD_CMD_WRITE:
        {
          void * buffer = malloc(len);
          ubd_read(childfd, buffer, len);
          reply.error = __cpu_to_be32(operations->write(buffer, len, from, context));
          free(buffer);
          ubd_write(childfd, &reply, sizeof(struct nbd_reply));
          break;
        }
        case NBD_CMD_DISC:
        {
          close(childfd);
          if (operations->disc)
          {
            operations->disc(context);
          }
          if (operations->cleanup)
          {
            operations->cleanup(context);
          }
          return 0;
        }
#ifdef NBD_FLAG_SEND_FLUSH
        case NBD_CMD_FLUSH:
        {
          if (operations->flush)
          {
            reply.error = __cpu_to_be32(operations->flush(context));
          }
          ubd_write(childfd, &reply, sizeof(struct nbd_reply));
          break;
        }
#endif
#ifdef NBD_FLAG_SEND_TRIM
        case NBD_CMD_TRIM:
        {
          if (operations->trim)
          {
            reply.error = __cpu_to_be32(operations->trim(from, len, context));
          }
          ubd_write(childfd, &reply, sizeof(struct nbd_reply));
          break;
        }
#endif
      }
    }
    if (bytesRead == -1)
    {
      fprintf(stderr, "Error reading from socket: %s\n", strerror(errno));
    }

    close(childfd);
    if (operations->cleanup)
    {
      operations->cleanup(context);
    }

    return 0;
  });

  th2.detach();

  return 0;
}
