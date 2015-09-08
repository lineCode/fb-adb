/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in
 *  the LICENSE file in the root directory of this source tree. An
 *  additional grant of patent rights can be found in the PATENTS file
 *  in the same directory.
 *
 */
#pragma once
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include "dbg.h"

#ifndef ECOMM
#define ECOMM EBADRPC
#endif

#define ERR_ERRNO_WAS_ZERO -1

#define ARRAYSIZE(ar) (sizeof (ar) / sizeof (*(ar)))

// Suppress compiler warning about unused computation
static inline bool verify_dummy(bool x) { return x; }

#ifdef NDEBUG
# define VERIFY(x) (verify_dummy(x))
#else
# define VERIFY(x) (assert(x))
#endif

// Reslists own resources (or more precisely, they contain lists of
// cleanup closures).  Every time we allocate a resource, we mark it
// owned by the reslist named by _reslist_current.  reslist deallocate
// their owned resources when they are destroyed.  Scoped reslists are
// automatically destroyed when they go out of scope.  They go out of
// scope either on normal return (in which case the compiler runs the
// cleanup) or on die(), in which case the reslist machiney takes care
// of running the cleanups.
//
// The only operations that can affect the _reslist_current are
// SCOPED_RESLIST and WITH_CURRENT_RESLIST.

typedef void (*cleanupfn)(void* data);

struct resource {
    enum { RES_RESLIST_ONHEAP, RES_RESLIST_ONSTACK, RES_CLEANUP } type;
    // Circular list
    struct resource* prev;
    struct resource* next;
};

struct reslist {
    struct resource r;
    struct reslist* parent;
    struct resource head;
};

struct cleanup {
    struct resource r;
    cleanupfn fn;
    void* fndata;
};

// Create a new reslist owned by _reslist_current.
// Does _not_ make the new reslist current.
struct reslist* reslist_create(void);

// Destroy a reslist.  Cleans up all resources owned by that reslist.
void reslist_destroy(struct reslist* rl);

// Transfer resources owned by resist DONOR to reslist RECIPIENT.
// DONOR's resources are spliced in-order to the head of RECIPIENT.
// That is, when RECIPIENT is destroyed, all of DONOR's resources are
// cleaned up, and then all of RECIPIENT's.
void reslist_xfer(struct reslist* recipient, struct reslist* donor);

void _reslist_scoped_push(struct reslist* rl);
void _reslist_scoped_pop(struct reslist* rl);

void _reslist_guard_push(struct reslist** saved_rl, struct reslist* rl);
void _reslist_guard_pop(struct reslist** saved_rl);

#define PASTE(a,b) a##b
#define GENSYM(sym) PASTE(sym, __LINE__)

#define SCOPED_RESLIST(varname)                                 \
    __attribute__((cleanup(_reslist_scoped_pop)))               \
    struct reslist PASTE(varname,_reslist_);                    \
    struct reslist* varname = &PASTE(varname,_reslist_);        \
    _reslist_scoped_push(varname)

#define WITH_CURRENT_RESLIST(_rl)                               \
    __attribute__((cleanup(_reslist_guard_pop)))                \
    struct reslist* GENSYM(_reslist_saved);                     \
    _reslist_guard_push(&GENSYM(_reslist_saved), (_rl))

// Each resource owned by a reslist is associated with a cleanup
// function.  Adding a cleanup function requires allocating memory and
// can fail.  To make sure we can clean up every resource we allocate,
// we allocate a cleanup *before* the resource it owns, allocate the
// resource, and commit the cleanup object to that resource.
// The commit operation cannot fail.

// Allocate a new cleanup object. The storage for the cleanup object
// itself is owned by the current reslist (and is heap-allocated, and
// so can fail), but the new cleanup owns no resource.
struct cleanup* cleanup_allocate(void);

// Commit the cleanup object to a resource.  The cleanup object must
// have been previously allocated with cleanup_allocate.  A given
// cleanup object can be committed to a resource once.  When a cleanup
// object is allocated to a resource, it is re-inserted at the head of
// the current reslist.  Reslists clean up their resources in reverse
// order of insertion.
void cleanup_commit(struct cleanup* cl, cleanupfn fn, const void* fndata);

// Deregister and deallocate the given cleanup object CL, but do not
// run any cleanup functions to which CL may have been committed.
// If CL is NULL, do nothing.
void cleanup_forget(struct cleanup* cl);

// Allocate and commit a cleanup object that will unlink a file of the
// given name.  Failure to unlink the file is ignored.
struct unlink_cleanup;
struct unlink_cleanup* unlink_cleanup_allocate(const char* filename);
void unlink_cleanup_commit(struct unlink_cleanup* ucl);

// Allocate memory owned by the current reslist.
__attribute__((malloc))
void* xalloc(size_t sz);
__attribute__((malloc))
void* xcalloc(size_t sz);

// Code that fails calls die() or one of its variants below.
// Control then flows to the nearest enclosing catch_error.

typedef struct errinfo {
    int err;
    const char* msg;
    const char* prgname;
    unsigned want_msg : 1;
} errinfo;

