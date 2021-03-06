/*
 * Copyright © 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <cstdlib>
#include <cstring>
#include <string>
#include <sstream>
#include <vector>
#include <list>
#include <assert.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stddef.h>
#include "function_fetcher.hpp"
#include "gltypes.hpp"

#include "i965_batchbuffer_logger_app.h"
#include "i965_batchbuffer_logger_output.h"

/*
 * Environmental variables that control output:
 *  - I965_BLACKBOX_FILENAME provides filename prefix for output, defualt
 *                           value given by macro DEFAULT_FILENAME.
 *
 *  - I965_BLACKBOX_MAX_FILESIZE is number of bytes before a new file is
 *                               started in the log, default value is given
 *                               by macro DEFAULT_MAX_FILESIZE
 *
 *  - I965_BLACKBOX_MAX_FRAMES_PERFILE is number of frames before a new file
 *                                     is started in the log, default value
 *                                     is given by macro DEFAULT_MAX_FRAMES_PER_FILE
 *
 *  - I965_BLACKBOX_NUM_MOST_RECENT_KEEP if non-zero, gives the number of most recent
 *                                       execbuffer2 calls to keep as dedicated files
 *                                       (usually for the purpose of debugging a GPU
 *                                       hang). When the value is non-zero only these
 *                                       are kept instead of the log of the entire
 *                                       application.
 *
 * - I965_BLACKBOX_GL_LIB name of GL .so to use to load GL symbols;
 *                        if not set, use the value "libGL.so"
 *
 * - I965_BLACKBOX_GLES_LIB name of GLES .so to use to load GLES2/3 symbols;
 *                          if not set, use the value "libGLESv2.so"
 *
 * - I965_BLACKBOX_EGL_LIB name of EGL .so to use to load EGL symbols;
 *                         if not set, use the value "libEGL.so"
 *
 * Interception Notes:
 *  The methodology for interception of GL/GLES API calls
 *  is taken from apitrace (https://github.com/apitrace/apitrace).
 *
 *
 * 1. For each GL/GLES function glFoo, we 
 *  a. define the function glFoo, this makes the pre and post
 *     calls to i965_batchbuffer_logger_app
 *  b. define static functions _glFoo_init and _glFoo_do_nothing
 *  c. define a function pointer _glFoo initialized to _glFoo_init
 *  d. _glFoo_init uses fetch_function() to get the real GL function
 *
 * 2. fetch_function() relies on gl_dlsym() and gles_dlsym(); it uses
 *    the former by default but will change to using the latter if
 *    EGL usage is detected.
 * 
 * 3. There are 3 "high-level getters"
 *  a. gl_sym() for fetching GL functions first via glXGetProcAddress
 *     and glXGetProcAddressARB, (grabbed via the via dlsym_lib_style())
 *     and if they both fail then to use dlsym_lib_style()
 *  b. egl_sym() for fetching EGL functions first via eglGetProcAddress,
 *     (grabbed via the via dlsym_lib_style()) and if it 
 *     fails then to use dlsym_lib_style()
 *  c. gles_sym() for fetching GL functions first via eglGetProcAddress,
 *     (grabbed via the via dlsym_lib_style()) and if it 
 *     fails then to use dlsym_lib_style()
 *
 * 4. We have special implementations of
 *   a. glXGetProcAddress/glXGetProcAddressARB.
 *     i. glXSwapBuffers --> return our locally defined glXSwapBuffers
 *     ii. GL functions --> return our locall defined GL functions
 *     iii. fallback to whatever gl_dlsym() returns
 *   b. eglGetProcAddress
 *    i. eglSwapBuffers --> return our locally defined eglSwapBuffers
 *    ii. GL functions --> return our locall defined GL functions
 *    iii. try using egl_dlsym(), if non-null return that value
 *    iv. fallback to whatever egl_dlsym() returns
 *   c. dlsym
 *    i. if one of glXGetProcAddress, glXGetProcAddressARB,
 *       glXSwapBuffers, eglGetProcAddress, eglSwapBuffers,
 *       then use our locally defined one
 *    ii. GL functions --> return our locall defined GL functions
 *    iii. fallback to whatever real_dlsym() returns
 */

