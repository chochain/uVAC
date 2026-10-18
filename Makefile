CC = g++
CC_FLAG = -std=c++17 -g -O2 -Wall -pthread \
          -DXT0_U32=1 -DE4_NOS=0 -DE4_RTOP=1 \
          -fno-pie -no-pie -fcf-protection=none \
          -fomit-frame-pointer -fno-stack-check -fno-stack-protector \
		  -march=native -ffast-math -funroll-loops

SRCS = \
	main/ceforth.cpp 	 \
	main/ceforth_sys.cpp  \
	main/ceforth_task.cpp \

ESPS = \
	main/esp32/xserver2.cpp \
	main/esp32/xforth.cpp   \
	main/esp32/xgl_actor.cpp

FLST = \
	tests/eforth \
	tests/xeforth

exe: tests/eforth

esp: tests/xeforth

all: exe

%.o: %.cpp
	$(CC) $(CC_FLAG) -Imain -c -o $@ $<

tests/eforth: main/main.o $(SRCS:%.cpp=%.o)
	$(CC) $(CC_FLAG) -o $@ $^

tests/xeforth: main/main.o $(SRCS:%.cpp=%.o) $(ESPS:%.cpp=%.o)
	$(CC) $(CC_FLAG) -o $@ $^

debug: tests/eforth
	/bin/valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes $^

clean:
	rm main/*.o main/esp32/*.o $(FLST)


