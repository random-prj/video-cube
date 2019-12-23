#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <math.h>

#include <sys/ioctl.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <xf86drm.h>
#include <sys/mman.h>
#include <linux/dma-buf.h>
//if system does not have libdri2 then possible to build from https://github.com/robclark/libdri2
extern "C"
{
#include <X11/extensions/dri2.h>
}

#include <iostream>
#include "videocube.h"

using namespace std;

Display *x_display = NULL;

void checkGlError( unsigned int  LineNumber )
{
    EGLint error = eglGetError();
    if (error != EGL_SUCCESS)
    {
        printf("eglGetError: %04X at %d\n",error, LineNumber );
    }
}

char* loadBMP ( char *fileName, int *pwidth, int *pheight )
{
    int i;
    char* buffer = NULL;
    int imagesize = 0;
    FILE* f = fopen(fileName, "rb");

    if(f == NULL) return NULL;

    unsigned char info[54];
    fread(info, sizeof(unsigned char), 54, f); // read the 54-byte header

    // extract image height and width from header
    int width = *(int*)&info[18];
    int height = *(int*)&info[22];
    imagesize = width*height *4;
    buffer = (char*)malloc(imagesize);
    if (buffer == NULL)
    {
        fclose(f);
        return 0;
    }
    *pwidth = width;
    *pheight = height;
    printf("Bitmap %s %d %d %p\n",fileName,width, height, buffer);

    int row_padded = (width*3 + 3) & (~3);
    char* data = (char*)malloc(row_padded);

    for(int i = 0; i < height; i++)
    {
        char* dst = buffer+(height-i-1)*width*4;
        fread(data, sizeof(unsigned char), row_padded, f);
        for(int j = 0; j < width; j++ )
        {
            // Convert (B, G, R) to (R, G, B, A)
            dst[j*4+2] = data[j*3+0];
            dst[j*4+1] = data[j*3+1];
            dst[j*4+0] = data[j*3+2];
            dst[j*4+3] = 0xFF;
        }
    }
    free(data);
    fclose(f);
    return buffer;
}

PFNEGLCREATEIMAGEKHRPROC  funcEglCreateImageKHR = nullptr;
PFNEGLDESTROYIMAGEKHRPROC funcEglDestroyImageKHR = nullptr;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC funcGlEGLImageTargetTexture2DOES = nullptr;
int DriCardFd = 0;

int OpenDrm()
{
	int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if( fd < 0 )
	{
		cout << "cannot open /dev/dri/card0\n";
		return -1;
	}

	uint64_t hasDumb = 0;
	if( drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &hasDumb) < 0 )
	{
		close( fd );
		//cout << "/dev/dri/card0 has no support for DUMB_BUFFER\n";

		//maybe Raspberry Pi4 or other platform
		fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
		if( fd < 0 )
		{
			cout << "cannot open /dev/dri/card1\n";
			return -1;
		}

		hasDumb = 0;
		if( drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &hasDumb) < 0 )
		{
			close( fd );
			cout << "/dri/card1 has no support for DUMB_BUFFER\n";
			return -1;
		}
	}

	if( !hasDumb )
	{
		close( fd );
		cout << "no support for DUMB_BUFFER\n";
		return -1;
	}

	//Get DRM authorization
	drm_magic_t magic;
	if( drmGetMagic(fd, &magic) )
	{
		cout << "no DRM magic\n";
		close( fd );
		return -1;
	}

	Window root = DefaultRootWindow( x_display );
	if( !DRI2Authenticate( x_display, root, magic ) )
	{
		close( fd );
		cout << "Failed DRI2Authenticate\n";
		return -1;
	}
	cout << "DRM fd "<< fd <<"\n";
	return fd;
}

void CloseDrm()
{
	if( DriCardFd>0 )
	{
		close( DriCardFd>0 );
		DriCardFd = 0;
	}
}

