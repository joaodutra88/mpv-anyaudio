// libmpv-2.dll shim for Nuvio Desktop (Windows).
//
// Why it exists: Nuvio creates libmpv with "config=no" and "input-vo-keyboard=no",
// so the user's mpv.conf / input.conf / scripts are never read. This shim exports
// exactly the symbols player_bridge.dll resolves, forwards everything to the real
// libmpv (real\ folder) and applies nuvio-mpv.conf right before mpv_initialize -- after
// the options the app sets, so it wins when a key is repeated.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <fstream>
#include <string>
#include <vector>

typedef void mpv_handle;

typedef mpv_handle *(*fn_create)();
typedef int (*fn_initialize)(mpv_handle *);
typedef void (*fn_terminate)(mpv_handle *);
typedef int (*fn_set_option)(mpv_handle *, const char *, int, void *);
typedef int (*fn_set_option_string)(mpv_handle *, const char *, const char *);
typedef int (*fn_set_property)(mpv_handle *, const char *, int, void *);
typedef int (*fn_set_property_string)(mpv_handle *, const char *, const char *);
typedef int (*fn_get_property)(mpv_handle *, const char *, int, void *);
typedef int (*fn_command)(mpv_handle *, const char **);
typedef const char *(*fn_error_string)(int);
typedef void (*fn_free)(void *);
typedef void *(*fn_wait_event)(mpv_handle *, double);
typedef void (*fn_wakeup)(mpv_handle *);

static HMODULE g_real = nullptr;
static char g_dir[MAX_PATH] = {0};
static char g_log[MAX_PATH] = {0};

static fn_create p_create;
static fn_initialize p_initialize;
static fn_terminate p_terminate;
static fn_set_option p_set_option;
static fn_set_option_string p_set_option_string;
static fn_set_property p_set_property;
static fn_set_property_string p_set_property_string;
static fn_get_property p_get_property;
static fn_command p_command;
static fn_error_string p_error_string;
static fn_free p_free;
static fn_wait_event p_wait_event;
static fn_wakeup p_wakeup;
static int (*p_command_string)(mpv_handle *, const char *);
static void logLine(const char *fmt, ...);

// fired: the shortcut already fired for this press; it only re-arms when the key is released.
struct KeyBinding {
    int key;
    bool ctrl;
    bool fired;
    std::string command;
};

struct KeyboardWorker {
    std::atomic_bool stop{false};
    std::thread thread;
    DWORD tid = 0;              // to wake GetMessage on stop
    HHOOK hook = nullptr;       // null = fell back to polling (key is not swallowed)
    int swallowKey = 0;         // key whose keyup must also be swallowed
    // Modifiers seen in the hook's own queue: GetAsyncKeyState for CTRL may not
    // have updated yet when the key arrives, and the press was lost.
    bool ctrlDown = false, shiftDown = false, altDown = false, winDown = false;
    mpv_handle *h = nullptr;
    std::vector<KeyBinding> bindings;
};
static std::mutex keyboardMutex;
static std::unordered_map<mpv_handle *, std::unique_ptr<KeyboardWorker>> keyboards;
// The hook runs on the thread that installed it; one player at a time in practice (Nuvio opens one).
static std::mutex hookWorkerMutex;
static KeyboardWorker *g_hookWorker = nullptr;

static void fireCommand(KeyboardWorker *worker, KeyBinding &binding) {
    if (!worker->h) return;
    int idle = 1;
    if (p_get_property(worker->h, "idle-active", 3, &idle) >= 0 && !idle) {
        int rc = p_command_string(worker->h, binding.command.c_str());
        if (rc < 0) {
            logLine("shortcut failed (%s): %s",
                    p_error_string ? p_error_string(rc) : "?", binding.command.c_str());
            const char *notice[] = {"show-text", "mpv shortcut failed", nullptr};
            p_command(worker->h, notice);
        }
    }
}

