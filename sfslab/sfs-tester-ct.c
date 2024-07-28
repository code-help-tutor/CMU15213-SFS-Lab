WeChat: cstutorcs
QQ: 749389476
Email: tutorcs@163.com
#include "lanes/lanes.h"
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "sfs-api.h"
#include "sfs_threads.h"

#include <argp.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <readline/history.h>
#include <readline/readline.h>

int ct_orig_main(int, char **);

static_assert(sizeof(lua_Integer) >= sizeof(ssize_t),
              "expected lua_Integer to be able to hold ssize_t");

/// Helper: Functionally the same as luaL_checklstring, but does not
/// accept numbers.  (luaL_checklstring just wraps lua_tolstring,
/// which will silently convert numbers to strings *in place*,
/// potentially corrupting any table within which the number is a key.
/// I am not making this up, it's the documented behavior:
/// <https://www.lua.org/manual/5.4/manual.html#lua_tolstring>.)
static const char *luaL_checklstring_strict(lua_State *L, int index,
                                            size_t *len)
{
    if (lua_type(L, index) != LUA_TSTRING)
    {
        luaL_typeerror(L, index, lua_typename(L, LUA_TSTRING));
        // The compiler is not aware that luaL_typeerror does not return.
        return NULL;
    }
    return lua_tolstring(L, index, len);
}

/// Helper: Accept an argument which was passed as a Lua integer but
/// is properly a size_t.  If it is negative, or if it is too positive
/// to fit in ssize_t, issue an error.
static size_t luaL_checksize(lua_State *L, int index)
{
    lua_Integer val = luaL_checkinteger(L, index);
    if (val < 0)
    {
        luaL_argerror(L, index, "may not be negative");
        // The compiler is not aware that luaL_argerror does not return.
        return 0;
    }
    size_t sval = (size_t)val;
    // This condition _may_ be redundant but I don't feel like proving it.
    if (sval > (size_t)SSIZE_MAX)
    {
        luaL_argerror(L, index, "too positive for ssize_t");
        return 0;
    }
    return sval;
}

/// Helper: Accept an argument which was passed as a Lua integer but
/// is properly a file descriptor, i.e. a positive 'int'.  If it is
/// outside the appropriate range, issue an error.
static int luaL_checkfd(lua_State *L, int index)
{
    lua_Integer val = luaL_checkinteger(L, index);
    if (val < 0)
    {
        luaL_argerror(L, index, "may not be negative");
        return 0;
    }
    if (val > (lua_Integer)INT_MAX)
    {
        luaL_argerror(L, index, "too positive for int");
        return 0;
    }
    return (int)val;
}

/// Helper: Return (fail, strerror(err), err) to Lua. This is the
/// error reporting convention used by the stock 'io' library.  'fail'
/// is an unspecified but falsey value.  Note that all integer values
/// are truthy in Lua, including zero.  (We can't use luaL_fileresult
/// for this because it doesn't take the actual error code as an
/// argument.)
static int luaL_ioerror(lua_State *L, int err)
{
    luaL_pushfail(L);
    lua_pushstring(L, strerror(err));
    lua_pushinteger(L, err);
    return 3;
}

/// Helper: Return (fail, sprintf("%s: %s", fname, strerror(err)), err)
/// to Lua.  For failed operations that have an interesting name
/// to report along with the error code.
static int luaL_ioerror_f(lua_State *L, int err, const char *fname)
{
    luaL_pushfail(L);
    lua_pushfstring(L, "%s: %s", fname, strerror(err));
    lua_pushinteger(L, err);
    return 3;
}

// The sfs-disk API is wrapped as a table of functions which is made
// available to trace code.

// disk.format(diskName, diskSize) returns an unspecified truthy value
// on success or a failure tuple on error.
static int disk_format(lua_State *L)
{
    const char *disk = luaL_checklstring_strict(L, 1, NULL);
    size_t size = luaL_checksize(L, 2);

    int result = sfs_format(disk, size);
    if (result != 0)
        return luaL_ioerror_f(L, -result, disk);
    lua_pushboolean(L, 1);
    return 1;
}