bool CheckDrm( ESContext* esContext )
{
    DriCardFd = 0;
    char* EglExtString = (char*)eglQueryString( esContext->eglDisplay, EGL_EXTENSIONS );
    if( strstr( EglExtString, "EGL_EXT_image_dma_buf_import") )
    {
	cout << "DMA_BUF feature must be supported!!!\n";

	DriCardFd = OpenDrm();
	if( DriCardFd>0 )
	{
		funcEglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC) eglGetProcAddress("eglCreateImageKHR");
		funcEglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC) eglGetProcAddress("eglDestroyImageKHR");
		funcGlEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) eglGetProcAddress("glEGLImageTargetTexture2DOES");
		if( funcEglCreateImageKHR && funcEglDestroyImageKHR && funcGlEGLImageTargetTexture2DOES )
		{
			cout << "DMA_BUF feature supported!!!\n";
		}
		else
		{
			CloseDrm();
		}
	}
    }
return DriCardFd != 0;
}

int CreateDmaBuf( int Width, int Height, int* DmaFd, void** Plane )
{
	int dmaFd = *DmaFd = 0;
	void* pplane = *Plane = nullptr;

	// Create dumb buffer
	drm_mode_create_dumb buffer = { 0 };
	buffer.width = Width;
	buffer.height = Height;
	buffer.handle = 0;
	buffer.bpp = 32; //Bits per pixel
	buffer.flags = 0;

	int ret = drmIoctl( DriCardFd, DRM_IOCTL_MODE_CREATE_DUMB, &buffer);
	cout << "DRM_IOCTL_MODE_CREATE_DUMB " << buffer.handle << " " << ret << "\n";
	if (ret < 0)
	{
		cout << "Error cannot DRM_IOCTL_MODE_CREATE_DUMB\n";
		return -1;
	}

	// Get the dmabuf for the buffer
	drm_prime_handle prime = { 0 };
	prime.handle = buffer.handle;
	prime.flags = DRM_CLOEXEC | DRM_RDWR;

	ret = drmIoctl( DriCardFd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);
	if (ret < 0)
	{
		cout << "Error cannot DRM_IOCTL_PRIME_HANDLE_TO_FD " << errno << " " << ret <<"\n";
		return -1;
	}
	dmaFd = prime.fd;
/*
	struct drm_mode_map_dumb mode_map;
	mode_map.handle = buffer.handle;
	mode_map.pad = 0;
	mode_map.offset = 0;
	ret = drmIoctl( DriCardFd, DRM_IOCTL_MODE_MAP_DUMB, &mode_map);
	if (ret < 0)
	{
		cout << "Error cannot DRM_IOCTL_MODE_MAP " << errno << " " << ret <<"\n";
		return -1;
	}
*/
	// Map the buffer to userspace
	int Bpp = 32;
	pplane = mmap(NULL, Width*Height*Bpp/8, PROT_READ | PROT_WRITE, MAP_SHARED, dmaFd, 0);
	if( pplane == MAP_FAILED )
	{
		cout << "Error cannot mmap\n";
		return -1;
	}

	//return valid values
	*DmaFd = dmaFd;
	*Plane = pplane;
	cout << "DMABUF created "<< dmaFd << " " << (void*)Plane <<"\n";
	return 0;
}

int CreateDmaBufferImage( ESContext* esContext, int Width, int Height, int* DmaFd, void** Plane, EGLImageKHR* Image )
{
	int dmaFd = 0;
	void* planePtr = nullptr;

	int Bpp = 32;
	int ret0 = CreateDmaBuf( Width, Height, &dmaFd, &planePtr );
	if( ret0<0 )
		return -1;

	EGLint img_attrs[] = {
		EGL_WIDTH, Width,
		EGL_HEIGHT, Height,
		EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ABGR8888,
		EGL_DMA_BUF_PLANE0_FD_EXT, dmaFd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, Width * Bpp / 8,
		EGL_NONE
	};

	EGLImageKHR image = funcEglCreateImageKHR( esContext->eglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, 0, &img_attrs[0] );

	*Plane = planePtr;
	*DmaFd  = dmaFd;
	*Image = image;
	cout << "DMA_BUF pointer " << (void*)planePtr << "\n";
	cout << "DMA_BUF fd " << (int)dmaFd << "\n";
	cout << "EGLImageKHR " << image << "\n";
	return 0;
}

GLuint loadTexture ( char *fileName )
{
    int width, height;
    char *buffer = loadBMP ( fileName, &width, &height );
    GLuint texId;

    if ( buffer == NULL )
    {
        printf( "Error loading (%s) image.\n", fileName );
        return (GLuint)-1;
    }

    glGenTextures ( 1, &texId );
    glBindTexture ( GL_TEXTURE_2D, texId );

    glTexImage2D ( GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, buffer );
    glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
    glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
    glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
    glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

    free ( buffer );
    return texId;
}