// Global hook: same detection as polling, plus the ability to keep the key from reaching
// the app UI (that is what opened the WebView search on F3). Only swallows with the player
// window focused and the exact combination - otherwise passes it through untouched.
static LRESULT CALLBACK keyboardHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION || code == HC_NOREMOVE) {
        KeyboardWorker *worker;
        {
            std::lock_guard<std::mutex> lock(hookWorkerMutex);
            worker = g_hookWorker;
        }
        if (worker) {
            const KBDLLHOOKSTRUCT *kb = (const KBDLLHOOKSTRUCT *)lParam;
            bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
            bool up = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
            if (down || up) {
                switch (kb->vkCode) {
                    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
                        worker->ctrlDown = down; break;
                    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
                        worker->shiftDown = down; break;
                    case VK_MENU: case VK_LMENU: case VK_RMENU:
                        worker->altDown = down; break;
                    case VK_LWIN: case VK_RWIN:
                        worker->winDown = down; break;
                    default: break;
                }
                for (auto &binding : worker->bindings) {
                    if (binding.key != (int)kb->vkCode) continue;
                    if (up) {
                        binding.fired = false;
                        if (worker->swallowKey == binding.key) {
                            worker->swallowKey = 0;
                            return 1;
                        }
                        break;
                    }
                    DWORD foregroundPid = 0;
                    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
                    bool foreground = foregroundPid == GetCurrentProcessId();
                    // Tracked by the hook itself, with the async state as a safety net.
                    bool ctrl = worker->ctrlDown || (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                    bool other = worker->shiftDown || worker->altDown || worker->winDown ||
                                 (GetAsyncKeyState(VK_MENU) & 0x8000) ||
                                 (GetAsyncKeyState(VK_SHIFT) & 0x8000) ||
                                 (GetAsyncKeyState(VK_LWIN) & 0x8000) ||
                                 (GetAsyncKeyState(VK_RWIN) & 0x8000);
                    // Not focused: mark as fired so it doesn't trigger when focus returns, and
                    // don't swallow (the key belongs to whoever is in front).
                    if (!foreground) {
                        binding.fired = true;
                        break;
                    }
                    if (other || ctrl != binding.ctrl) break;  // different combination: not ours
                    if (binding.fired) return 1;               // auto-repeat of the same press
                    binding.fired = true;
                    worker->swallowKey = binding.key;
                    fireCommand(worker, binding);
                    return 1;  // never reaches the Nuvio UI
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

static void stopKeyboard(mpv_handle *h) {
    std::unique_ptr<KeyboardWorker> worker;
    {
        std::lock_guard<std::mutex> lock(keyboardMutex);
        auto found = keyboards.find(h);
        if (found == keyboards.end()) return;
        worker = std::move(found->second);
        keyboards.erase(found);
    }
    worker->stop.store(true);
    // GetMessage blocks: without WM_QUIT the thread never leaves the loop and join hangs.
    for (int i = 0; i < 200 && !worker->tid; i++) Sleep(5);
    if (worker->tid) PostThreadMessageW(worker->tid, WM_QUIT, 0, 0);
    if (worker->thread.joinable()) worker->thread.join();
}

static void startKeyboard(mpv_handle *h) {
    if (!h || !p_command_string) return;
    // fired: the shortcut already fired for this press; it only re-arms when the key is released.
    std::vector<KeyBinding> bindings;
    std::ifstream file(std::string(g_dir) + "\\input.conf");
    std::string line;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                                 line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();  // input.conf is CRLF: the \r would end up in the mpv command
        }
        auto split = line.find_first_of(" \t");
        if (split == std::string::npos) continue;
        auto name = line.substr(0, split);
        int key = 0;
        // CTRL+<0-9|A-Z> and F1..F12. Bare keys without CTRL are excluded on purpose:
        // focus is process-wide, so typing in the Nuvio search would trigger shortcuts.
        bool ctrl = name.rfind("CTRL+", 0) == 0;
        std::string base = ctrl ? name.substr(5) : name;
        if (ctrl && base.size() == 1 &&
            ((base[0] >= '0' && base[0] <= '9') || (base[0] >= 'A' && base[0] <= 'Z'))) {
            key = base[0];  // VK for digit/letter == uppercase ASCII
        } else if (!ctrl && base.size() >= 2 && base[0] == 'F' &&
                   base.find_first_not_of("0123456789", 1) == std::string::npos) {
            int n = atoi(base.c_str() + 1);
            if (n >= 1 && n <= 12) key = VK_F1 + n - 1;
        }
        auto command = line.substr(split + 1);
        while (!command.empty() && (command.front() == ' ' || command.front() == '\t')) {
            command.erase(command.begin());
        }
        if (key && !command.empty()) bindings.push_back({key, ctrl, false, command});
    }
    if (bindings.empty()) {
        logLine("keyboard: no shortcuts in %s\\input.conf", g_dir);
        return;
    }
    stopKeyboard(h);  // reused handle: never overwrite a thread that is still alive
    // The thread runs outside the loader's control: pin the module so a host
    // FreeLibrary doesn't unload code that is still executing.
    static std::atomic_bool pinned{false};
    if (!pinned.exchange(true)) {
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           (LPCSTR)&startKeyboard, &self);
    }
    // The worker enters the map (under lock) BEFORE the thread exists: if stopKeyboard
    // ran in the window between creating the thread and registering it, the handle would
    // be destroyed while the thread was still using h.
    std::lock_guard<std::mutex> lock(keyboardMutex);
    auto &slot = keyboards[h];
    slot = std::make_unique<KeyboardWorker>();
    auto state = slot.get();
    state->h = h;
    state->bindings = std::move(bindings);
    slot->thread = std::thread([state]() mutable {
        // Message queue before publishing the tid: without it the stop's PostThreadMessage
        // fails (thread has no queue yet) and GetMessage stays stuck forever.
        MSG msg;
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        state->tid = GetCurrentThreadId();
        {
            std::lock_guard<std::mutex> lock(hookWorkerMutex);
            g_hookWorker = state;
        }
        state->hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHookProc, nullptr, 0);
        if (state->hook) {
            // The hook is global and runs on this thread: it needs a live message loop.
            while (!state->stop.load() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            }
            UnhookWindowsHookEx(state->hook);
            state->hook = nullptr;
        } else {
            // Without the hook the shim keeps working, it just can't swallow the key
            // (the app UI sees the shortcut too). 20ms polling, as before.
            logLine("keyboard hook failed (win32=%lu): falling back to polling", GetLastError());
            while (!state->stop.load()) {
                DWORD foregroundPid = 0;
                GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
                bool foreground = foregroundPid == GetCurrentProcessId();
                bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                bool other = (GetAsyncKeyState(VK_MENU) & 0x8000) ||
                             (GetAsyncKeyState(VK_SHIFT) & 0x8000) ||
                             (GetAsyncKeyState(VK_LWIN) & 0x8000) ||
                             (GetAsyncKeyState(VK_RWIN) & 0x8000);
                for (auto &binding : state->bindings) {
                    bool down = (GetAsyncKeyState(binding.key) & 0x8000) != 0;
                    if (!down) {
                        binding.fired = false;  // re-arm only on release: no auto-repeat
                        continue;
                    }
                    // Not focused: consume the press so it doesn't fire when focus returns.
                    if (!foreground) { binding.fired = true; continue; }
                    if (binding.fired) continue;
                    // Modifier not settled yet (e.g. CTRL arrives on the next poll):
                    // don't consume the press, retry while the key is still down.
                    if (other || ctrl != binding.ctrl) continue;
                    binding.fired = true;
                    fireCommand(state, binding);
                }
                Sleep(20);
            }
        }
        {
            std::lock_guard<std::mutex> lock(hookWorkerMutex);
            if (g_hookWorker == state) g_hookWorker = nullptr;
        }
    });
}

static void logLine(const char *fmt, ...) {
    if (!g_log[0]) return;
    // The log only serves the latest diagnosis: above 256 KB it restarts instead of growing forever.
    struct _stat64 info;
    FILE *f = fopen(g_log, _stat64(g_log, &info) == 0 && info.st_size > 256 * 1024 ? "w" : "a");
    if (!f) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

static void resolveDir(HMODULE self) {
    GetModuleFileNameA(self, g_dir, MAX_PATH);
    char *slash = strrchr(g_dir, '\\');
    if (slash) *slash = 0;
    _snprintf_s(g_log, sizeof(g_log), _TRUNCATE, "%s\\shim.log", g_dir);
}

static bool loadReal() {
    static bool tried = false;
    if (tried) return g_real != nullptr;
    tried = true;

    char path[MAX_PATH];
    const char *env = getenv("NUVIO_LIBMPV_REAL");
    if (env && *env) {
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s", env);
    } else {
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\real\\libmpv-2.dll", g_dir);
    }

    g_real = LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_real) {
        logLine("ERROR: failed to load %s (win32=%lu)", path, GetLastError());
        return false;
    }

    p_create = (fn_create)GetProcAddress(g_real, "mpv_create");
    p_initialize = (fn_initialize)GetProcAddress(g_real, "mpv_initialize");
    p_terminate = (fn_terminate)GetProcAddress(g_real, "mpv_terminate_destroy");
    p_set_option = (fn_set_option)GetProcAddress(g_real, "mpv_set_option");
    p_set_option_string = (fn_set_option_string)GetProcAddress(g_real, "mpv_set_option_string");
    p_set_property = (fn_set_property)GetProcAddress(g_real, "mpv_set_property");
    p_set_property_string = (fn_set_property_string)GetProcAddress(g_real, "mpv_set_property_string");
    p_get_property = (fn_get_property)GetProcAddress(g_real, "mpv_get_property");
    p_command = (fn_command)GetProcAddress(g_real, "mpv_command");
    p_error_string = (fn_error_string)GetProcAddress(g_real, "mpv_error_string");
    p_free = (fn_free)GetProcAddress(g_real, "mpv_free");
    p_wait_event = (fn_wait_event)GetProcAddress(g_real, "mpv_wait_event");
    p_wakeup = (fn_wakeup)GetProcAddress(g_real, "mpv_wakeup");
    p_command_string = (int (*)(mpv_handle *, const char *))GetProcAddress(g_real, "mpv_command_string");

    // mpv_command_string is only needed for shortcuts: without it the keyboard is off, passthrough keeps working.
    if (!p_command_string) logLine("warning: no mpv_command_string, shortcuts disabled");
    if (!p_create || !p_initialize || !p_terminate || !p_set_option || !p_set_option_string ||
        !p_set_property || !p_set_property_string || !p_get_property || !p_command ||
        !p_error_string || !p_free || !p_wait_event || !p_wakeup) {
        logLine("ERROR: %s lacks the expected symbols", path);
        FreeLibrary(g_real);
        g_real = nullptr;  // otherwise the next call sees g_real != null and uses a null pointer
        return false;
    }
    logLine("real libmpv: %s", path);
    return true;
}

static void applyConf(mpv_handle *h) {
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\nuvio-mpv.conf", g_dir);

    FILE *f = fopen(path, "r");
    if (!f) {
        logLine("conf missing: %s", path);
        return;
    }

    char line[4096];
    int applied = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 0 || *p == '#' || *p == ';') continue;

        char *eq = strchr(p, '=');
        if (!eq) {
            logLine("ignored (no '='): %s", p);
            continue;
        }
        *eq = 0;
        char *name = p;
        char *value = eq + 1;
        char *end = name + strlen(name);
        while (end > name && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;

        int rc = p_set_option_string(h, name, value);
        if (rc < 0) {
            logLine("FAILED %s=%s (%s)", name, value, p_error_string ? p_error_string(rc) : "?");
        } else {
            applied++;
        }
    }
    fclose(f);
    logLine("applied %d options from %s", applied, path);
}

