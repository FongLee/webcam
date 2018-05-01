all: webcam_shower
#webcam_server webcam_shower
CC=g++
LD=ld
#CC=arm-linux-g++
#LD=arm-linux-ld
CXXFLAGS=	-c -g -O0 -fPIC   
#CXXFLAGS=	-c -g -O0 -fPIC -I/opt/webcam/ffmpeg/include -I/usr/include -L/opt/webcam/ffmpeg/lib
OBJS_SERVER=	capture.o vcompress.o sender.o server.o
OBJS_SHOWER= 	vshow.o recver.o shower.o
LIBS_SERVER=	-lavcodec -lswscale -lavutil -lx264 -lpthread -lz
LIBS_SHOWER=    -lavcodec -lswscale -lavutil  -lX11 -lXext

.cpp.o:
	$(CC) $(CXXFLAGS) $<

webcam_server: $(OBJS_SERVER)
	$(CC) -o $@ $^ -L/opt/webcam/ffmpeg/lib   $(LIBS_SERVER) 

webcam_shower: $(OBJS_SHOWER)
	$(CC) -o $@ $^    $(LIBS_SHOWER)

clean:
	rm -f *.o
	rm -f webcam_server
	rm -f webcam_shower