// disk.mount(diskName) returns an unspecified truthy value on success
// or a failure tuple on error.
static int disk_mount(lua_State *L)
{
    const char *disk = luaL_checklstring_strict(L, 1, NULL);

    int result = sfs_mount(disk);
    if (result != 0)
        return luaL_ioerror_f(L, -result, disk);
    lua_pushboolean(L, 1);
    return 1;
}

// disk.unmount() returns an unspecified truthy value on success
// or a failure tuple on error.
static int disk_unmount(lua_State *L)
{
    int result = sfs_unmount();
    if (result != 0)
        return luaL_ioerror(L, -result);
    lua_pushboolean(L, 1);
    return 1;
}

// disk.open(fileName) returns the fd on success or a failure tuple on error.
static int disk_open(lua_State *L)
{
    const char *fname = luaL_checklstring_strict(L, 1, NULL);
    int fd = sfs_open(fname);
    if (fd < 0)
    {
        return luaL_ioerror_f(L, -fd, fname);
    }
    lua_pushinteger(L, fd);
    return 1;
}

// disk.close(fd) cannot fail, returns nothing
static int disk_close(lua_State *L)
{
    int fd = luaL_checkfd(L, 1);
    sfs_close(fd);
    return 0;
}

// disk.read(fd, [maxbytes]) returns a Lua string (possibly containing
// internal NULs) on success (including EOF, which will produce an empty
// string) or a failure tuple on error.
static int disk_read(lua_State *L)
{
    int fd = luaL_checkfd(L, 1);
    size_t maxbytes = luaL_opt(L, luaL_checksize, 2, LUAL_BUFFERSIZE);

    luaL_Buffer B;
    char *place = luaL_buffinitsize(L, &B, maxbytes);

    ssize_t nread = sfs_read(fd, place, maxbytes);
    if (nread < 0)
    {
        // There doesn't appear to be a more efficient way to discard
        // a buffer than this.
        luaL_pushresultsize(&B, 0);
        lua_pop(L, 1);
        return luaL_ioerror(L, (int)-nread);
    }

    luaL_pushresultsize(&B, (size_t)nread);
    return 1;
}

// disk.write(fd, buf) -- length of the buffer is used as the length argument
// returns the number of bytes successfully written, or a failure tuple.
static int disk_write(lua_State *L)
{
    int fd = luaL_checkfd(L, 1);

    size_t bufsiz;
    const char *buf = luaL_checklstring_strict(L, 2, &bufsiz);

    ssize_t nwritten = sfs_write(fd, buf, bufsiz);
    if (nwritten < 0)
    {
        return luaL_ioerror(L, (int)-nwritten);
    }

    lua_pushinteger(L, nwritten);
    return 1;
}

// disk.seek(fd, delta) returns the new seek position on success, or a
// failure tuple.  note that delta is signed and interpreted relative
// to the current seek position.
static int disk_seek(lua_State *L)
{
    int fd = luaL_checkfd(L, 1);
    lua_Integer delta = luaL_checkinteger(L, 2);
    lua_Integer newpos = sfs_seek(fd, delta);
    if (newpos < 0)
    {
        return luaL_ioerror(L, (int)-newpos);
    }
    lua_pushinteger(L, newpos);
    return 1;
}

// disk.getpos(fd) returns the current position on success, or a
// failure tuple.
static int disk_getpos(lua_State *L)
{
    int fd = luaL_checkfd(L, 1);
    lua_Integer pos = sfs_getpos(fd);
    if (pos < 0)
    {
        return luaL_ioerror(L, (int)-pos);
    }
    lua_pushinteger(L, pos);
    return 1;
}

// disk.remove(name) returns an unspecified truthy value on success
// or a failure tuple on error.
static int disk_remove(lua_State *L)
{
    const char *fname = luaL_checklstring_strict(L, 1, NULL);

    int result = sfs_remove(fname);
    if (result != 0)
        return luaL_ioerror_f(L, -result, fname);
    lua_pushboolean(L, 1);
    return 1;
}

