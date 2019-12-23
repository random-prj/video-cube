#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include "videocube.h"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <iostream>

using namespace std;

bool stop = false;
bool frame_ready = false;
mutex update_mtx;
condition_variable update_cv;
thread loop_thread;

GstElement* Convertor = nullptr;
GstElement* Appsink = nullptr;
GstElement* Audiobin = nullptr;

int VideoWidth=0;
int VideoHeight=0;
void* dmabuf_ptr = nullptr;

void on_new_pad_added(GstElement *decodebin, GstPad *pad, void* param )
{
    GstCaps *caps = gst_pad_query_caps (pad, NULL);
    GstStructure *str = gst_caps_get_structure (caps, 0);
    GstPad *link_sink = nullptr;
    //g_warning ("NEW PAD:: %s", gst_structure_get_name (str));
    if (g_strrstr (gst_structure_get_name (str), "video"))
    {
	if( Convertor )
	{
	    link_sink = gst_element_get_static_pad( Convertor, "sink" );
	    //std::cout << "appsink_ pad for link "<< (void*)link_sink << "\n";
	    if (GST_PAD_IS_LINKED (link_sink))
	    {
		//std::cout << "already linked\n";
		g_object_unref (link_sink);
		return;
	    }
	    GstPadLinkReturn r = gst_pad_link( pad, link_sink );
	    gst_object_unref( link_sink );
	    //std::cout << "video pad linked " << r << "\n";
	}
    }
    if( g_strrstr( gst_structure_get_name(str), "audio") )
    {
	if( Audiobin )
	{
		link_sink = gst_element_get_static_pad( Audiobin, "sink" );
		if (GST_PAD_IS_LINKED (link_sink)) {
			g_object_unref (link_sink);
			return;
		}
		GstPadLinkReturn r = gst_pad_link (pad, link_sink);
		gst_object_unref (link_sink);
		//std::cout << "audio pad linked " << r << "\n";
	}
    }
}

void on_eos( GstAppSink *Appsink, gpointer pParam )
{
    stop = true;
    update_cv.notify_one();
    cout << "EOS\n";
}

GstFlowReturn on_new_preroll( GstAppSink *Appsink, gpointer pParam )
{
    cout << "ON PREROLL\n";
    GstFlowReturn ret = GST_FLOW_OK;
    return ret;
}

GstFlowReturn on_new_sample( GstAppSink *pAppsink, gpointer pParam )
{
	GstFlowReturn ret = GST_FLOW_OK;
	GstSample *Sample = gst_app_sink_pull_sample(pAppsink);
	if( Sample )
	{
		if( VideoWidth==0 || VideoHeight==0 )
		{
			GstCaps* caps = gst_sample_get_caps( Sample );
			GstStructure* structure = gst_caps_get_structure (caps, 0);
			gst_structure_get_int (structure, "width", &VideoWidth);
			gst_structure_get_int (structure, "height", &VideoHeight);
			cout << "Stream Resolution " << VideoWidth << " " << VideoHeight << "\n";
		}

		GstBuffer *Buffer = gst_sample_get_buffer( Sample );
		if( Buffer )
		{
			GstMapInfo MapInfo;
			memset(&MapInfo, 0, sizeof(MapInfo));
			gboolean Mapped = gst_buffer_map( Buffer, &MapInfo, GST_MAP_READ );
			if( Mapped )
			{
				if( dmabuf_ptr )
					memcpy( dmabuf_ptr, MapInfo.data, MapInfo.size );
				gst_buffer_unmap( Buffer, &MapInfo);
				frame_ready = true;
				update_cv.notify_one();
			}
		}
		gst_sample_unref( Sample );
	}
	return ret;
}

/*
void main_loop_thread( void* param )
{
	GMainLoop* ml = (GMainLoop*)param;
	cout << "Main Loop Thread " << param << "\n";
	g_main_loop_run( ml );
	cout << "Main Loop Thread Stop\n";
}
*/

