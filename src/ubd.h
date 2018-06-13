#pragma once

#include <linux/nbd.h>
#include <linux/types.h>
#include <sys/types.h>

struct ubd_operations
{
  size_t (*read)(void * buffer, size_t size, size_t offset, void * context);
  size_t (*write)(const void * buffer, size_t size, size_t offset, void * context);
  void (*disc)(void * context);
  int (*flush)(void * context);
  int (*trim)(size_t start, size_t size, void * context);
  void (*cleanup)(void * context);
};

int ubd_register(const char * path, size_t size, struct ubd_operations * operations, void * context);
bool ubd_disconnect(const char * nbdPath);