// disk.rename(oldname, newname) returns an unspecified truthy value on success
// or a failure tuple on error.
static int disk_rename(lua_State *L)
{
    const char *oldname = luaL_checklstring_strict(L, 1, NULL);
    const char *newname = luaL_checklstring_strict(L, 2, NULL);

    int result = sfs_rename(oldname, newname);
    if (result == 0)
    {
        lua_pushboolean(L, 1);
        return 1;
    }
    // This is the only place where we need to report *two*
    // names in the error message.
    luaL_pushfail(L);
    lua_pushfstring(L, "rename(%s -> %s): %s", oldname, newname,
                    strerror(-result));
    lua_pushinteger(L, -result);
    return 3;
}

// disk.list() returns an array of strings on success, or a
// failure tuple on error.  (Done this way mainly because I do not
// presently have the time to learn how the Lua iterator protocol
// works.)
static int disk_list(lua_State *L)
{
    sfs_list_cookie cookie = NULL;
    int status;
    luaL_Buffer B;
    lua_createtable(L, 8, 0);
    lua_Integer idx = 1; // Lua arrays are 1-based
    for (;;)
    {
        char *dest = luaL_buffinitsize(L, &B, SFS_FILE_NAME_SIZE_LIMIT);
        status = sfs_list(&cookie, dest, SFS_FILE_NAME_SIZE_LIMIT);
        if (status != 0)
            break;
        luaL_pushresultsize(&B, strlen(dest));
        lua_rawseti(L, 1, idx);
        idx++;
    }

    // There's a live luaL_buffer right now, but sfs_list didn't write
    // anything into it.  There is no auxlib API to discard a buffer
    // without pushing its contents.
    luaL_pushresultsize(&B, 0);
    if (status > 0)
    {
        // Discard the junk string from the above luaL_pushresultsize call,
        // leaving the table in place to be returned.
        lua_pop(L, 1);
        return 1;
    }
    else
    {
        // Discard both the junk string and the table.
        lua_pop(L, 2);
        return luaL_ioerror(L, -status);
    }
}

static const luaL_Reg disk_fns[] = {
    {"format", disk_format},
    {"mount", disk_mount},
    {"unmount", disk_unmount},
    {"open", disk_open},
    {"close", disk_close},
    {"read", disk_read},
    {"write", disk_write},
    {"seek", disk_seek},
    {"getPos", disk_getpos},
    {"remove", disk_remove},
    {"rename", disk_rename},
    {"list", disk_list},
    {0, 0},
};

// This must be separate from init_lua so that we can make the "disk"
// table available via "require", which is necessary for it to be
// available in lane functions.
static int luaopen_disk(lua_State *L)
{
    luaL_newlib(L, disk_fns);
    return 1;
}

//
// Note: Much code below was cribbed from lua/lua.c.
//

// The part of interpreter initialization that isn't done by luaL_newstate.
// Must be called "in protected mode", i.e. there must be a lua_[x]pcall
// frame on the C stack outer to us.  Throws a Lua error on failure.
static void init_lua(lua_State *L)
{
    // sanity check
    luaL_checkversion(L);

    // load the Lua standard libraries
    luaL_openlibs(L);

    // load Lanes
    luaopen_lanes(L);

    // load the disk functions
    luaL_requiref(L, "disk", luaopen_disk, 1);

    // Restart the garbage collector and enable generational collection.
    // (It was stopped in main, immediately after invoking luaL_newstate.)
    lua_gc(L, LUA_GCRESTART);
    lua_gc(L, LUA_GCGEN, 0, 0);
}

// Run a trace
static void execute_trace(lua_State *L, const char *tracefile)
{
    if (!strcmp(tracefile, "/dev/stdin"))
    {
        tracefile = NULL; // luaL_loadfile takes NULL as stdin
    }
    int status = luaL_loadfile(L, tracefile);
    if (status != LUA_OK)
    {
        lua_error(L);
        return;
    }
    lua_call(L, 0, 0);
}

// Interactive Lua interpreter.  Most of this code was copied verbatim from
// lua/lua.c.
#define lua_initreadline(L) ((void)L, rl_readline_name = "lua")
#define lua_readline(L, b, p) ((void)L, ((b) = readline(p)) != NULL)
#define lua_saveline(L, line) ((void)L, add_history(line))
#define lua_freeline(L, b) ((void)L, free(b))