// default filename prefix
#define DEFAULT_FILENAME "i965_blackbox_log"

// default max file size of 16MB before starting new file
#define DEFAULT_MAX_FILESIZE (16 * 1024 * 1024)

// default max number of frames before starting new file
#define DEFAULT_MAX_FRAMES_PER_FILE 100

////////////////////////////////////
// Global vomit.
static struct i965_batchbuffer_logger_app *logger_app = NULL;
static struct i965_batchbuffer_logger_session logger_session = { .opaque = NULL };
static unsigned int most_recent_ioctl_max = 0;
static long max_filesize = 0;
static unsigned int numframes_per_file;
static unsigned int frame_count = 0;
static unsigned int api_count = 0;
static bool prefer_gl_sym = true;


namespace {

struct function_list
{
  const char *m_name;
  void *m_function;
};

template<typename T>
T
read_from_environment(const char *env, T default_value)
{
  const char *tmp;
  T return_value(default_value);
  
  tmp = std::getenv(env);
  if (tmp != nullptr) {
     std::string str(tmp);
     std::istringstream istr(tmp);
     istr >> return_value;
  }

  return return_value;
}
  
class Block
{
public:
  void
  set(const void *name, uint32_t name_length,
      const void *value, uint32_t value_length)
  {
    m_name.resize(name_length);
    if (name_length > 0) {
      std::memcpy(&m_name[0], name, name_length);
    }

    m_value.resize(value_length);
    if (value_length > 0) {
      std::memcpy(&m_value[0], value, value_length);
    }
  }

  const void*
  name(void) const
  {
    return m_name.empty() ?
      nullptr :
      &m_name[0];
  }

  uint32_t
  name_length(void) const
  {
    return m_name.size();
  }

  const void*
  value(void) const
  {
    return m_value.empty() ?
      nullptr :
      &m_value[0];
  }

  uint32_t
  value_length(void) const
  {
    return m_value.size();
  }
  
private:
  std::vector<uint8_t> m_name;
  std::vector<uint8_t> m_value;
};

class Session
{
public:  
  ~Session();

  static
  void
  write_fcn(void *pthis,
            enum i965_batchbuffer_logger_message_type_t tp,
            const void *name, uint32_t name_length,
            const void *value, uint32_t value_length);

  static
  void
  pre_execbuffer2_ioctl_fcn(void *pthis, unsigned int id);

  static
  void
  post_execbuffer2_ioctl_fcn(void *pthis, unsigned int id);
  
  static
  void
  close_fcn(void *pthis);

  static
  struct i965_batchbuffer_logger_session
  start_session(unsigned int most_recent_ioctl_max,
                struct i965_batchbuffer_logger_app *app,
                long max_filesize)
  {
    struct i965_batchbuffer_logger_session_params params;
    params.client_data = new Session(most_recent_ioctl_max, max_filesize);
    params.write = &Session::write_fcn;
    params.close = &Session::close_fcn;
    params.pre_execbuffer2_ioctl = &Session::pre_execbuffer2_ioctl_fcn;
    params.post_execbuffer2_ioctl = &Session::post_execbuffer2_ioctl_fcn;
    return app->begin_session(app, &params);
  }
  
private:
  Session(unsigned int most_recent_ioctl_max,
          long max_filesize);

void
  start_new_file(void);

  void
  close_file(void);

  void
  write_to_file(enum i965_batchbuffer_logger_message_type_t tp,
                const void *name, uint32_t name_length,
                const void *value, uint32_t value_length);

  unsigned int m_most_recent_ioctl_max;
  long m_max_filesize;
  unsigned int m_count;