// Call FN with FNDATA with an internal resource list as current.
// If FN returns normally, transfer resources added to that resource
// list to the resource list that was current at the time of
// catch_error.  On error, destroy the resource list.  Return false on
// normal return or true on error.  If EI is non-null, fill it on
// error.  Strings are allocated on the resource list in effect at the
// time catch_error is called.  If want_msg is zero, error strings are
// not allocated, but ei->err is still set.
bool catch_error(void (*fn)(void* fndata),
                 void* fndata,
                 struct errinfo* ei);

// Like catch_error, but return true only in the case
// that we get an error matching ERRNUM; otherwise, rethrow.
bool catch_one_error(
    void (*fn)(void* fndata),
    void* fndata,
    int errnum);

__attribute__((noreturn))
void die_rethrow(struct errinfo* ei);

__attribute__((noreturn))
void diev(int err, const char* fmt, va_list args);
__attribute__((noreturn,format(printf, 2, 3)))
void die(int err, const char* fmt, ...);
__attribute__((noreturn,format(printf, 1, 2)))
void die_errno(const char* fmt, ...);
__attribute__((noreturn))
void die_oom(void);

__attribute__((malloc,format(printf, 1, 2)))
char* xaprintf(const char* fmt, ...);
__attribute__((malloc))
char* xavprintf(const char* fmt, va_list args);
__attribute__((malloc))
char* xstrdup(const char* s);
char* xstrndup(const char* s, size_t nr);

bool error_temporary_p(int errnum);

extern const char* orig_argv0;
extern const char* prgname;
void set_prgname(const char* s);
extern int real_main(int argc, char** argv);

size_t nextpow2sz(size_t sz);

#define XMIN(a,b)                \
    ({ typeof (a) _a = (a);      \
        typeof (a) _b = (b);     \
        _a > _b ? _b : _a; })

#define XMAX(a,b)                \
    ({ typeof (a) _a = (a);      \
        typeof (a) _b = (b);     \
        _a > _b ? _a : _b; })

#define SATADD(r, a, b)                         \
    ({                                          \
        typedef __typeof((*(r))) xT;            \
        xT xa = (a);                            \
        xT xb = (b);                            \
        int overflow;                           \
        if (((xT) -1) - xa < xb) {              \
            overflow = 1;                       \
            *(r) = ((xT) -1);                   \
        } else {                                \
            overflow = 0;                       \
            *(r) = xa + xb;                     \
        }                                       \
                                                \
        overflow;                               \
    })

#define XPOW2P(v)                               \
    ({                                          \
        __typeof((v)) _v = (v);                 \
        (_v & (_v-1)) == 0;                     \
    })

struct iovec;
size_t iovec_sum(const struct iovec* iov, unsigned niovec);

#ifndef _POSIX_VDISABLE
#define _POSIX_VDISABLE 0
#endif

void* generate_random_bytes(size_t howmany);
char* hex_encode_bytes(const void* bytes, size_t n);
char* gen_hex_random(size_t nr_bytes);

void* first_non_null(void* s, ...);
bool string_starts_with_p(const char* string, const char* prefix);
bool string_ends_with_p(const char* string, const char* suffix);

#ifdef HAVE_CLOCK_GETTIME
double xclock_gettime(clockid_t clk_id);
#endif

extern sigset_t signals_unblock_for_io;
extern sigset_t orig_sigmask;
extern int signal_quit_in_progress;

void _unblock_io_unblocked_signals(sigset_t* saved);
void _restore_io_unblocked_signals(sigset_t* saved);

#define WITH_IO_SIGNALS_ALLOWED()                                       \
    __attribute__((cleanup(_restore_io_unblocked_signals)))             \
    sigset_t GENSYM(_saved_signals);                                    \
    _unblock_io_unblocked_signals(&GENSYM(_saved_signals))

void save_signals_unblock_for_io(void);
void sigaction_restore_as_cleanup(int signo, struct sigaction* sa);

// Like execvpe, but actually works on bionic.  Die on error.
// Not async-signal-safe.
__attribute__((noreturn))
void xexecvpe(const char* file,
              const char* const* argv,
              const char* const* envp);

struct sigtstp_cookie;
enum sigtstp_mode {
    SIGTSTP_BEFORE_SUSPEND,
    SIGTSTP_AFTER_RESUME,
    SIGTSTP_AFTER_UNEXPECTED_SIGCONT,
};

typedef void (*sigtstp_callback)(
    enum sigtstp_mode mode,
    void* data);

struct sigtstp_cookie* sigtstp_register(sigtstp_callback cb, void* cbdata);
void sigtstp_unregister(struct sigtstp_cookie* cookie);

// Set an itimer timeout.  Blocks SIGALRM except inside IO.  If we get
// a SIGALRM, raise a die-exception from IO context.  Restore signals
// and timer on unwind.
void set_timeout(const struct itimerval* timer);

// When set, caller promises to re-raise pending quit signals, so
// don't longjmp out of them immediately.
extern bool hack_defer_quit_signals;