/* mark in error messages for incomplete statements */
#define EOFMARK "<eof>"
#define EOFMARKLEN (sizeof(EOFMARK) - 1)
#define LUA_PROMPT "> "
#define LUA_PROMPT2 ">> "
#define LUA_MAXINPUT 512

/*
** Return the string to be used as a prompt by the interpreter. Leave
** the string (or nil, if using the default value) on the stack, to keep
** it anchored.
*/
static const char *get_prompt(lua_State *L, int firstline)
{
    if (lua_getglobal(L, firstline ? "_PROMPT" : "_PROMPT2") == LUA_TNIL)
        return (firstline ? LUA_PROMPT : LUA_PROMPT2); /* use the default */
    else
    { /* apply 'tostring' over the value */
        const char *p = luaL_tolstring(L, -1, NULL);
        lua_remove(L, -2); /* remove original value */
        return p;
    }
}

/*
** Check whether 'status' signals a syntax error and the error
** message at the top of the stack ends with the above mark for
** incomplete statements.
*/
static int incomplete(lua_State *L, int status)
{
    if (status == LUA_ERRSYNTAX)
    {
        size_t lmsg;
        const char *msg = lua_tolstring(L, -1, &lmsg);
        if (lmsg >= EOFMARKLEN && strcmp(msg + lmsg - EOFMARKLEN, EOFMARK) == 0)
        {
            lua_pop(L, 1);
            return 1;
        }
    }
    return 0; /* else... */
}

/*
** Prompt the user, read a line, and push it into the Lua stack.
*/
static int pushline(lua_State *L, int firstline)
{
    char buffer[LUA_MAXINPUT];
    char *b = buffer;
    size_t l;
    const char *prmt = get_prompt(L, firstline);
    int readstatus = lua_readline(L, b, prmt);
    if (readstatus == 0)
        return 0;  // no input (prompt will be popped by caller)
    lua_pop(L, 1); // remove prompt
    l = strlen(b);
    if (l > 0 && b[l - 1] == '\n') // line ends with newline?
        b[--l] = '\0';             // remove it
    if (firstline && b[0] == '=')  // for compatibility with 5.2, ...
        lua_pushfstring(L, "return %s", b + 1); // change '=' to 'return'
    else
        lua_pushlstring(L, b, l);
    lua_freeline(L, b);
    return 1;
}

/*
** Try to compile line on the stack as 'return <line>;'; on return, stack
** has either compiled chunk or original line (if compilation failed).
*/
static int addreturn(lua_State *L)
{
    const char *line = lua_tostring(L, -1); // original line
    const char *retline = lua_pushfstring(L, "return %s;", line);
    int status = luaL_loadbuffer(L, retline, strlen(retline), "=stdin");
    if (status == LUA_OK)
    {
        lua_remove(L, -2);         // remove modified line
        if (line[0] != '\0')       // non empty?
            lua_saveline(L, line); // keep history
    }
    else
        lua_pop(L, 2); // pop result from 'luaL_loadbuffer' and modified line
    return status;
}

/*
** Read multiple lines until a complete Lua statement
*/
static int multiline(lua_State *L)
{
    for (;;)
    { // repeat until gets a complete statement
        size_t len;
        const char *line = lua_tolstring(L, 1, &len);
        int status = luaL_loadbuffer(L, line, len, "=stdin");
        if (!incomplete(L, status) || !pushline(L, 0))
        {
            lua_saveline(L, line); // keep history
            return status; // cannot or should not try to add continuation line
        }
        lua_pushliteral(L, "\n"); // add newline...
        lua_insert(L, -2);        // ...between the two lines
        lua_concat(L, 3);         // join them
    }
}

/*
** Read a line and try to load (compile) it first as an expression (by
** adding "return " in front of it) and second as a statement. Return
** the final status of load/call with the resulting function (if any)
** in the top of the stack.
*/
static int loadline(lua_State *L)
{
    int status;
    lua_settop(L, 0);
    if (!pushline(L, 1))
        return -1; // no input
    if ((status = addreturn(L)) != LUA_OK)
    { // 'return ...' did not work?
        // try as command, maybe with continuation lines
        status = multiline(L);
    }
    lua_remove(L, 1); // remove line from the stack
    lua_assert(lua_gettop(L) == 1);
    return status;
}

