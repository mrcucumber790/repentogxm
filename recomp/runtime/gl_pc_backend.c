/* Native WGL/OpenGL implementation of guest_gl_backend.
 *
 * The generated guest bridge owns x86 stack decoding.  This file is the one
 * platform edge where 32-bit identity-mapped guest addresses become native
 * PC pointers, and where typed APIENTRY pointers returned by KAGE's resolver
 * are called.  It is intentionally unusable in a 64-bit process: an array of
 * guest pointers (notably glShaderSource) is only layout-identical to a native
 * pointer array when both pointer widths are four bytes. */
#include "gl_pc_backend.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl_bridge.h"
#include "kage_pc_backend.h"

#define GL_PC_MAX_SYMBOLS GUEST_GL_SURFACE_COUNT

static size_t s_gl_pc_resolved_count;
static size_t s_gl_pc_missing_count;
static const char *s_gl_pc_missing[GL_PC_MAX_SYMBOLS];
static int s_gl_pc_installed;
static char s_gl_pc_error[192];

static void gl_pc_set_error(const char *message)
{
    if (!message)
        message = "unknown PC GL backend error";
#if defined(_MSC_VER)
    _snprintf_s(s_gl_pc_error, sizeof s_gl_pc_error, _TRUNCATE,
                "%s", message);
#else
    snprintf(s_gl_pc_error, sizeof s_gl_pc_error, "%s", message);
#endif
}

static void gl_pc_record_resolved(void)
{
    s_gl_pc_resolved_count++;
}

static void gl_pc_record_missing(const char *name)
{
    if (s_gl_pc_missing_count < GL_PC_MAX_SYMBOLS)
        s_gl_pc_missing[s_gl_pc_missing_count++] = name;
}

#if (defined(_WIN32) && UINTPTR_MAX == UINT32_MAX) || GUEST_MEM_WINDOW

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
/* 64-bit window hosts (SDL + desktop GL): native pointers come from
 * kage_pc_backend_gl_proc(); guest addresses are translated per call. */
#define APIENTRY
#endif

/* Exact Khronos scalar layout, without importing a second GL header into the
 * guest ABI translation unit. */
typedef unsigned int  GLenum;
typedef unsigned int  GLbitfield;
typedef unsigned int  GLuint;
typedef int           GLint;
typedef int           GLsizei;
typedef unsigned char GLboolean;
typedef float         GLfloat;
typedef double        GLdouble;
typedef char          GLchar;
typedef unsigned char GLubyte;

static void gl_pc_debug_after_call(const char *name)
{
    kage_pc_backend_note_guest_gl_call(name);
}

/* glGetString results are driver memory.  The guest receives a copy inside
 * guest-addressable memory, made once per (name, index) and kept for the
 * life of the process, matching the driver's own lifetime guarantee. */
#define GL_PC_STRING_SLOTS 512u

typedef struct gl_pc_string_slot {
    uint32_t name;
    uint32_t index;
    char *copy;
} gl_pc_string_slot;

static gl_pc_string_slot s_gl_pc_strings[GL_PC_STRING_SLOTS];

static guest_gl_addr gl_pc_guest_string(guest_gl_enum name, uint32_t index,
                                        const GLubyte *native)
{
#if GUEST_MEM_WINDOW
    size_t i;
    size_t length;

    if (!native)
        return 0u;
    for (i = 0u; i < GL_PC_STRING_SLOTS; ++i) {
        gl_pc_string_slot *slot = &s_gl_pc_strings[i];
        if (slot->copy && slot->name == name && slot->index == index)
            return guest_addr(slot->copy);
        if (!slot->copy) {
            length = strlen((const char *)native) + 1u;
            slot->copy = (char *)malloc(length);
            if (!slot->copy)
                return 0u;
            memcpy(slot->copy, native, length);
            slot->name = name;
            slot->index = index;
            return guest_addr(slot->copy);
        }
    }
    return 0u;
#else
    (void)name;
    (void)index;
    (void)s_gl_pc_strings;
    return (guest_gl_addr)(uintptr_t)native;
#endif
}

typedef void (APIENTRY *gl_pc_shader_source_fn)(
    GLuint shader, GLsizei count, const GLchar *const *string,
    const GLint *length);

