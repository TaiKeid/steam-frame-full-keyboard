// Keeps the folder Steam's streaming client loads Full Keyboard's library from
// (stream/openvr_passthrough.c).
//
// Steam can only be told about the library when it starts: the SDL_OPENVR_LIBRARY variable has to be in
// its environment. A drop-in for Steam's user service (src/stream_support.cpp) runs `start` here before
// every Steam start and reads the environment file this writes. Everything lives in the user's runtime
// folder, which the system empties at shutdown:
//
//   steam.env                 the variable, written only when the library passed its check
//   framekeyboard-stream.so   a copy of the library, so removing the keyboard cannot take it from a
//                             running Steam
//   openvr.so                 what the variable names: a link to the copy, or to Steam's own library
//                             while Streaming support is off
//   libopenvr_api.so          a link to Steam's own library, which the copy depends on: its search
//                             path is its own folder, so it binds Steam's copy with or without the
//                             streaming client's LD_LIBRARY_PATH
//   off                       tells a library that is already loaded to stay idle
//   failed                    the last `start` failed, so this Steam runs without the library;
//                             Settings says so instead of asking for a restart
//
// Steam's start must never depend on this program. The drop-in ignores its exit status, bounds its time
// and removes steam.env before it runs, so a missing or failing gate leaves Steam exactly as it comes.
//
//   framekeyboard-stream-gate start    before Steam starts: prepare, then write steam.env
//   framekeyboard-stream-gate prepare  Streaming support was turned on while Steam already has the
//   variable framekeyboard-stream-gate stock    turned off: point the link at Steam's own library
//   framekeyboard-stream-gate check F R  whether F loads and binds R, Steam's library (used by the two
//                                        above)
//
// Options, for tests: --runtime DIR, --library FILE, --steam DIR.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LIBRARY_NAME "framekeyboard-stream.so"
#define LINK_NAME "openvr.so"
#define REAL_LIBRARY "libopenvr_api.so"
#define VARIABLE "SDL_OPENVR_LIBRARY"

enum { kCheckSeconds = 3 };

static char runtime_dir[PATH_MAX];
static char library[PATH_MAX];
static char steam_dir[PATH_MAX];
static char self[PATH_MAX];

static bool join(char* out, const char* directory, const char* name) {
    return snprintf(out, PATH_MAX, "%s/%s", directory, name) < PATH_MAX;
}

// The environment file is not quoted, so the path it carries has to be plain.
static bool plain_path(const char* path) {
    if (path[0] != '/') {
        return false;
    }
    for (const char* c = path; *c; ++c) {
        const bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                        (*c >= '0' && *c <= '9') || *c == '/' || *c == '.' || *c == '_' || *c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool defaults(void) {
    const ssize_t length = readlink("/proc/self/exe", self, sizeof self - 1);
    if (length <= 0) {
        return false;
    }
    self[length] = '\0';
    if (runtime_dir[0] == '\0' &&
        snprintf(runtime_dir, sizeof runtime_dir, "/run/user/%u/framekeyboard-stream",
                 (unsigned)getuid()) >= (int)sizeof runtime_dir) {
        return false;
    }
    if (library[0] == '\0') {
        // The library is installed beside this program.
        char directory[PATH_MAX];
        snprintf(directory, sizeof directory, "%s", self);
        char* slash = strrchr(directory, '/');
        if (!slash) {
            return false;
        }
        *slash = '\0';
        if (!join(library, directory, LIBRARY_NAME)) {
            return false;
        }
    }
    if (steam_dir[0] == '\0') {
        const char* home = getenv("HOME");
        if (!home || home[0] != '/' ||
            snprintf(steam_dir, sizeof steam_dir, "%s/.local/share/Steam/steamrtarm64", home) >=
                (int)sizeof steam_dir) {
            return false;
        }
    }
    return true;
}

// The folder is ours alone: a real directory of this user that nobody else can enter.
static bool own_directory(void) {
    if (mkdir(runtime_dir, 0700) != 0 && errno != EEXIST) {
        return false;
    }
    struct stat info;
    return lstat(runtime_dir, &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == getuid() &&
           chmod(runtime_dir, 0700) == 0;
}

static bool write_all(int fd, const char* data, size_t length) {
    while (length != 0) {
        const ssize_t written = write(fd, data, length);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        data += written;
        length -= (size_t)written;
    }
    return true;
}

// Writes beside the target and renames over it, so a reader never sees half a file.
static bool replace_file(const char* name, const char* source_path, const char* text, mode_t mode) {
    char target[PATH_MAX], temporary[PATH_MAX], hidden[NAME_MAX];
    snprintf(hidden, sizeof hidden, ".%s.new", name);
    if (!join(target, runtime_dir, name) || !join(temporary, runtime_dir, hidden)) {
        return false;
    }
    unlink(temporary);
    const int out = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode);
    if (out < 0) {
        return false;
    }
    bool ok = true;
    if (source_path) {
        const int in = open(source_path, O_RDONLY | O_CLOEXEC);
        ok = in >= 0;
        char buffer[65536];
        while (ok) {
            const ssize_t count = read(in, buffer, sizeof buffer);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                ok = count == 0;
                break;
            }
            ok = write_all(out, buffer, (size_t)count);
        }
        if (in >= 0) {
            close(in);
        }
    } else {
        ok = write_all(out, text, strlen(text));
    }
    ok = ok && fchmod(out, mode) == 0 && fsync(out) == 0;
    ok = close(out) == 0 && ok;
    ok = ok && rename(temporary, target) == 0;
    if (!ok) {
        unlink(temporary);
    }
    return ok;
}

static bool replace_link_named(const char* name, const char* destination) {
    char target[PATH_MAX], temporary[PATH_MAX], hidden[NAME_MAX];
    snprintf(hidden, sizeof hidden, ".%s.new", name);
    if (!join(target, runtime_dir, name) || !join(temporary, runtime_dir, hidden)) {
        return false;
    }
    unlink(temporary);
    if (symlink(destination, temporary) != 0 || rename(temporary, target) != 0) {
        unlink(temporary);
        return false;
    }
    return true;
}

static bool replace_link(const char* destination) {
    return replace_link_named(LINK_NAME, destination);
}

static void remove_named(const char* name) {
    char path[PATH_MAX];
    if (join(path, runtime_dir, name)) {
        unlink(path);
    }
}

// In a process of its own, with no library path of the environment's: the library has to load, offer
// the call it wraps and reach the real library's calls through its dependency, and that dependency has
// to be `real`, Steam's own copy, found through the library's own search path. The streaming client
// searches its LD_LIBRARY_PATH first, which today names the same folder; were that to change, the
// library would still bind Steam's copy and not another one on the system.
static int check(const char* file, const char* real) {
    void* handle = dlopen(file, RTLD_NOW | RTLD_LOCAL);
    if (!handle || !dlsym(handle, "VR_GetGenericInterface")) {
        return 1;
    }
    void* call = dlsym(handle, "VR_InitInternal");
    Dl_info info;
    char bound[PATH_MAX], expected[PATH_MAX];
    return call && dladdr(call, &info) && info.dli_fname && realpath(info.dli_fname, bound) &&
                   realpath(real, expected) && strcmp(bound, expected) == 0
               ? 0
               : 1;
}

static bool loads(const char* file) {
    char real[PATH_MAX];
    if (!join(real, steam_dir, REAL_LIBRARY) || access(real, R_OK) != 0) {
        return false;
    }
    const pid_t child = fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        unsetenv("LD_LIBRARY_PATH");
        execl(self, self, "check", file, real, (char*)NULL);
        _exit(127);
    }
    struct timespec started;
    clock_gettime(CLOCK_MONOTONIC, &started);
    for (;;) {
        int status = 0;
        const pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (done < 0 && errno != EINTR) {
            return false;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - started.tv_sec >= kCheckSeconds) {
            kill(child, SIGKILL);
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
            return false;
        }
        const struct timespec pause = {0, 10 * 1000 * 1000};
        nanosleep(&pause, NULL);
    }
}