/*
** Prints (calling the Lua 'print' function) any values on the stack
*/
static void l_print(lua_State *L)
{
    int n = lua_gettop(L);
    if (n > 0)
    { // any result to be printed?
        luaL_checkstack(L, LUA_MINSTACK, "too many results to print");
        lua_getglobal(L, "print");
        lua_insert(L, 1);
        if (lua_pcall(L, n, 0, 0) != LUA_OK)
        {
            fprintf(stderr, "error calling 'print' (%s)\n",
                    lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
}

/*
** Do the REPL: repeatedly read (load) a line, evaluate (call) it, and
** print any results.
*/
static void interact(lua_State *L)
{
    int status;
    lua_initreadline(L);
    while ((status = loadline(L)) != -1)
    {
        if (status == LUA_OK)
            status = lua_pcall(L, 0, LUA_MULTRET, lua_gettop(L));
        if (status == LUA_OK)
            l_print(L);
        else
        {
            fprintf(stderr, "%s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
}

#define NUM_ARGS 4
// Parsed command line arguments
struct command_line_args
{
    unsigned int timeout;
    unsigned int verbose;
    const char *trace;
    const char *logfile;
};

// Main body of interpreter, called by main via lua_pcall.  Arguments
// have already been processed and signal handling is already set up.
// Receives a 'struct command_line_args' as userdata from the Lua stack.
static int pmain(lua_State *L)
{
    struct command_line_args *args = lua_touserdata(L, 1);
    init_lua(L);

    // Expose the arguments in the 'arg' table.  Note that this is
    // different from the 'arg' table you get in the regular Lua.
    lua_createtable(L, 0, NUM_ARGS);

    lua_pushinteger(L, args->timeout);
    lua_setfield(L, -2, "timeout");

    lua_pushinteger(L, args->verbose);
    lua_setfield(L, -2, "verbose");

    if (args->trace)
    {
        lua_pushstring(L, args->trace);
    }
    else
    {
        lua_pushboolean(L, 0);
    }
    lua_setfield(L, -2, "trace");

    if (args->logfile)
    {
        // int logfd = open(args->logfile, O_CREAT | O_WRONLY);
        // dup2(logfd, STDERR_FILENO);
        FILE *log = fopen(args->logfile, "w");
        dup2(fileno(log), STDERR_FILENO);
    }

    lua_setglobal(L, "arg");

    // If we have a trace, execute it, else offer an interactive
    // read-eval-print loop.
    if (args->trace)
    {
        execute_trace(L, args->trace);
    }
    else
    {
        interact(L);
    }

    return 0;
}

// Functions below this point run outside any lua_pcall frame and should
// not touch any Lua state object (except for a tiny bit of code in 'main').

// Command line parsing functions and data
static const struct argp_option command_line_options[] = {
    {"timeout", 't', "SECONDS", 0,
     "How long to allow the trace to run (default: 60s)."
     "  Ignored when running interactively.",
     0},
    {"verbose", 'v', 0, 0,
     "Describe progress of the trace (repeat for even more detail)", 0},
    {"logfile", 'l', "LOG", 0, "Writes standard error to logfile", 0},
    {0, 0, 0, 0, 0, 0}};

static int command_line_parse_1(int key, char *arg, struct argp_state *state)
{
    struct command_line_args *pargs = state->input;
    switch (key)
    {
    case 't':
    {
        char *endp;
        unsigned long val = strtoul(arg, &endp, 10);
        // Not necessary to check errno here because we can safely assume
        // ULONG_MAX > UINT_MAX.
        if (val > UINT_MAX || endp == arg || *endp)
        {
            argp_error(state, "invalid timeout value '%s'", arg);
        }
        pargs->timeout = (unsigned int)val;
        return 0;
    }
    case 'v':
        pargs->verbose += 1;
        if (pargs->verbose == 0)
        {
            argp_error(state, "cannot be that verbose");
        }
        return 0;
    case 'l':
        pargs->logfile = arg;
        return 0;
    case ARGP_KEY_ARG:
        if (pargs->trace)
        {
            argp_error(state, "can only run one trace per invocation");
        }
        pargs->trace = arg;
        return 0;
    case ARGP_KEY_END:
        if (pargs->trace == NULL)
        {
            // If we're running an interactive interpreter, turn off
            // the timeout.  Nobody wants their REPL to bomb out in the
            // middle of typing a line.
            if (isatty(STDIN_FILENO))
            {
                pargs->timeout = 0;
            }
            else
            {
                // So we have something to put in the Lua 'arg.trace' table.
                pargs->trace = "/dev/stdin";
            }
        }
        return 0;
    default:
        return ARGP_ERR_UNKNOWN;
    }
}

static const struct argp command_line_spec = {
    command_line_options,
    command_line_parse_1,
    "[TRACE]",
    "\nTests your implementation of sfs-disk.c against a test trace.\n"
    "If no trace is given, starts an interactive Lua session.\n"
    "\n"
    "Options:",
    NULL,
    NULL,
    NULL};

static void command_line_parse(struct command_line_args *args, int argc,
                               char **argv)
{
    // Set defaults
    args->timeout = 60;   // one minute
    args->verbose = 0;    // quiet
    args->trace = NULL;   // backstop
    args->logfile = NULL; // backstop

    int err = argp_parse(&command_line_spec, argc, argv, 0, 0, args);
    if (err)
    {
        fprintf(stderr, "argp_parse: %s\n", strerror(err));
        exit(1);
    }
}

// Multithreaded programs don't play nice with signals; we have all
// the usual headaches from the kernel faking a function call at an
// arbitrary point in the program, and on top of that, that function
// call could happen on an arbitrary _thread_.  My preferred way to
// cut the concurrency potential down to the point where I can
// actually reason about it, is to block almost all the signals at the
// beginning of the program, and consume the external signals we
// actually care about on a dedicated signal-handling thread, which
// consumes them with sigtimedwait(2), instead of actually allowing
// the signals to be delivered.

// The sole purpose of this function is to be sa_handler, because
// we need to make sure that the signals we care about are not
// set to be ignored.  It will never actually be called.
static void dummy_signal_handler(int unused)
{
    // does nothing
}

/// Arguments to the signal-handling thread.
struct signal_thread_args
{
    sigset_t exit_signals;
    unsigned int test_timeout;
};

/// The signal-handling thread procedure.  The sole purpose of this
/// thread is to call exit() upon receipt of any signal that should
/// terminate the program, yanking the rug out from under the Lua
/// threads.  We do this instead of allowing the signal to terminate
/// the process because it may be marginally more reliable, because it
/// gives us a global timeout for free, and because in the future I'd
/// like to give the Lua threads a chance to exit cleanly if I can
/// figure out a sane way to do that.  (I do not trust Lanes'
/// cancellation mechanism, I think it might be playing fast and loose
/// with the rules for pthreads.)
static void *signal_threadproc(void *argp)
{
    struct signal_thread_args *args = argp;
    pthread_detach(pthread_self());

    int sig;
    struct timespec timeout;
    struct timespec *timeoutp;
    if (args->test_timeout)
    {
        timeout.tv_sec = args->test_timeout;
        timeout.tv_nsec = 0;
        timeoutp = &timeout;
    }
    else
    {
        timeoutp = NULL;
    }

    // This loop is needed because ptrace-generated SIGTRAPs will
    // cause sigtimedwait to return -1/EINTR.  Without it, the program
    // would crash immediately upon continuing from a gdb breakpoint.
    do
    {
        sig = sigtimedwait(&args->exit_signals, NULL, timeoutp);
    } while (sig == -1 && errno == EINTR);

    if (sig == -1 && errno == EAGAIN)
    {
        fprintf(stderr, "Test timeout (%us) has expired.", args->test_timeout);
    }
    else if (sig == -1)
    {
        fprintf(stderr, "sigtimedwait: %s.", strerror(errno));
    }
    else
    {
        fprintf(stderr, "Received signal (%s).", strsignal(sig));
    }
    fputs("  Abandoning test.\n", stderr);

    exit(19);
}

/// Set up signal handling.  Installs signal handlers, adjusts the
/// signal mask, and spawns the signal-handling thread.  Must be
/// called before the first call to pthread_create and before the
/// first call to any Lua API function.
///
/// The 'test_timeout' argument is how long, in seconds, to allow
/// the test to run before giving up on it.
///
/// If any error occurs during setup, the process will be terminated.
static void init_signals(unsigned int test_timeout)
{
    // We must not block any of the _synchronous_ signals, nor any
    // of the blockable signals that stop the entire process.
    static const int signals_left_unblocked[] = {
        SIGABRT, // abort()
        SIGBUS,  // "Bus error" variant of memory protection violation
        SIGFPE,  // Integer division by zero, etc.
        SIGILL,  // Invalid or privileged machine instruction
        SIGSEGV, // "Segmentation fault" variant of memory protection violation
        SIGSTKFLT, // "Stack fault on coprocessor - unused"
        SIGSYS,    // Bad system call
        SIGTRAP,   // Debugger breakpoint in program not attached to debugger

        SIGTSTP, // control-Z
        SIGTTIN, // terminal input by background process
        SIGTTOU, // terminal output by background process
        0        // end of list
    };

    struct sigaction sa;
    sa.sa_handler = dummy_signal_handler;
    sa.sa_flags = SA_RESTART;
    sigfillset(&sa.sa_mask);
    for (int i = 0; signals_left_unblocked[i]; i++)
    {
        sigdelset(&sa.sa_mask, signals_left_unblocked[i]);
    }

    if (pthread_sigmask(SIG_BLOCK, &sa.sa_mask, NULL))
    {
        perror("pthread_sigmask");
        exit(1);
    }

    // The signals we actually want to respond to with custom code are
    // those that indicate the program should shut down promptly but
    // cleanly because of some external event (SIGTERM, etc.)
    static const int clean_shutdown_signals[] = {
        SIGHUP,  // Disconnected from controlling terminal
        SIGINT,  // User pressed control-C
        SIGPWR,  // Imminent power failure
        SIGTERM, // default action of `kill` command
        SIGXCPU, // CPU time limit exceeded
        0        // end of list
    };

    struct signal_thread_args *args = malloc(sizeof(struct signal_thread_args));
    if (!args)
    {
        perror("malloc");
        exit(1);
    }
    args->test_timeout = test_timeout;
    sigemptyset(&args->exit_signals);
    for (int i = 0; clean_shutdown_signals[i]; i++)
    {
        sigaddset(&args->exit_signals, clean_shutdown_signals[i]);
        if (sigaction(clean_shutdown_signals[i], &sa, NULL))
        {
            perror("sigaction");
            exit(1);
        }
    }

    pthread_t t;
    int err = pthread_create(&t, NULL, signal_threadproc, args);
    if (err)
    {
        fprintf(stderr, "pthread_create: %s\n", strerror(err));
        exit(1);
    }
}

int sfs_thread_create_internal(pthread_t *thread, const pthread_attr_t *attr,
                               void *(*start_routine)(void *), void *arg)
{

    return __ctThreadCreateActual(thread, attr, start_routine, arg);
}

int ct_orig_main(int argc, char **argv)
{
    struct command_line_args args;
    command_line_parse(&args, argc, argv);
    init_signals(args.timeout);

    lua_State *L = luaL_newstate();
    if (!L)
    {
        perror("luaL_newstate");
        return 1;
    }
    lua_gc(L, LUA_GCSTOP);           // disable GC during library initialization
    lua_pushcfunction(L, &pmain);    // going to call 'pmain' in protected mode
    lua_pushlightuserdata(L, &args); // sole argument 'args'
    int status = lua_pcall(L, 1, 0, 0); // one arg, nothing returned
    if (status != LUA_OK)
    {
        const char *msg = lua_tostring(L, -1);
        fprintf(stderr, "%s: %s\n", argv[0], msg);
        lua_pop(L, 1);
    }
    lua_close(L);
    return (status == LUA_OK) ? 0 : 1;
}