/* The guest passes an array of 32-bit guest string addresses. */
static void gl_pc_shader_source(gl_pc_shader_source_fn native,
                                guest_gl_uint shader, guest_gl_sizei count,
                                guest_gl_addr strings, guest_gl_addr lengths)
{
#if GUEST_MEM_WINDOW
    const GLchar *local[16];
    const GLchar **host = local;
    guest_gl_sizei i;

    if (count < 0)
        count = 0;
    if ((size_t)count > sizeof local / sizeof local[0]) {
        host = (const GLchar **)malloc((size_t)count * sizeof *host);
        if (!host)
            return;
    }
    for (i = 0; i < count; ++i)
        host[i] = (const GLchar *)guest_ptr(ld32(strings + 4u * (uint32_t)i));
    native((GLuint)shader, (GLsizei)count, host,
           (const GLint *)guest_ptr(lengths));
    if (host != local)
        free((void *)host);
#else
    native((GLuint)shader, (GLsizei)count,
           (const GLchar *const *)(uintptr_t)strings,
           (const GLint *)(uintptr_t)lengths);
#endif
}

#include "gl_pc_backend_generated.inc"

#if GUEST_MEM_WINDOW
/* Location memo.  The game asks glGetUniformLocation / glGetAttribLocation
 * by name on every draw; Mesa hashes the name (SHA-1) per query, which was
 * ~5% of a frame on the Switch.  Locations only change when a program is
 * relinked or deleted, and both clear the memo. */
#define GL_PC_LOCATION_SLOTS 1024u
#define GL_PC_LOCATION_NAME  48u

typedef struct gl_pc_location_slot {
    uint32_t program;   /* 0 = empty */
    uint32_t kind;      /* 1 = uniform, 2 = attribute */
    int32_t location;
    char name[GL_PC_LOCATION_NAME];
} gl_pc_location_slot;

static gl_pc_location_slot s_gl_pc_locations[GL_PC_LOCATION_SLOTS];
static guest_gl_int (*s_gl_pc_uniform_location)(guest_gl_uint, guest_gl_addr);
static guest_gl_int (*s_gl_pc_attrib_location)(guest_gl_uint, guest_gl_addr);
static void (*s_gl_pc_link_program)(guest_gl_uint);
static void (*s_gl_pc_delete_program)(guest_gl_uint);

static guest_gl_int gl_pc_cached_location(
    uint32_t kind, guest_gl_uint program, guest_gl_addr name,
    guest_gl_int (*query)(guest_gl_uint, guest_gl_addr))
{
    const char *text = (const char *)guest_ptr(name);
    uint32_t hash = 2166136261u ^ kind ^ (program * 16777619u);
    size_t length;
    uint32_t probe;

    if (!text || program == 0u)
        return query(program, name);
    for (length = 0u; length < GL_PC_LOCATION_NAME && text[length]; ++length)
        hash = (hash ^ (uint8_t)text[length]) * 16777619u;
    if (length >= GL_PC_LOCATION_NAME)
        return query(program, name);
    for (probe = 0u; probe < 16u; ++probe) {
        gl_pc_location_slot *slot =
            &s_gl_pc_locations[(hash + probe) & (GL_PC_LOCATION_SLOTS - 1u)];
        if (!slot->program) {
            guest_gl_int location = query(program, name);
            slot->program = program;
            slot->kind = kind;
            slot->location = location;
            memcpy(slot->name, text, length + 1u);
            return location;
        }
        if (slot->program == program && slot->kind == kind &&
            memcmp(slot->name, text, length + 1u) == 0)
            return slot->location;
    }
    return query(program, name);
}

static guest_gl_int gl_pc_memo_uniform_location(guest_gl_uint program,
                                                guest_gl_addr name)
{
    return gl_pc_cached_location(1u, program, name,
                                 s_gl_pc_uniform_location);
}

static guest_gl_int gl_pc_memo_attrib_location(guest_gl_uint program,
                                               guest_gl_addr name)
{
    return gl_pc_cached_location(2u, program, name,
                                 s_gl_pc_attrib_location);
}

static void gl_pc_memo_link_program(guest_gl_uint program)
{
    memset(s_gl_pc_locations, 0, sizeof s_gl_pc_locations);
    s_gl_pc_link_program(program);
}