static bool stock(void) {
    char real[PATH_MAX];
    remove_named("steam.env");
    if (!own_directory() || !join(real, steam_dir, REAL_LIBRARY)) {
        return false;
    }
    // First the mark a loaded library reads, then the link the next stream follows.
    const bool marked = replace_file("off", NULL, "", 0600);
    return replace_link(real) && marked;
}

static bool prepare(void) {
    char copy[PATH_MAX], real[PATH_MAX];
    if (!own_directory() || !join(copy, runtime_dir, LIBRARY_NAME) ||
        !join(real, steam_dir, REAL_LIBRARY)) {
        return false;
    }
    // The copy's dependency, found beside it, before the copy is checked.
    if (!replace_link_named(REAL_LIBRARY, real) || !replace_file(LIBRARY_NAME, library, NULL, 0755) ||
        !loads(copy)) {
        return false;
    }
    if (!replace_link(LIBRARY_NAME)) {
        return false;
    }
    remove_named("off");
    return true;
}

static bool start(void) {
    char link_path[PATH_MAX], line[PATH_MAX + 64];
    remove_named("steam.env");
    if (!join(link_path, runtime_dir, LINK_NAME) || !plain_path(link_path) || !prepare()) {
        return false;
    }
    snprintf(line, sizeof line, VARIABLE "=%s\n", link_path);
    return replace_file("steam.env", NULL, line, 0600);
}

int main(int argc, char** argv) {
    if (argc == 4 && strcmp(argv[1], "check") == 0) {
        return check(argv[2], argv[3]);
    }
    if (argc < 2) {
        return 2;
    }
    for (int i = 2; i + 1 < argc; i += 2) {
        char* into = strcmp(argv[i], "--runtime") == 0   ? runtime_dir
                     : strcmp(argv[i], "--library") == 0 ? library
                     : strcmp(argv[i], "--steam") == 0   ? steam_dir
                                                         : NULL;
        if (!into || snprintf(into, PATH_MAX, "%s", argv[i + 1]) >= PATH_MAX) {
            return 2;
        }
    }
    if (argc % 2 != 0 || !defaults()) {
        return 2;
    }
    bool ok;
    if (strcmp(argv[1], "start") == 0) {
        ok = start();
    } else if (strcmp(argv[1], "prepare") == 0) {
        ok = prepare();
    } else if (strcmp(argv[1], "stock") == 0) {
        return stock() ? 0 : 1;
    } else {
        return 2;
    }
    if (!ok) {
        // Whatever went wrong, a Steam that already has the variable must find its own library there.
        stock();
        // Only in a folder that is ours alone: a refused folder gets nothing.
        if (strcmp(argv[1], "start") == 0 && own_directory()) {
            replace_file("failed", NULL, "", 0600);
        }
    } else {
        remove_named("failed");
    }
    return ok ? 0 : 1;
}