  unsigned int m_most_recent_ioctl_file_cnt;
  std::list<std::string> m_most_recent_ioctl_files;
  
  /* Because we split a single session across many files,
   * we need to reset the block to zero on ending a file
   * and restore the block structure at the start of a
   * new file; m_block_stack holds the block structure.
   */
  std::vector<Block> m_block_stack;
  std::string m_prefix;
  std::string m_filename;
  std::FILE *m_file;
};

} //anonymous namespace

//////////////////////////////////////////
// Session methods
Session::
Session(unsigned int most_recent_ioctl_max,
        long max_filesize):
  m_most_recent_ioctl_max(most_recent_ioctl_max),
  m_max_filesize(max_filesize),
  m_count(0),
  m_most_recent_ioctl_file_cnt(0),
  m_file(nullptr)
{
  static unsigned int count(0);
  std::string filename_prefix;

  filename_prefix = read_from_environment<std::string>("I965_BLACKBOX_FILENAME", DEFAULT_FILENAME);
  if (m_most_recent_ioctl_max == 0)
    {
      std::ostringstream str;
      str << filename_prefix << "-" << ++count;
      m_prefix = str.str();
    }
  else
    {
      m_prefix = filename_prefix;
    }
  std::printf("i965-blackbox: Start new session \"%s\"\n", m_prefix.c_str());
  start_new_file();
}

Session::
~Session()
{
  close_file();
}

void
Session::
close_file(void)
{
  if (!m_file)
    {
      return;
    }

  for (auto iter = m_block_stack.rbegin(); iter != m_block_stack.rend(); ++iter)
    {
      write_to_file(I965_BATCHBUFFER_LOGGER_MESSAGE_BLOCK_END, nullptr, 0, nullptr, 0);
    }
  std::printf("i965-blackbox: close file \"%s\" of size %ld\n",
              m_filename.c_str(), std::ftell(m_file));
  std::fflush(stdout);
  std::fclose(m_file);
  m_file = nullptr;

  if (m_most_recent_ioctl_max > 0)
    {
      while (m_most_recent_ioctl_file_cnt >= m_most_recent_ioctl_max)
        {
          std::string file_to_delete;

          file_to_delete = m_most_recent_ioctl_files.front();
          std::remove(file_to_delete.c_str());
          m_most_recent_ioctl_files.pop_front();
          --m_most_recent_ioctl_file_cnt;
        }
      m_most_recent_ioctl_files.push_back(m_filename);
      ++m_most_recent_ioctl_file_cnt;
      m_filename.clear();
    }
}

void
Session::
start_new_file(void)
{
   close_file();

   std::ostringstream str;
   str << m_prefix << "." << m_count++;
   m_filename = str.str();
   m_file = std::fopen(m_filename.c_str(), "w");
   std::printf("i965-blackbox: Start new file \"%s\" at api-call #%u\n", m_filename.c_str(), api_count);
   std::fflush(stdout);
   for (auto iter = m_block_stack.begin(); iter != m_block_stack.end(); ++iter)
     {
       write_to_file(I965_BATCHBUFFER_LOGGER_MESSAGE_BLOCK_BEGIN,
                     iter->name(), iter->name_length(),
                     iter->value(), iter->value_length());
     }
}

void
Session::
write_to_file(enum i965_batchbuffer_logger_message_type_t tp,
              const void *name, uint32_t name_length,
              const void *value, uint32_t value_length)
{
   if (!m_file)
     {
       return;
     }

   struct i965_batchbuffer_logger_header hdr;

   hdr.type = tp;
   hdr.name_length = name_length;
   hdr.value_length = value_length;
   std::fwrite(&hdr, sizeof(hdr), 1, m_file);

   if (name_length > 0)
     {
       std::fwrite(name, sizeof(char), name_length, m_file);
     }

   if (value_length > 0)
     {
       std::fwrite(value, sizeof(char), value_length, m_file);
     }
}