GLuint CreateSimpleTexture2D( )
{
    GLuint textureId;
    int width  = 256;
    int height = 256;
    unsigned char* pixels = (unsigned char*)malloc( width*height*4);
    for( int y=0; y<height; y++ )
    {
        for( int x=0; x<width; x++ )
        {
            int idx = y*width*4;
            pixels[idx+x*4+0]=x;
            pixels[idx+x*4+1]=0; //(x*2)&0xFF;
            pixels[idx+x*4+2]=0; //(x*4)&0xFF;
            pixels[idx+x*4+3]=0xFF;
        }
    }
    glPixelStorei ( GL_UNPACK_ALIGNMENT, 1 );
    checkGlError( __LINE__ );
    glGenTextures ( 1, &textureId );
    checkGlError( __LINE__ );
    glBindTexture ( GL_TEXTURE_2D, textureId );
    checkGlError( __LINE__ );
    glTexImage2D ( GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
    checkGlError( __LINE__ );
    glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
    checkGlError( __LINE__ );
    glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
    checkGlError( __LINE__ );
    glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
    checkGlError( __LINE__ );
    glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
    checkGlError( __LINE__ );
    free( pixels );
    return textureId;
}

GLuint esLoadShader ( GLenum type, const char *shaderSrc )
{
    GLuint shader;
    GLint compiled;

    // Create the shader object
    shader = glCreateShader ( type );

    if ( shader == 0 )
        return 0;

    // Load the shader source
    glShaderSource ( shader, 1, &shaderSrc, NULL );

    // Compile the shader
    glCompileShader ( shader );

    // Check the compile status
    glGetShaderiv ( shader, GL_COMPILE_STATUS, &compiled );

    if ( !compiled )
    {
        GLint infoLen = 0;

        glGetShaderiv ( shader, GL_INFO_LOG_LENGTH, &infoLen );
        if ( infoLen > 1 )
        {
            char* infoLog = (char*)malloc (sizeof(char) * infoLen );
            glGetShaderInfoLog ( shader, infoLen, NULL, infoLog );
            printf( "Error compiling shader:\n%s\n", infoLog );
            free ( infoLog );
        }

        glDeleteShader ( shader );
        return 0;
    }
    return shader;
}

GLuint esLoadProgram ( const char *vertShaderSrc, const char *fragShaderSrc )
{
    GLuint vertexShader;
    GLuint fragmentShader;
    GLuint programObject;
    GLint linked;

    // Load the vertex/fragment shaders
    vertexShader = esLoadShader ( GL_VERTEX_SHADER, vertShaderSrc );
    if ( vertexShader == 0 )
        return 0;

    fragmentShader = esLoadShader ( GL_FRAGMENT_SHADER, fragShaderSrc );
    if ( fragmentShader == 0 )
    {
        glDeleteShader( vertexShader );
        return 0;
    }

    // Create the program object
    programObject = glCreateProgram ( );

    if ( programObject == 0 )
        return 0;

    glAttachShader ( programObject, vertexShader );
    glAttachShader ( programObject, fragmentShader );

    // Link the program
    glLinkProgram ( programObject );

    // Check the link status
    glGetProgramiv ( programObject, GL_LINK_STATUS, &linked );

    if ( !linked )
    {
        GLint infoLen = 0;

        glGetProgramiv ( programObject, GL_INFO_LOG_LENGTH, &infoLen );

        if ( infoLen > 1 )
        {
            char* infoLog = (char*)malloc (sizeof(char) * infoLen );

            glGetProgramInfoLog ( programObject, infoLen, NULL, infoLog );
            printf( "Error linking program:\n%s\n", infoLog );

            free ( infoLog );
        }

        glDeleteProgram ( programObject );
        return 0;
    }

    // Free up no longer needed shader resources
    glDeleteShader ( vertexShader );
    glDeleteShader ( fragmentShader );

    return programObject;
}

EGLBoolean CreateEGLContext ( ESContext* esContext, EGLint attribList[] )
{
    EGLint numConfigs;
    EGLint majorVersion;
    EGLint minorVersion;
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    EGLConfig config;
    EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE, EGL_NONE };

    // Get Display
    display = eglGetDisplay((EGLNativeDisplayType)x_display);
    if ( display == EGL_NO_DISPLAY )
    {
        return EGL_FALSE;
    }

    // Initialize EGL
    if ( !eglInitialize(display, &majorVersion, &minorVersion) )
    {
        return EGL_FALSE;
    }

    // Get configs
    if ( !eglGetConfigs(display, NULL, 0, &numConfigs) )
    {
        return EGL_FALSE;
    }

    // Choose config
    if ( !eglChooseConfig(display, attribList, &config, 1, &numConfigs) )
    {
        return EGL_FALSE;
    }

    // Create a surface
    surface = eglCreateWindowSurface(display, config, (EGLNativeWindowType)esContext->hWnd, NULL);
    if ( surface == EGL_NO_SURFACE )
    {
        return EGL_FALSE;
    }

    // Create a GL context
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttribs );
    if ( context == EGL_NO_CONTEXT )
    {
        return EGL_FALSE;
    }

    // Make the context current
    if ( !eglMakeCurrent(display, surface, surface, context) )
    {
        return EGL_FALSE;
    }

    esContext->eglDisplay = display;
    esContext->eglSurface = surface;
    esContext->eglContext = context;
    return EGL_TRUE;
}