extern "C" __declspec(dllexport) mpv_handle *mpv_create() {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&mpv_create, &self);
    resolveDir(self);
    if (!loadReal()) return nullptr;
    return p_create();
}

extern "C" __declspec(dllexport) int mpv_initialize(mpv_handle *h) {
    if (!loadReal()) return -1;
    if (h) applyConf(h);
    int rc = p_initialize(h);
    // The host is not C++: an exception crossing the extern "C" boundary aborts Nuvio.
    // Without the keyboard the player keeps playing, so the failure is swallowed on purpose.
    if (rc >= 0) {
        try {
            startKeyboard(h);
        } catch (const std::exception &e) {
            logLine("keyboard disabled (%s)", e.what());
        } catch (...) {
            logLine("keyboard disabled (unknown exception)");
        }
    }
    return rc;
}

extern "C" __declspec(dllexport) void mpv_terminate_destroy(mpv_handle *h) {
    stopKeyboard(h);
    if (loadReal()) p_terminate(h);
}

extern "C" __declspec(dllexport) int mpv_set_option(mpv_handle *h, const char *name, int format, void *data) {
    if (!loadReal()) return -1;
    return p_set_option(h, name, format, data);
}

extern "C" __declspec(dllexport) int mpv_set_option_string(mpv_handle *h, const char *name, const char *value) {
    if (!loadReal()) return -1;
    return p_set_option_string(h, name, value);
}