static void gl_pc_memo_delete_program(guest_gl_uint program)
{
    memset(s_gl_pc_locations, 0, sizeof s_gl_pc_locations);
    s_gl_pc_delete_program(program);
}

static void gl_pc_install_location_memo(guest_gl_backend *backend)
{
    memset(s_gl_pc_locations, 0, sizeof s_gl_pc_locations);
    if (!backend->glGetUniformLocation || !backend->glGetAttribLocation ||
        !backend->glLinkProgram || !backend->glDeleteProgram)
        return;
    s_gl_pc_uniform_location = backend->glGetUniformLocation;
    s_gl_pc_attrib_location = backend->glGetAttribLocation;
    s_gl_pc_link_program = backend->glLinkProgram;
    s_gl_pc_delete_program = backend->glDeleteProgram;
    backend->glGetUniformLocation = gl_pc_memo_uniform_location;
    backend->glGetAttribLocation = gl_pc_memo_attrib_location;
    backend->glLinkProgram = gl_pc_memo_link_program;
    backend->glDeleteProgram = gl_pc_memo_delete_program;
}
#endif

typedef char gl_pc_surface_count_must_match[
    GL_PC_BACKEND_GENERATED_COUNT == GUEST_GL_SURFACE_COUNT ? 1 : -1];

int gl_pc_backend_install(void)
{
    guest_gl_backend backend;

    if (!kage_pc_backend_ready()) {
        gl_pc_set_error("PC GL backend: KAGE WGL context is not ready");
        return 0;
    }

    memset(&backend, 0, sizeof backend);
    memset(&s_gl_pc_native, 0, sizeof s_gl_pc_native);
    memset((void *)s_gl_pc_missing, 0, sizeof s_gl_pc_missing);
    s_gl_pc_resolved_count = 0u;
    s_gl_pc_missing_count = 0u;
    s_gl_pc_installed = 0;

    gl_pc_resolve_generated();
    gl_pc_bind_generated(&backend);
#if GUEST_MEM_WINDOW
    gl_pc_install_location_memo(&backend);
#endif
    guest_gl_install_backend(&backend);
    s_gl_pc_installed = 1;

    if (s_gl_pc_missing_count) {
#if defined(_MSC_VER)
        _snprintf_s(s_gl_pc_error, sizeof s_gl_pc_error, _TRUNCATE,
                    "PC GL backend: %u of %u native symbols missing",
                    (unsigned)s_gl_pc_missing_count,
                    (unsigned)GUEST_GL_SURFACE_COUNT);
#else
        snprintf(s_gl_pc_error, sizeof s_gl_pc_error,
                 "PC GL backend: %u of %u native symbols missing",
                 (unsigned)s_gl_pc_missing_count,
                 (unsigned)GUEST_GL_SURFACE_COUNT);
#endif
    } else {
        s_gl_pc_error[0] = '\0';
    }
    return 1;
}

#else

int gl_pc_backend_install(void)
{
    gl_pc_set_error(
        "PC GL backend: requires a 32-bit Windows identity-pointer build");
    return 0;
}

#endif

void gl_pc_backend_uninstall(void)
{
    /* Call this before destroying the WGL context; it prevents stale driver
     * function pointers from remaining reachable through guest dispatch. */
    guest_gl_install_backend(NULL);
    s_gl_pc_installed = 0;
    s_gl_pc_resolved_count = 0u;
    s_gl_pc_missing_count = 0u;
    memset((void *)s_gl_pc_missing, 0, sizeof s_gl_pc_missing);
}

int gl_pc_backend_installed(void) { return s_gl_pc_installed; }
int gl_pc_backend_complete(void)
{
    return s_gl_pc_installed && s_gl_pc_missing_count == 0u;
}
size_t gl_pc_backend_resolved_count(void) { return s_gl_pc_resolved_count; }
size_t gl_pc_backend_missing_count(void) { return s_gl_pc_missing_count; }
const char *gl_pc_backend_missing_symbol(size_t index)
{
    return index < s_gl_pc_missing_count ? s_gl_pc_missing[index] : NULL;
}
const char *gl_pc_backend_last_error(void)
{
    return s_gl_pc_error[0] ? s_gl_pc_error : "PC GL backend: no error";
}
