/* conf.h.  Generated by configure.  */
/* conf.h.in.  Generated from configure.ac by autoheader.  */
// Modified by Chris to work for an Apple computer with OS 9 or above

/* Define to enable console */
/* #undef CONSOLE */

/* d2x major version */
#define D1XMAJOR "0"

/* d2x minor version */
#define D1XMINOR "56"

/* d2x micro version */
#define D1XMICRO "0"

/* Define if you want to build the editor */
/* #undef EDITOR */

/* Define if you want to build for mac datafiles */
//#define MACDATA 

/* Define to disable asserts, int3, etc. */
/* #undef NDEBUG */

/* Define if you want an assembler free build */
#define NO_ASM 

/* Define if you want an OpenGL build */
//#define OGL

/* Define for a "release" build */
/* #undef RELEASE */

/* Define this to be the shared game directory root */
#define SHAREPATH "."

/* Define if your processor needs data to be word-aligned */
/* #undef WORDS_NEED_ALIGNMENT */


        /* General defines */
#if defined(__APPLE__) && defined(__MACH__)
# define __unix__
/* Define if you want a network build */
# define NETWORK
# define USE_UDP
//# define USE_IPX

#define IPv6

/* Define to 1 if the system has the type `struct timespec'. */
#define HAVE_STRUCT_TIMESPEC 1

/* Define to 1 if the system has the type `struct timeval'. */
#define HAVE_STRUCT_TIMEVAL 1

#else // Mac OS 9
# ifndef __MWERKS__
#  define inline
# endif

#define OGL_RUNTIME_LOAD	// avoids corruption of OpenGL

/* Define to 1 if the system has the type `struct timespec'. */
#define HAVE_STRUCT_TIMESPEC 0

/* Define to 1 if the system has the type `struct timeval'. */
#define HAVE_STRUCT_TIMEVAL 0
#endif	// OS 9/X

#define SDL_INPUT 1
