HEADERS = \
  ubd.h

SOURCES = \
  ubd.cpp

TARGET = ubd
TARGET_TYPE = static
CCFLAGS = -std=c++11 -msse4.1
LDFLAGS = 

include Cpp.mk

ifeq ($(TARGET),)
$(error "Missing value for TARGET")
endif
