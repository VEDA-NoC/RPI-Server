CXX = g++
CXXFLAGS = -Wall -Wextra -O2 -std=c++17 -Iinclude
GST_CFLAGS = $(shell pkg-config --cflags gstreamer-1.0)
GST_LIBS = $(shell pkg-config --libs gstreamer-1.0)
SQLITE_CFLAGS = $(shell pkg-config --cflags sqlite3)
SQLITE_LIBS = $(shell pkg-config --libs sqlite3)
LIBS = $(GST_LIBS) $(SQLITE_LIBS) -lpthread

TARGET = app
APP_SRCS = \
	src/config/app_config.cpp \
	src/logging/logger.cpp \
	src/db/recording_index.cpp \
	src/media/channel_ingest.cpp \
	src/storage/storage_manager.cpp \
	src/main.cpp

all: $(TARGET)

$(TARGET): $(APP_SRCS)
	$(CXX) $(CXXFLAGS) $(GST_CFLAGS) $(SQLITE_CFLAGS) $(APP_SRCS) -o $(TARGET) $(LIBS)

debug: CXXFLAGS += -O0 -g
debug: clean $(TARGET)

clean:
	rm -f $(TARGET)