void
Session::
close_fcn(void *pthis)
{
   Session *p;
   p = static_cast<Session*>(pthis);
   delete p;
}

void
Session::
pre_execbuffer2_ioctl_fcn(void *pthis, unsigned int id)
{
  Session *p;
  p = static_cast<Session*>(pthis);

  if (p->m_most_recent_ioctl_max > 0)
    {
      p->start_new_file();
      return;
    }

  if (!p->m_file)
    {
      return;
    }
  
  if (p->m_max_filesize > 0 && std::ftell(p->m_file) > p->m_max_filesize)
    {
      p->start_new_file();
    }
  else
    {
      std::printf("i965-blackbox: flush file \"%s\"\n", p->m_filename.c_str());
      std::fflush(p->m_file);
    }
}

void
Session::
post_execbuffer2_ioctl_fcn(void *pthis, unsigned int id)
{  
  Session *p;
  p = static_cast<Session*>(pthis);

  if (p->m_file)
    {
      if (p->m_most_recent_ioctl_max > 0)
        {
          p->close_file();
        }
      else
        {
          std::printf("i965-blackbox: flush\"%s\"\n", p->m_filename.c_str());
          std::fflush(p->m_file);
        }
    }
}

void
Session::
write_fcn(void *pthis,
          enum i965_batchbuffer_logger_message_type_t tp,
          const void *name, uint32_t name_length,
          const void *value, uint32_t value_length)
{
   Session *p;
   p = static_cast<Session*>(pthis);

   switch (tp)
     {
     case I965_BATCHBUFFER_LOGGER_MESSAGE_BLOCK_BEGIN:
       p->m_block_stack.push_back(Block());
       p->m_block_stack.back().set(name, name_length, value, value_length);
       break;

     case I965_BATCHBUFFER_LOGGER_MESSAGE_BLOCK_END:
       p->m_block_stack.pop_back();
       break;

     case I965_BATCHBUFFER_LOGGER_MESSAGE_VALUE:
       break;
     }
   p->write_to_file(tp, name, name_length, value, value_length);
}

/////////////////////////////////////////////
// and so it begins, the global vomit.

/* Grab a symbol, if handle is not initialized, then
 * intialize it with RTLD_NEXT (if it works) or a
 * handle for the named .so.
 */
static
void*
fetch_function(const char *name)
{
  if (prefer_gl_sym)
    {
      std::printf("i965-blackbox: Use gl_dlsym to fetch %s\n", name);
      return gl_dlsym(name);
    }
  else
    {
      std::printf("i965-blackbox: Use gles_dlsym to fetch %s\n", name);
      return gles_dlsym(name);        
    }
}