int main (int argc, char *argv[])
{
	if( argc<2)
	{
		cout << "Need video file name param\n";
		return -1;
	}
	cout << "Video file name: " << (char*)argv[1] << "\n";

	gst_init( 0, nullptr );
	GMainLoop* MainLoop = g_main_loop_new( nullptr, FALSE );
	cout << "Main Loop created " << (void*) MainLoop << "\n";

	GstPipeline* Pipeline = (GstPipeline *)gst_pipeline_new( "my-pipeline" );

	GstElement* Filesrc = gst_element_factory_make( "filesrc", nullptr );
	g_object_set( Filesrc, "location", argv[1], nullptr );

	GstElement* Decodebin = gst_element_factory_make( "decodebin", "my-decodebin" );

	GstAppSinkCallbacks AppSinkCb;
	AppSinkCb.eos = on_eos;
	AppSinkCb.new_preroll = on_new_preroll;
	AppSinkCb.new_sample = on_new_sample;

	Appsink = gst_element_factory_make("appsink", "vPlayerSink");

	gst_app_sink_set_callbacks( (GstAppSink*)Appsink, &AppSinkCb, nullptr, NULL );
	gst_app_sink_set_drop( (GstAppSink*)Appsink, true );
	gst_app_sink_set_max_buffers((GstAppSink*)Appsink, 2);
	g_object_set( Appsink, "wait-on-eos", FALSE, NULL);

	Convertor = gst_element_factory_make("videoconvert", "my-convertor");

	GstCaps *Caps = gst_caps_new_simple( "video/x-raw", "format", G_TYPE_STRING, "RGBA", nullptr );
	gst_app_sink_set_caps( (GstAppSink*)Appsink, Caps );
	gst_caps_unref( Caps );

	gst_bin_add( GST_BIN(Pipeline), Filesrc );
	gst_bin_add( GST_BIN(Pipeline), Decodebin );
	gst_bin_add( GST_BIN(Pipeline), Convertor );
	gst_bin_add( GST_BIN(Pipeline), Appsink );

	gst_element_link_many( Filesrc , Decodebin, nullptr );
	gst_element_link_many( Convertor, Appsink, nullptr );
	g_signal_connect( Decodebin, "pad-added", G_CALLBACK (on_new_pad_added), nullptr );

	Audiobin = gst_bin_new ( "audiobin" );
	GstElement* Audioconvert = gst_element_factory_make( "audioconvert", "my-audioconvert" );
	GstPad* Audiopad = gst_element_get_static_pad ( Audioconvert, "sink" );
	GstElement* Audiovolume = gst_element_factory_make ("volume", "my-volume" );
	g_object_set( G_OBJECT( Audiovolume ),"volume",1.0, nullptr );
	GstElement* Alsasink = gst_element_factory_make ( "alsasink", "my-alsasink" );
	gst_bin_add_many( GST_BIN(Audiobin), Audioconvert, Audiovolume, Alsasink, nullptr );
	gst_element_link_many( Audioconvert, Audiovolume, Alsasink, nullptr );
	gst_element_add_pad( Audiobin, gst_ghost_pad_new("sink", Audiopad) );
	gst_object_unref( Audiopad );
	gst_bin_add( GST_BIN(Pipeline), Audiobin );

	int width = 1280;
	int height = 768;
	ESContext esContext;
	UserData  userData;
	memset( &esContext, 0, sizeof( ESContext) );
	esContext.userData = &userData;
	esContext.width = width;
	esContext.height = height;

	if ( !WinCreate ( &esContext, "VideoCube Demo") )
	{
		return -1;
	}

	EGLint attribList[] =
	{
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_DEPTH_SIZE, 8,
		EGL_NONE
	};

	if ( !CreateEGLContext ( &esContext, attribList ) )
	{
		return -1;
	}

	if ( !InitEsContext ( &esContext ) )
		return -1;

	//loop_thread = std::thread( &main_loop_thread, MainLoop );

	gst_element_set_state( GST_ELEMENT(Pipeline), GST_STATE_PLAYING );

	struct timeval t1, t2;
	struct timezone tz;
	float deltatime;

	gettimeofday ( &t1, &tz );

	bool once = true;
	dmabuf_ptr = nullptr;
	stop = false;
	frame_ready = false;
	while( !stop )
	{
		std::unique_lock<std::mutex> lck( update_mtx );
		while( 1 )
		{
			update_cv.wait( lck );
			if( stop ) break;
			if( frame_ready ) break;
		}
		if( stop ) break;
		frame_ready = false;

		if( once && VideoWidth!=0 && VideoHeight!=0 )
		{
			once = false;
			dmabuf_ptr =  CreateVideoTexture( &esContext, VideoWidth, VideoHeight );
		}

		gettimeofday(&t2, &tz);
		deltatime = (float)(t2.tv_sec - t1.tv_sec + (t2.tv_usec - t1.tv_usec) * 1e-6);
		t1 = t2;

		Update( &esContext, deltatime );
		Draw( &esContext );
		eglSwapBuffers( esContext.eglDisplay, esContext.eglSurface );
		//usleep(20000);
	}

	return 0;
}
