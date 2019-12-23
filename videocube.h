
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#define make_fourcc_code(a, b, c, d) ((__u32)(a) | ((__u32)(b) << 8) | ((__u32)(c) << 16) | ((__u32)(d) << 24))
#define DRM_FORMAT_ABGR8888	make_fourcc_code('A', 'B', '2', '4') /* [31:0] A:B:G:R 8:8:8:8 little endian */
#define DRM_FORMAT_RGBA8888	make_fourcc_code('R', 'A', '2', '4') /* [31:0] R:G:B:A 8:8:8:8 little endian */
#define DRM_FORMAT_BGRA8888	make_fourcc_code('B', 'A', '2', '4') /* [31:0] B:G:R:A 8:8:8:8 little endian */

typedef struct
{
	GLfloat	m[4][4];
}ESMatrix;

typedef struct
{
	GLuint	texture;
	GLuint	textureV;
	bool textureV_ready;
	GLuint	programObject;	// Handle to a program object
	GLint	positionLoc;	// Attribute locations
	GLint	texCoordLoc;
	GLint	samplerLoc;
	GLint	mvpLoc;		// Uniform locations
	GLfloat	*vertices;	// Vertex data
	GLfloat	angle;		// Rotation angle
	ESMatrix mvpMatrix;	// MVP matrix
	GLuint	*indices;
	int	numIndices;
} UserData;

typedef struct _escontext
{
	void*	userData;
	GLint	width;		// Window width
	GLint	height;		// Window height
	EGLNativeWindowType  hWnd; // Window handle
	EGLDisplay eglDisplay;	// EGL display
	EGLContext eglContext;	// EGL context
	EGLSurface eglSurface;	// EGL surface
	EGLImageKHR ImageKHR;
	int DmaFd;
	void* Plane;
}ESContext;

int InitEsContext ( ESContext *esContext );
void Update ( ESContext *esContext, float deltaTime );
void Draw ( ESContext *esContext );
EGLBoolean WinCreate(ESContext *esContext, const char *title);
EGLBoolean CreateEGLContext ( ESContext* esContext, EGLint attribList[] );
void* CreateVideoTexture( ESContext* esContext, int Width, int Height );

