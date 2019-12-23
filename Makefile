CXFLAGS+=-std=c++11 -O3 -fPIC -Wall -Wdeprecated-declarations
LIBS+=-ldl -lpthread -lgstapp-1.0 -lgstbase-1.0 -lgstreamer-1.0 -lgobject-2.0 -lglib-2.0 -lGLESv2 -lEGL -lm -lX11 -ldrm -ldri2
#INCLUDES=-I/usr/include/gstreamer-1.0 -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include -I/usr/include/libdrm
INCLUDES=-I/usr/include/gstreamer-1.0 -I/usr/include/glib-2.0 -I/usr/lib/arm-linux-gnueabihf/glib-2.0/include -I/usr/include/libdrm
LDFLAGS+=-L/usr/lib/x86_64-linux-gnu

player: player.cpp videocube.cpp
	$(CXX) $(CXFLAGS) $(LDFLAGS) $(INCLUDES) player.cpp videocube.cpp -o player $(LIBS) 

clean:
	rm -f player