///
//  WinCreate()
//
//      This function initialized the native X11 display and window for EGL
//
EGLBoolean WinCreate(ESContext *esContext, const char *title)
{
    Window root;
    XSetWindowAttributes swa;
    XSetWindowAttributes  xattr;
    Atom wm_state;
    XWMHints hints;
    XEvent xev;
    EGLConfig ecfg;
    EGLint num_config;
    Window win;

    /*
     * X11 native display initialization
     */

    x_display = XOpenDisplay(NULL);
    if ( x_display == NULL )
    {
        return EGL_FALSE;
    }

    root = DefaultRootWindow(x_display);

    swa.event_mask  =  ExposureMask | PointerMotionMask | KeyPressMask;
    win = XCreateWindow(
              x_display, root,
              0, 0, esContext->width, esContext->height, 0,
              CopyFromParent, InputOutput,
              CopyFromParent, CWEventMask,
              &swa );

    xattr.override_redirect = 0;
    XChangeWindowAttributes ( x_display, win, CWOverrideRedirect, &xattr );

    hints.input = 1;
    hints.flags = InputHint;
    XSetWMHints(x_display, win, &hints);

    // make the window visible on the screen
    XMapWindow (x_display, win);
    XStoreName (x_display, win, title);

    // get identifiers for the provided atom name strings
    wm_state = XInternAtom (x_display, "_NET_WM_STATE", 0);

    memset ( &xev, 0, sizeof(xev) );
    xev.type                 = ClientMessage;
    xev.xclient.window       = win;
    xev.xclient.message_type = wm_state;
    xev.xclient.format       = 32;
    xev.xclient.data.l[0]    = 1;
    xev.xclient.data.l[1]    = 0;
    XSendEvent (
        x_display,
        DefaultRootWindow ( x_display ),
        0,
        SubstructureNotifyMask,
        &xev );

    esContext->hWnd = (EGLNativeWindowType) win;
    return EGL_TRUE;
}

void esMatrixMultiply(ESMatrix *result, ESMatrix *srcA, ESMatrix *srcB)
{
    ESMatrix    tmp;
    int         i;

    for (i=0; i<4; i++)
    {
        tmp.m[i][0] =	(srcA->m[i][0] * srcB->m[0][0]) +
                        (srcA->m[i][1] * srcB->m[1][0]) +
                        (srcA->m[i][2] * srcB->m[2][0]) +
                        (srcA->m[i][3] * srcB->m[3][0]) ;

        tmp.m[i][1] =	(srcA->m[i][0] * srcB->m[0][1]) +
                        (srcA->m[i][1] * srcB->m[1][1]) +
                        (srcA->m[i][2] * srcB->m[2][1]) +
                        (srcA->m[i][3] * srcB->m[3][1]) ;

        tmp.m[i][2] =	(srcA->m[i][0] * srcB->m[0][2]) +
                        (srcA->m[i][1] * srcB->m[1][2]) +
                        (srcA->m[i][2] * srcB->m[2][2]) +
                        (srcA->m[i][3] * srcB->m[3][2]) ;

        tmp.m[i][3] =	(srcA->m[i][0] * srcB->m[0][3]) +
                        (srcA->m[i][1] * srcB->m[1][3]) +
                        (srcA->m[i][2] * srcB->m[2][3]) +
                        (srcA->m[i][3] * srcB->m[3][3]) ;
    }
    memcpy(result, &tmp, sizeof(ESMatrix));
}