#define FUNCTION_ENTRY(name, type_arg_list, arg_list) \
  static void _##name##_init type_arg_list;           \
  typedef void (*_##name##_ftype) type_arg_list;      \
  _##name##_ftype _##name = _##name##_##init;         \
  static void _##name##_do_nothing type_arg_list      \
  {                                                   \
    return;                                           \
  }                                                   \
  static void _##name##_init type_arg_list            \
  {                                                   \
    _##name = (_##name##_ftype)fetch_function(#name); \
    if (!_##name)                                     \
      _##name = _##name##_do_nothing;                 \
    _##name arg_list;                                 \
  }

#define FUNCTION_ENTRY_RET(type, name, type_arg_list, arg_list) \
  static type _##name##_init type_arg_list;           \
  typedef type (*_##name##_ftype) type_arg_list;      \
  _##name##_ftype _##name = _##name##_##init;         \
  static type _##name##_do_nothing type_arg_list      \
  {                                                   \
    typedef type foo_type;                            \
    return foo_type();                                \
  }                                                   \
  static type _##name##_init type_arg_list            \
  {                                                   \
    _##name = (_##name##_ftype)fetch_function(#name); \
    if (!_##name)                                     \
      _##name = _##name##_do_nothing;                 \
    return _##name arg_list;                          \
  }

#include "build/function_macros.inc"

#undef FUNCTION_ENTRY
#undef FUNCTION_ENTRY_RET

#define FUNCTION_ENTRY(name, type_arg_list, arg_list)              \
  extern "C" void name type_arg_list                               \
  {                                                                \
    if (logger_app)                                                \
      {                                                            \
        logger_app->pre_call(logger_app, api_count, #name, #name); \
      }                                                            \
    _##name arg_list;                                              \
    if (logger_app)                                                \
      {                                                            \
        logger_app->post_call(logger_app, api_count);              \
      }                                                            \
    ++api_count;                                                   \
  }

#define FUNCTION_ENTRY_RET(type, name, type_arg_list, arg_list)    \
  extern "C" type name type_arg_list                               \
  {                                                                \
    type return_value;                                             \
    if (logger_app)                                                \
      {                                                            \
        logger_app->pre_call(logger_app, api_count, #name, #name); \
      }                                                            \
    return_value = _##name arg_list;                               \
    if (logger_app)                                                \
      {                                                            \
        logger_app->post_call(logger_app, api_count);              \
      }                                                            \
    ++api_count;                                                   \
    return return_value;                                           \
  }

#include "build/function_macros.inc"

extern "C" void glXSwapBuffers(void *dpy, GLXDrawable drawable);
extern "C" EGLBoolean eglInitialize(void *dpy, int32_t *major, int32_t *minor);
extern "C" unsigned int eglSwapBuffers(void *dpy, void *surface);
extern "C" void* glXGetProcAddress(const char *name);
extern "C" void* glXGetProcAddressARB(const char *name);
extern "C" void* eglGetProcAddress(const char *name);

struct function_list every_function[] =
  {
#undef FUNCTION_ENTRY
#undef FUNCTION_ENTRY_RET
#define FUNCTION_ENTRY(name, type_arg_list, arg_list) { #name, (void*)name },
#define FUNCTION_ENTRY_RET(type, name, type_arg_list, arg_list) { #name, (void*)name },
#include "build/function_macros.inc"
    FUNCTION_ENTRY(glXSwapBuffers, X, X)
    FUNCTION_ENTRY(eglInitialize, X, X)
    FUNCTION_ENTRY(eglSwapBuffers, X, X)
    FUNCTION_ENTRY(glXGetProcAddress, X, X)
    FUNCTION_ENTRY(glXGetProcAddressARB, X, X)
    FUNCTION_ENTRY(eglGetProcAddress, X, X)
  };

static
void*
gl_function(const char *name)
{
  const unsigned int sz = sizeof(every_function) / sizeof(every_function[0]);
  for (unsigned int i = 0; i < sz; ++i)
    {
      if (std::strcmp(name, every_function[i].m_name) == 0)
        {
          return every_function[i].m_function;
        }
    }
  return nullptr;
}

static
bool
frame_should_start_new_session(void)
{
  return numframes_per_file > 0
    && frame_count >= numframes_per_file
    && most_recent_ioctl_max == 0;
}

extern "C"
void
glXSwapBuffers(void *dpy, GLXDrawable drawable)
{
   typedef void (*fptr_type)(void*, GLXDrawable);
   static fptr_type fptr = nullptr;
   if (fptr == nullptr)
     {
       fptr = (fptr_type)gl_dlsym("glXSwapBuffers");
     }

   if (logger_app)
     {
       logger_app->pre_call(logger_app, api_count, "glXSwapBuffers", "glXSwapBuffers");
     }

   fptr(dpy, drawable);

   if (logger_app)
     {
       logger_app->post_call(logger_app, api_count);
       if (frame_should_start_new_session())
         {
           frame_count = 0;
           logger_app->end_session(logger_app, logger_session);
           logger_session = Session::start_session(most_recent_ioctl_max, logger_app, max_filesize);
         }
     }

   ++frame_count;
   ++api_count;
}

extern "C"
EGLBoolean
eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor)
{
  typedef EGLBoolean (*fptr_type)(EGLDisplay, EGLint*, EGLint*);
  static fptr_type fptr = nullptr;

  prefer_gl_sym = false;
  if (fptr == nullptr)
     {
       fptr = (fptr_type) FunctionFetcher::egl().dlsym_lib_style("eglInitialize");
     }
  return fptr(dpy, major, minor);
}

extern "C"
EGLBoolean
eglSwapBuffers(EGLDisplay dpy, EGLSurface surface)
{
   typedef unsigned int (*fptr_type)(EGLDisplay, EGLSurface);
   static fptr_type fptr = nullptr;
   unsigned int R;

   if (fptr == nullptr)
     {
       fptr = (fptr_type)egl_dlsym("eglSwapBuffers");
     }

   if (logger_app)
     {
       logger_app->pre_call(logger_app, api_count, "eglSwapBuffers", "eglSwapBuffers");
     }

   R = fptr(dpy, surface);

   if (logger_app)
     {
       logger_app->post_call(logger_app, api_count);
       if (frame_should_start_new_session())
         {
           frame_count = 0;
           logger_app->end_session(logger_app, logger_session);
           logger_session = Session::start_session(most_recent_ioctl_max, logger_app, max_filesize);
         }
     }

   ++frame_count;
   ++api_count;
   return R;
}

extern "C"
void*
glXGetProcAddress(const char *name)
{
   void *q;
   q = gl_function(name);
   if (q)
     {
       return q;
     }

   /* we do not know the function, sighs; just rely on
    * gl_dlsym() then.
    */
   return gl_dlsym(name);
}

extern "C"
void*
glXGetProcAddressARB(const char *name)
{
  return glXGetProcAddress(name);
}

extern "C"
void*
eglGetProcAddress(const char *name)
{
   prefer_gl_sym = false;

   void *q;
   q = gl_function(name);
   if (q)
     {
       return q;
     }

   q = egl_dlsym(name);
   if (q)
     {
       return q;
     }

   return gles_dlsym(name);
}

extern "C"
void*
dlsym(void *handle, const char *symbol)
{
   void *q;
   q = gl_function(symbol);
   if (q)
     {
       return q;
     }

   return real_dlsym(handle, symbol);
}

extern "C"
void*
dlopen(const char *filename, int flag)
{
  std::printf("i965-blackbox: dlopen(\"%s\", %d)\n", filename, flag);
  return __libc_dlopen_mode(filename, flag);
}

__attribute__((constructor))
static
void
start_session(void)
{
   max_filesize =
     read_from_environment<long>("I965_BLACKBOX_MAX_FILESIZE",
                                 DEFAULT_MAX_FILESIZE);
   std::printf("i965-blackbox: file size set to %ld\n", max_filesize);

   numframes_per_file =
     read_from_environment<unsigned int>("I965_BLACKBOX_MAX_FRAMES_PERFILE",
                                         DEFAULT_MAX_FRAMES_PER_FILE);
   std::printf("i965-blackbox: number frames to file set to %u\n", numframes_per_file);

   most_recent_ioctl_max =
     read_from_environment<unsigned int>("I965_BLACKBOX_NUM_MOST_RECENT_KEEP", 0);
   if (most_recent_ioctl_max > 0)
     {
       std::printf("i965-blackbox: keeping only %u most recent batchbuffer logs\n",
                   most_recent_ioctl_max);
     }
   
   logger_app = i965_batchbuffer_logger_app_acquire();
   logger_session = Session::start_session(most_recent_ioctl_max, logger_app, max_filesize);
}

__attribute__((destructor))
static
void
end_session(void)
{
   if (logger_app) {
      std::printf("i965-blackbox: shutdown.\n");
      logger_app->end_session(logger_app, logger_session);
      logger_app->release_app(logger_app);
      logger_app = nullptr;
   }
}