extern "C" __declspec(dllexport) int mpv_set_property(mpv_handle *h, const char *name, int format, void *data) {
    if (!loadReal()) return -1;
    return p_set_property(h, name, format, data);
}

extern "C" __declspec(dllexport) int mpv_set_property_string(mpv_handle *h, const char *name, const char *value) {
    if (!loadReal()) return -1;
    return p_set_property_string(h, name, value);
}

extern "C" __declspec(dllexport) int mpv_get_property(mpv_handle *h, const char *name, int format, void *data) {
    if (!loadReal()) return -1;
    return p_get_property(h, name, format, data);
}

extern "C" __declspec(dllexport) int mpv_command(mpv_handle *h, const char **args) {
    if (!loadReal()) return -1;
    return p_command(h, args);
}

extern "C" __declspec(dllexport) const char *mpv_error_string(int error) {
    if (!loadReal()) return "shim: libmpv unavailable";
    return p_error_string(error);
}

extern "C" __declspec(dllexport) void mpv_free(void *data) {
    if (loadReal()) p_free(data);
}

extern "C" __declspec(dllexport) void *mpv_wait_event(mpv_handle *h, double timeout) {
    if (!loadReal()) return nullptr;
    return p_wait_event(h, timeout);
}

extern "C" __declspec(dllexport) void mpv_wakeup(mpv_handle *h) {
    if (loadReal()) p_wakeup(h);
}