void esFrustum(ESMatrix *result, float left, float right, float bottom, float top, float nearZ, float farZ)
{
    float       deltaX = right - left;
    float       deltaY = top - bottom;
    float       deltaZ = farZ - nearZ;
    ESMatrix    frust;

    if ( (nearZ <= 0.0f) || (farZ <= 0.0f) ||
            (deltaX <= 0.0f) || (deltaY <= 0.0f) || (deltaZ <= 0.0f) )
        return;

    frust.m[0][0] = 2.0f * nearZ / deltaX;
    frust.m[0][1] = frust.m[0][2] = frust.m[0][3] = 0.0f;

    frust.m[1][1] = 2.0f * nearZ / deltaY;
    frust.m[1][0] = frust.m[1][2] = frust.m[1][3] = 0.0f;

    frust.m[2][0] = (right + left) / deltaX;
    frust.m[2][1] = (top + bottom) / deltaY;
    frust.m[2][2] = -(nearZ + farZ) / deltaZ;
    frust.m[2][3] = -1.0f;

    frust.m[3][2] = -2.0f * nearZ * farZ / deltaZ;
    frust.m[3][0] = frust.m[3][1] = frust.m[3][3] = 0.0f;

    esMatrixMultiply(result, &frust, result);
}


void esPerspective(ESMatrix *result, float fovy, float aspect, float nearZ, float farZ)
{
    GLfloat frustumW, frustumH;

    frustumH = tanf( fovy / 360.0f * M_PI ) * nearZ;
    frustumW = frustumH * aspect;

    esFrustum( result, -frustumW, frustumW, -frustumH, frustumH, nearZ, farZ );
}

#define S1 0.52f
#define S2 0.32f
#define pA - S1, - S2, - S1,
#define pB - S1,   S2, - S1,
#define pC   S1,   S2, - S1,
#define pD   S1, - S2, - S1,
#define pE - S1, - S2,   S1,
#define pF - S1,   S2,   S1,
#define pG   S1,   S2,   S1,
#define pH   S1, - S2,   S1,

GLfloat g_cubeVerts[] =
{
    pA pB pC pD
    pE pF pG pH
    pA pB pF pE
    pD pC pG pH
};

GLuint g_cubeIndices[] =
{
    0, 1, 2, //front plane
    2, 0, 3,
    4, 5, 6, //back plane
    6, 7, 4,
    8, 9, 10, //left plane
    10,8, 11,
    12,13,14, //right plane
    14,12,15,

    1, 5, 6, //top plane
    6, 2, 1,
    0, 4, 7, //bottom plane
    7, 0, 3
};

