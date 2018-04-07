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

int ubd_register(const char * nbdPath, size_t size, struct ubd_operations * operations, void * context)
{
  uint64_t blockSize = 514*1024;

  printf("Initializing %s with size=%ld blsize=%ld blocks=%ld\n", nbdPath, size, blockSize, size/blockSize);

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

  err = ioctl(nbd, NBD_SET_BLKSIZE, blockSize);
  printf("NBD_SET_BLKSIZE(%ld)=%d\n", blockSize, err);
  err = ioctl(nbd, NBD_SET_SIZE_BLOCKS, size / blockSize);
  printf("NBD_SET_SIZE_BLOCKS(%ld)=%d\n", size / blockSize, err);
  err = ioctl(nbd, NBD_CLEAR_SOCK);

  if (!fork())
  {
    close(childfd);

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

    exit(0);
  }

  int htmp = open(nbdPath, O_RDONLY);
  close(htmp);

  close(parentfd);

  struct nbd_request request = {0};
  struct nbd_reply reply = {0};

  reply.magic = __be32_to_cpu(NBD_REPLY_MAGIC);

  int bytesRead;
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
        if (operations->disc)
        {
          operations->disc(context);
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
  return 0;
}
