# Makefile to build helloword.cpp

CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra
TARGET = helloword
OBJS = helloworld.o

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJS)


helloworld.o: helloworld.cpp
	$(CXX) $(CXXFLAGS) -c helloworld.cpp -o helloworld.o

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) *.o

# show help
help:
	@echo "Usage: make [all|run|clean]"