//
// Initialize the shader and program object
//
int InitEsContext ( ESContext *esContext )
{
    esContext->userData = malloc(sizeof(UserData));
    memset(esContext->userData,0,sizeof(UserData));

    UserData *userData = (UserData*)esContext->userData;
    GLbyte vShaderStr[] =
        "uniform mat4 u_mvpMatrix;                   \n"
        "attribute vec4 a_position;                  \n"
        "attribute vec2 a_texCoord;                  \n"
        "varying vec2 v_texCoord;                    \n"
        "void main()                                 \n"
        "{                                           \n"
        "   gl_Position = u_mvpMatrix * a_position;  \n"
        "   v_texCoord = a_texCoord;                 \n"
        "}                                           \n";

    GLbyte fShaderStr[] =
        "precision mediump float;                            \n"
        "varying vec2 v_texCoord;                            \n"
        "uniform sampler2D s_texture;                        \n"
        "void main()                                         \n"
        "{                                                   \n"
        "  gl_FragColor = texture2D( s_texture, v_texCoord );\n"
        "}                                                   \n";

    // Load the shaders and get a linked program object
    userData->programObject = esLoadProgram ( (const char*)vShaderStr, (const char*)fShaderStr );
    checkGlError( __LINE__ );

    // Get the attribute locations
    userData->positionLoc = glGetAttribLocation ( userData->programObject, "a_position" );
    checkGlError( __LINE__ );
    userData->texCoordLoc = glGetAttribLocation ( userData->programObject, "a_texCoord" );
    checkGlError( __LINE__ );

    // Get the uniform locations
    userData->mvpLoc = glGetUniformLocation( userData->programObject, "u_mvpMatrix" );
    checkGlError( __LINE__ );
    userData->samplerLoc = glGetUniformLocation ( userData->programObject, "s_texture" );
    checkGlError( __LINE__ );

    userData->vertices = g_cubeVerts;
    userData->indices = g_cubeIndices;

    // Starting rotation angle for the cube
    userData->angle = 45.0f;

    glClearColor ( 0.2f, 0.2f, 0.2f, 0.0f );

    userData->texture = loadTexture("vframe.bmp"); //CreateSimpleTexture2D( );

    if( CheckDrm( esContext) )
    {
    }

    return GL_TRUE;
}

//return pointer to DMABUF memory
void* CreateVideoTexture( ESContext* esContext, int Width, int Height )
{
	CreateDmaBufferImage( esContext, Width, Height, &esContext->DmaFd, &esContext->Plane, &esContext->ImageKHR );
	GLuint texId;
	glGenTextures ( 1, &texId );
	glBindTexture ( GL_TEXTURE_2D, texId );
	glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	funcGlEGLImageTargetTexture2DOES(GL_TEXTURE_2D, esContext->ImageKHR );
	checkGlError( __LINE__ );
	UserData *userData = (UserData*)esContext->userData;
	userData->textureV = texId;
	userData->textureV_ready = true;
	return esContext->Plane;
}

void esTranslate(ESMatrix *result, GLfloat tx, GLfloat ty, GLfloat tz)
{
    result->m[3][0] += (result->m[0][0] * tx + result->m[1][0] * ty + result->m[2][0] * tz);
    result->m[3][1] += (result->m[0][1] * tx + result->m[1][1] * ty + result->m[2][1] * tz);
    result->m[3][2] += (result->m[0][2] * tx + result->m[1][2] * ty + result->m[2][2] * tz);
    result->m[3][3] += (result->m[0][3] * tx + result->m[1][3] * ty + result->m[2][3] * tz);
}

void esRotate(ESMatrix *result, GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
    GLfloat sinAngle, cosAngle;
    GLfloat mag = sqrtf(x * x + y * y + z * z);

    sinAngle = sinf ( angle * M_PI / 180.0f );
    cosAngle = cosf ( angle * M_PI / 180.0f );
    if ( mag > 0.0f )
    {
        GLfloat xx, yy, zz, xy, yz, zx, xs, ys, zs;
        GLfloat oneMinusCos;
        ESMatrix rotMat;
        x /= mag;
        y /= mag;
        z /= mag;

        xx = x * x;
        yy = y * y;
        zz = z * z;
        xy = x * y;
        yz = y * z;
        zx = z * x;
        xs = x * sinAngle;
        ys = y * sinAngle;
        zs = z * sinAngle;
        oneMinusCos = 1.0f - cosAngle;

        rotMat.m[0][0] = (oneMinusCos * xx) + cosAngle;
        rotMat.m[0][1] = (oneMinusCos * xy) - zs;
        rotMat.m[0][2] = (oneMinusCos * zx) + ys;
        rotMat.m[0][3] = 0.0F;

        rotMat.m[1][0] = (oneMinusCos * xy) + zs;
        rotMat.m[1][1] = (oneMinusCos * yy) + cosAngle;
        rotMat.m[1][2] = (oneMinusCos * yz) - xs;
        rotMat.m[1][3] = 0.0F;

        rotMat.m[2][0] = (oneMinusCos * zx) - ys;
        rotMat.m[2][1] = (oneMinusCos * yz) + xs;
        rotMat.m[2][2] = (oneMinusCos * zz) + cosAngle;
        rotMat.m[2][3] = 0.0F;

        rotMat.m[3][0] = 0.0F;
        rotMat.m[3][1] = 0.0F;
        rotMat.m[3][2] = 0.0F;
        rotMat.m[3][3] = 1.0F;

        esMatrixMultiply( result, &rotMat, result );
    }
}

void esMatrixLoadIdentity(ESMatrix *result)
{
    memset(result, 0x0, sizeof(ESMatrix));
    result->m[0][0] = 1.0f;
    result->m[1][1] = 1.0f;
    result->m[2][2] = 1.0f;
    result->m[3][3] = 1.0f;
}

///
// Update MVP matrix based on time
//
void Update ( ESContext *esContext, float deltaTime )
{
    UserData *userData = (UserData*) esContext->userData;
    ESMatrix perspective;
    ESMatrix modelview;
    float    aspect;

    // Compute a rotation angle based on time to rotate the cube
    userData->angle += 1; //( deltaTime * 40.0f );
    if( userData->angle >= 360.0f )
        userData->angle -= 360.0f;

    // Compute the window aspect ratio
    aspect = (GLfloat) esContext->width / (GLfloat) esContext->height;

    // Generate a perspective matrix with a 60 degree FOV
    esMatrixLoadIdentity( &perspective );
    esPerspective( &perspective, 60.0f, aspect, 1.0f, 20.0f );

    // Generate a model view matrix to rotate/translate the cube
    esMatrixLoadIdentity( &modelview );

    // Translate away from the viewer
    esTranslate( &modelview, 0.0, 0.0, -2.0 );

    // Rotate the cube
    esRotate( &modelview, userData->angle, 1.0, 0.0, 1.0 );

    // Compute the final MVP by multiplying the
    // modevleiw and perspective matrices together
    esMatrixMultiply( &userData->mvpMatrix, &modelview, &perspective );
}

///
// Draw a triangle using the shader pair created in Init()
//
void Draw ( ESContext *esContext )
{
    GLfloat vTexVertices[] =
    {
        0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f,
    };

    UserData *userData = (UserData*)esContext->userData;

    // Set the viewport
    glViewport ( 0, 0, esContext->width, esContext->height );
    checkGlError( __LINE__ );

    // Clear the color buffer
    glClear ( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
    checkGlError( __LINE__ );
    glEnable(GL_DEPTH_TEST);
    checkGlError( __LINE__ );
    glDepthFunc(GL_LESS);
    checkGlError( __LINE__ );

    // Use the program object
    glUseProgram( userData->programObject );

    // Load the vertex position
    glVertexAttribPointer( userData->positionLoc, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(GLfloat), userData->vertices );
    checkGlError( __LINE__ );
    glEnableVertexAttribArray( userData->positionLoc );
    checkGlError( __LINE__ );

    // Load the MVP matrix
    glUniformMatrix4fv( userData->mvpLoc, 1, GL_FALSE, (GLfloat*) &userData->mvpMatrix.m[0][0] );
    checkGlError( __LINE__ );

    // Load the texture coordinate
    glVertexAttribPointer ( userData->texCoordLoc, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), &vTexVertices[0] );
    checkGlError( __LINE__ );
    glEnableVertexAttribArray ( userData->texCoordLoc );
    checkGlError( __LINE__ );

    // Bind the texture
    glActiveTexture ( GL_TEXTURE0 );
    checkGlError( __LINE__ );
    if( userData->textureV_ready )
	glBindTexture ( GL_TEXTURE_2D, userData->textureV );
    else
	glBindTexture ( GL_TEXTURE_2D, userData->texture );

    checkGlError( __LINE__ );

    // Set the sampler texture unit to 0
    glUniform1i ( userData->samplerLoc, 0 );
    checkGlError( __LINE__ );

    glDrawElements ( GL_TRIANGLES, 24 /*userData->numIndices*/, GL_UNSIGNED_INT, userData->indices );
}

///
// Cleanup
//
void ShutDown ( ESContext *esContext )
{
    UserData *userData = (UserData*)esContext->userData;

    if ( userData->vertices != NULL )
    {
        free ( userData->vertices );
    }

    if ( userData->indices != NULL )
    {
        free ( userData->indices );
    }

    // Delete program object
    glDeleteProgram ( userData->programObject );

    free(userData);
}

