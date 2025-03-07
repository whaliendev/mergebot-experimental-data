#define TRACE_TAG ADB
#include "sysdeps.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <memory>
#include <string>
#include <vector>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#if !defined(_WIN32)
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif
#include "adb.h"
#include "adb_auth.h"
#include "adb_client.h"
#include "adb_io.h"
#include "adb_utils.h"
#include "file_sync_service.h"
#include "services.h"
#include "shell_service.h"
#include "transport.h"
static int install_app(TransportType t, const char* serial, int argc, const char** argv);
static int install_multiple_app(TransportType t, const char* serial, int argc, const char** argv);
static int uninstall_app(TransportType t, const char* serial, int argc, const char** argv);
static int install_app_legacy(TransportType t, const char* serial, int argc, const char** argv);
static int uninstall_app_legacy(TransportType t, const char* serial, int argc, const char** argv);
static auto& gProductOutPath = *new std::string();
extern int gListenAll;
DefaultStandardStreamsCallback DEFAULT_STANDARD_STREAMS_CALLBACK(nullptr, nullptr); static constexpr char BUGZ_OK_PREFIX[] = "OK:"; static constexpr char BUGZ_FAIL_PREFIX[] = "FAIL:";
static std::string product_file(const char *extra) {
    if (gProductOutPath.empty()) {
        fprintf(stderr, "adb: Product directory not specified; "
                "use -p or define ANDROID_PRODUCT_OUT\n");
        exit(1);
    }
    return android::base::StringPrintf("%s%s%s",
                                       gProductOutPath.c_str(), OS_PATH_SEPARATOR_STR, extra);
}
static void help() {
    fprintf(stderr, "%s\n", adb_version().c_str());
    fprintf(stderr,
        " -a                            - directs adb to listen on all interfaces for a connection\n"
        " -d                            - directs command to the only connected USB device\n"
        "                                 returns an error if more than one USB device is present.\n"
        " -e                            - directs command to the only running emulator.\n"
        "                                 returns an error if more than one emulator is running.\n"
        " -s <specific device>          - directs command to the device or emulator with the given\n"
        "                                 serial number or qualifier. Overrides ANDROID_SERIAL\n"
        "                                 environment variable.\n"
        " -p <product name or path>     - simple product name like 'sooner', or\n"
        "                                 a relative/absolute path to a product\n"
        "                                 out directory like 'out/target/product/sooner'.\n"
        "                                 If -p is not specified, the ANDROID_PRODUCT_OUT\n"
        "                                 environment variable is used, which must\n"
        "                                 be an absolute path.\n"
        " -H                            - Name of adb server host (default: localhost)\n"
        " -P                            - Port of adb server (default: 5037)\n"
        " devices [-l]                  - list all connected devices\n"
        "                                 ('-l' will also list device qualifiers)\n"
        " connect <host>[:<port>]       - connect to a device via TCP/IP\n"
        "                                 Port 5555 is used by default if no port number is specified.\n"
        " disconnect [<host>[:<port>]]  - disconnect from a TCP/IP device.\n"
        "                                 Port 5555 is used by default if no port number is specified.\n"
        "                                 Using this command with no additional arguments\n"
        "                                 will disconnect from all connected TCP/IP devices.\n"
        "\n"
        "device commands:\n"
        "  adb push <local>... <remote>\n"
        "                               - copy files/dirs to device\n"
        "  adb pull [-a] <remote>... <local>\n"
        "                               - copy files/dirs from device\n"
        "                                 (-a preserves file timestamp and mode)\n"
        "  adb sync [ <directory> ]     - copy host->device only if changed\n"
        "                                 (-l means list but don't copy)\n"
        "  adb shell [-e escape] [-n] [-Tt] [-x] [command]\n"
        "                               - run remote shell command (interactive shell if no command given)\n"
        "                                 (-e: choose escape character, or \"none\"; default '~')\n"
        "                                 (-n: don't read from stdin)\n"
        "                                 (-T: disable PTY allocation)\n"
        "                                 (-t: force PTY allocation)\n"
        "                                 (-x: disable remote exit codes and stdout/stderr separation)\n"
        "  adb emu <command>            - run emulator console command\n"
        "  adb logcat [ <filter-spec> ] - View device log\n"
        "  adb forward --list           - list all forward socket connections.\n"
        "                                 the format is a list of lines with the following format:\n"
        "                                    <serial> \" \" <local> \" \" <remote> \"\\n\"\n"
        "  adb forward <local> <remote> - forward socket connections\n"
        "                                 forward specs are one of: \n"
        "                                   tcp:<port> (<local> may be \"tcp:0\" to pick any open port)\n"
        "                                   localabstract:<unix domain socket name>\n"
        "                                   localreserved:<unix domain socket name>\n"
        "                                   localfilesystem:<unix domain socket name>\n"
        "                                   dev:<character device name>\n"
        "                                   jdwp:<process pid> (remote only)\n"
        "  adb forward --no-rebind <local> <remote>\n"
        "                               - same as 'adb forward <local> <remote>' but fails\n"
        "                                 if <local> is already forwarded\n"
        "  adb forward --remove <local> - remove a specific forward socket connection\n"
        "  adb forward --remove-all     - remove all forward socket connections\n"
        "  adb reverse --list           - list all reverse socket connections from device\n"
        "  adb reverse <remote> <local> - reverse socket connections\n"
        "                                 reverse specs are one of:\n"
        "                                   tcp:<port> (<remote> may be \"tcp:0\" to pick any open port)\n"
        "                                   localabstract:<unix domain socket name>\n"
        "                                   localreserved:<unix domain socket name>\n"
        "                                   localfilesystem:<unix domain socket name>\n"
        "  adb reverse --no-rebind <remote> <local>\n"
        "                               - same as 'adb reverse <remote> <local>' but fails\n"
        "                                 if <remote> is already reversed.\n"
        "  adb reverse --remove <remote>\n"
        "                               - remove a specific reversed socket connection\n"
        "  adb reverse --remove-all     - remove all reversed socket connections from device\n"
        "  adb jdwp                     - list PIDs of processes hosting a JDWP transport\n"
        "  adb install [-lrtsdg] <file>\n"
        "                               - push this package file to the device and install it\n"
        "                                 (-l: forward lock application)\n"
        "                                 (-r: replace existing application)\n"
        "                                 (-t: allow test packages)\n"
        "                                 (-s: install application on sdcard)\n"
        "                                 (-d: allow version code downgrade (debuggable packages only))\n"
        "                                 (-g: grant all runtime permissions)\n"
        "  adb install-multiple [-lrtsdpg] <file...>\n"
        "                               - push this package file to the device and install it\n"
        "                                 (-l: forward lock application)\n"
        "                                 (-r: replace existing application)\n"
        "                                 (-t: allow test packages)\n"
        "                                 (-s: install application on sdcard)\n"
        "                                 (-d: allow version code downgrade (debuggable packages only))\n"
        "                                 (-p: partial application install)\n"
        "                                 (-g: grant all runtime permissions)\n"
        "  adb uninstall [-k] <package> - remove this app package from the device\n"
        "                                 ('-k' means keep the data and cache directories)\n"
        "  adb bugreport [<zip_file>]   - return all information from the device\n"
        "                                 that should be included in a bug report.\n"
        "\n"
        "  adb backup [-f <file>] [-apk|-noapk] [-obb|-noobb] [-shared|-noshared] [-all] [-system|-nosystem] [<packages...>]\n"
        "                               - write an archive of the device's data to <file>.\n"
        "                                 If no -f option is supplied then the data is written\n"
        "                                 to \"backup.ab\" in the current directory.\n"
        "                                 (-apk|-noapk enable/disable backup of the .apks themselves\n"
        "                                    in the archive; the default is noapk.)\n"
        "                                 (-obb|-noobb enable/disable backup of any installed apk expansion\n"
        "                                    (aka .obb) files associated with each application; the default\n"
        "                                    is noobb.)\n"
        "                                 (-shared|-noshared enable/disable backup of the device's\n"
        "                                    shared storage / SD card contents; the default is noshared.)\n"
        "                                 (-all means to back up all installed applications)\n"
        "                                 (-system|-nosystem toggles whether -all automatically includes\n"
        "                                    system applications; the default is to include system apps)\n"
        "                                 (<packages...> is the list of applications to be backed up.  If\n"
        "                                    the -all or -shared flags are passed, then the package\n"
        "                                    list is optional.  Applications explicitly given on the\n"
        "                                    command line will be included even if -nosystem would\n"
        "                                    ordinarily cause them to be omitted.)\n"
        "\n"
        "  adb restore <file>           - restore device contents from the <file> backup archive\n"
        "\n"
        "  adb disable-verity           - disable dm-verity checking on USERDEBUG builds\n"
        "  adb enable-verity            - re-enable dm-verity checking on USERDEBUG builds\n"
        "  adb keygen <file>            - generate adb public/private key. The private key is stored in <file>,\n"
        "                                 and the public key is stored in <file>.pub. Any existing files\n"
        "                                 are overwritten.\n"
        "  adb help                     - show this help message\n"
        "  adb version                  - show version num\n"
        "\n"
        "scripting:\n"
        "  adb wait-for[-<transport>]-<state>\n"
        "                               - wait for device to be in the given state:\n"
        "                                 device, recovery, sideload, or bootloader\n"
        "                                 Transport is: usb, local or any [default=any]\n"
        "  adb start-server             - ensure that there is a server running\n"
        "  adb kill-server              - kill the server if it is running\n"
        "  adb get-state                - prints: offline | bootloader | device\n"
        "  adb get-serialno             - prints: <serial-number>\n"
        "  adb get-devpath              - prints: <device-path>\n"
        "  adb remount                  - remounts the /system, /vendor (if present) and /oem (if present) partitions on the device read-write\n"
        "  adb reboot [bootloader|recovery]\n"
        "                               - reboots the device, optionally into the bootloader or recovery program.\n"
        "  adb reboot sideload          - reboots the device into the sideload mode in recovery program (adb root required).\n"
        "  adb reboot sideload-auto-reboot\n"
        "                               - reboots into the sideload mode, then reboots automatically after the sideload regardless of the result.\n"
        "  adb sideload <file>          - sideloads the given package\n"
        "  adb root                     - restarts the adbd daemon with root permissions\n"
        "  adb unroot                   - restarts the adbd daemon without root permissions\n"
        "  adb usb                      - restarts the adbd daemon listening on USB\n"
        "  adb tcpip <port>             - restarts the adbd daemon listening on TCP on the specified port\n"
        "\n"
        "networking:\n"
        "  adb ppp <tty> [parameters]   - Run PPP over USB.\n"
        " Note: you should not automatically start a PPP connection.\n"
        " <tty> refers to the tty for PPP stream. Eg. dev:/dev/omap_csmi_tty1\n"
        " [parameters] - Eg. defaultroute debug dump local notty usepeerdns\n"
        "\n"
        "adb sync notes: adb sync [ <directory> ]\n"
        "  <localdir> can be interpreted in several ways:\n"
        "\n"
        "  - If <directory> is not specified, /system, /vendor (if present), /oem (if present) and /data partitions will be updated.\n"
        "\n"
        "  - If it is \"system\", \"vendor\", \"oem\" or \"data\", only the corresponding partition\n"
        "    is updated.\n"
        "\n"
        "internal debugging:\n"
        "  adb reconnect                  Kick current connection from host side and make it reconnect.\n"
        "  adb reconnect device           Kick current connection from device side and make it reconnect.\n"
        "environment variables:\n"
        "  ADB_TRACE                    - Print debug information. A comma separated list of the following values\n"
        "                                 1 or all, adb, sockets, packets, rwx, usb, sync, sysdeps, transport, jdwp\n"
        "  ANDROID_SERIAL               - The serial number to connect to. -s takes priority over this if given.\n"
        "  ANDROID_LOG_TAGS             - When used with the logcat option, only these debug tags are printed.\n"
        );
}
static int usage() {
    help();
    return 1;
}
#if defined(_WIN32)
void stdin_raw_init();
void stdin_raw_restore();
#else
static termios g_saved_terminal_state;
static void stdin_raw_init() {
    if (tcgetattr(STDIN_FILENO, &g_saved_terminal_state)) return;
    termios tio;
    if (tcgetattr(STDIN_FILENO, &tio)) return;
    cfmakeraw(&tio);
    tio.c_cc[VTIME] = 0;
    tio.c_cc[VMIN] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &tio);
}
static void stdin_raw_restore() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_terminal_state);
}
#endif
int read_and_dump(int fd, bool use_shell_protocol = false,
                  StandardStreamsCallbackInterface* callback = &DEFAULT_STANDARD_STREAMS_CALLBACK) {
    int exit_code = 0;
    if (fd < 0) return exit_code;
    std::unique_ptr<ShellProtocol> protocol;
    int length = 0;
    char raw_buffer[BUFSIZ];
    char* buffer_ptr = raw_buffer;
    if (use_shell_protocol) {
        protocol.reset(new ShellProtocol(fd));
        if (!protocol) {
            LOG(ERROR) << "failed to allocate memory for ShellProtocol object";
            return 1;
        }
        buffer_ptr = protocol->data();
    }
    while (true) {
        if (use_shell_protocol) {
            if (!protocol->Read()) {
                break;
            }
            length = protocol->data_length();
            switch (protocol->id()) {
                case ShellProtocol::kIdStdout:
                    callback->OnStdout(buffer_ptr, length);
                    break;
                case ShellProtocol::kIdStderr:
                    callback->OnStderr(buffer_ptr, length);
                    break;
                case ShellProtocol::kIdExit:
                    exit_code = protocol->data()[0];
                    continue;
                default:
                    continue;
            }
            length = protocol->data_length();
        } else {
            D("read_and_dump(): pre adb_read(fd=%d)", fd);
            length = adb_read(fd, raw_buffer, sizeof(raw_buffer));
            D("read_and_dump(): post adb_read(fd=%d): length=%d", fd, length);
            if (length <= 0) {
                break;
            }
            callback->OnStdout(buffer_ptr, length);
        }
    }
    return callback->Done(exit_code);
}
static void read_status_line(int fd, char* buf, size_t count)
{
    count--;
    while (count > 0) {
        int len = adb_read(fd, buf, count);
        if (len <= 0) {
            break;
        }
        buf += len;
        count -= len;
    }
    *buf = '\0';
}
static void copy_to_file(int inFd, int outFd) {
    const size_t BUFSIZE = 32 * 1024;
    char* buf = (char*) malloc(BUFSIZE);
    if (buf == nullptr) fatal("couldn't allocate buffer for copy_to_file");
    int len;
    long total = 0;
#ifdef _WIN32
    int old_stdin_mode = -1;
    int old_stdout_mode = -1;
#endif
    D("copy_to_file(%d -> %d)", inFd, outFd);
    if (inFd == STDIN_FILENO) {
        stdin_raw_init();
#ifdef _WIN32
        old_stdin_mode = _setmode(STDIN_FILENO, _O_BINARY);
        if (old_stdin_mode == -1) {
            fatal_errno("could not set stdin to binary");
        }
#endif
    }
#ifdef _WIN32
    if (outFd == STDOUT_FILENO) {
        old_stdout_mode = _setmode(STDOUT_FILENO, _O_BINARY);
        if (old_stdout_mode == -1) {
            fatal_errno("could not set stdout to binary");
        }
    }
#endif
    while (true) {
        if (inFd == STDIN_FILENO) {
            len = unix_read(inFd, buf, BUFSIZE);
        } else {
            len = adb_read(inFd, buf, BUFSIZE);
        }
        if (len == 0) {
            D("copy_to_file() : read 0 bytes; exiting");
            break;
        }
        if (len < 0) {
            D("copy_to_file(): read failed: %s", strerror(errno));
            break;
        }
        if (outFd == STDOUT_FILENO) {
            fwrite(buf, 1, len, stdout);
            fflush(stdout);
        } else {
            adb_write(outFd, buf, len);
        }
        total += len;
    }
    if (inFd == STDIN_FILENO) {
        stdin_raw_restore();
#ifdef _WIN32
        if (_setmode(STDIN_FILENO, old_stdin_mode) == -1) {
            fatal_errno("could not restore stdin mode");
        }
#endif
    }
#ifdef _WIN32
    if (outFd == STDOUT_FILENO) {
        if (_setmode(STDOUT_FILENO, old_stdout_mode) == -1) {
            fatal_errno("could not restore stdout mode");
        }
    }
#endif
    D("copy_to_file() finished after %lu bytes", total);
    free(buf);
}
static void send_window_size_change(int fd, std::unique_ptr<ShellProtocol>& shell) {
    if (shell == nullptr) return;
#if defined(_WIN32)
    struct winsize {
        unsigned short ws_row;
        unsigned short ws_col;
        unsigned short ws_xpixel;
        unsigned short ws_ypixel;
    };
#endif
    winsize ws;
#if defined(_WIN32)
    const intptr_t intptr_handle = _get_osfhandle(STDOUT_FILENO);
    if (intptr_handle == -1) return;
    const HANDLE handle = reinterpret_cast<const HANDLE>(intptr_handle);
    CONSOLE_SCREEN_BUFFER_INFO info;
    memset(&info, 0, sizeof(info));
    if (!GetConsoleScreenBufferInfo(handle, &info)) return;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = info.srWindow.Bottom - info.srWindow.Top + 1;
    ws.ws_col = info.dwSize.X;
#else
    if (ioctl(fd, TIOCGWINSZ, &ws) == -1) return;
#endif
    size_t l = snprintf(shell->data(), shell->data_capacity(), "%dx%d,%dx%d",
                        ws.ws_row, ws.ws_col, ws.ws_xpixel, ws.ws_ypixel);
    shell->Write(ShellProtocol::kIdWindowSizeChange, l + 1);
}
struct StdinReadArgs {
    int stdin_fd, write_fd;
    bool raw_stdin;
    std::unique_ptr<ShellProtocol> protocol;
    char escape_char;
};
static void stdin_read_thread_loop(void* x) {
    std::unique_ptr<StdinReadArgs> args(reinterpret_cast<StdinReadArgs*>(x));
#if !defined(_WIN32)
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGTTIN);
    pthread_sigmask(SIG_BLOCK, &sigset, nullptr);
#endif
#if defined(_WIN32)
#else
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGWINCH);
    pthread_sigmask(SIG_UNBLOCK, &mask, nullptr);
#endif
    send_window_size_change(args->stdin_fd, args->protocol);
    char raw_buffer[BUFSIZ];
    char* buffer_ptr = raw_buffer;
    size_t buffer_size = sizeof(raw_buffer);
    if (args->protocol != nullptr) {
        buffer_ptr = args->protocol->data();
        buffer_size = args->protocol->data_capacity();
    }
    if (args->raw_stdin && args->escape_char != '\0') {
        buffer_size = 1;
    }
    enum EscapeState { kMidFlow, kStartOfLine, kInEscape };
    EscapeState state = kStartOfLine;
    while (true) {
        D("stdin_read_thread_loop(): pre unix_read_interruptible(fdi=%d,...)", args->stdin_fd);
        int r = unix_read_interruptible(args->stdin_fd, buffer_ptr,
                                        buffer_size);
        if (r == -1 && errno == EINTR) {
            send_window_size_change(args->stdin_fd, args->protocol);
            continue;
        }
        D("stdin_read_thread_loop(): post unix_read_interruptible(fdi=%d,...)", args->stdin_fd);
        if (r <= 0) {
            if (args->protocol) {
                args->protocol->Write(ShellProtocol::kIdCloseStdin, 0);
            }
            break;
        }
        if (args->raw_stdin && args->escape_char != '\0') {
            char ch = buffer_ptr[0];
            if (ch == args->escape_char) {
                if (state == kStartOfLine) {
                    state = kInEscape;
                    continue;
                } else {
                    state = kMidFlow;
                }
            } else {
                if (state == kInEscape) {
                    if (ch == '.') {
                        fprintf(stderr,"\r\n[ disconnected ]\r\n");
                        stdin_raw_restore();
                        exit(0);
                    } else {
                        buffer_ptr[0] = args->escape_char;
                        buffer_ptr[1] = ch;
                        ++r;
                    }
                }
                state = (ch == '\n' || ch == '\r') ? kStartOfLine : kMidFlow;
            }
        }
        if (args->protocol) {
            if (!args->protocol->Write(ShellProtocol::kIdStdin, r)) {
                break;
            }
        } else {
            if (!WriteFdExactly(args->write_fd, buffer_ptr, r)) {
                break;
            }
        }
    }
}
static std::string ShellServiceString(bool use_shell_protocol,
                                      const std::string& type_arg,
                                      const std::string& command) {
    std::vector<std::string> args;
    if (use_shell_protocol) {
        args.push_back(kShellServiceArgShellProtocol);
        const char* terminal_type = getenv("TERM");
        if (terminal_type != nullptr) {
            args.push_back(std::string("TERM=") + terminal_type);
        }
    }
    if (!type_arg.empty()) {
        args.push_back(type_arg);
    }
    return android::base::StringPrintf("shell%s%s:%s",
                                       args.empty() ? "" : ",",
                                       android::base::Join(args, ',').c_str(),
                                       command.c_str());
}
static int RemoteShell(bool use_shell_protocol, const std::string& type_arg,
                       char escape_char,
                       const std::string& command) {
    std::string service_string = ShellServiceString(use_shell_protocol,
                                                    type_arg, command);
    bool raw_stdin = (type_arg == kShellServiceArgPty ||
                      (type_arg.empty() && command.empty()));
    std::string error;
    int fd = adb_connect(service_string, &error);
    if (fd < 0) {
        fprintf(stderr,"error: %s\n", error.c_str());
        return 1;
    }
    StdinReadArgs* args = new StdinReadArgs;
    if (!args) {
        LOG(ERROR) << "couldn't allocate StdinReadArgs object";
        return 1;
    }
    args->stdin_fd = STDIN_FILENO;
    args->write_fd = fd;
    args->raw_stdin = raw_stdin;
    args->escape_char = escape_char;
    if (use_shell_protocol) {
        args->protocol.reset(new ShellProtocol(args->write_fd));
    }
    if (raw_stdin) stdin_raw_init();
#if !defined(_WIN32)
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = [](int) {};
    sa.sa_flags = 0;
    sigaction(SIGWINCH, &sa, nullptr);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGWINCH);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);
#endif
    int exit_code = 1;
    if (!adb_thread_create(stdin_read_thread_loop, args)) {
        PLOG(ERROR) << "error starting stdin read thread";
        delete args;
    } else {
        exit_code = read_and_dump(fd, use_shell_protocol);
    }
    if (raw_stdin) stdin_raw_restore();
    return exit_code;
}
static int adb_shell(int argc, const char** argv) {
    FeatureSet features;
    std::string error;
    if (!adb_get_feature_set(&features, &error)) {
        fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    bool use_shell_protocol = CanUseFeature(features, kFeatureShell2);
    if (!use_shell_protocol) {
        D("shell protocol not supported, using raw data transfer");
    } else {
        D("using shell protocol");
    }
    --argc;
    ++argv;
    int t_arg_count = 0;
    char escape_char = '~';
    while (argc) {
        if (!strcmp(argv[0], "-e")) {
            if (argc < 2 || !(strlen(argv[1]) == 1 || strcmp(argv[1], "none") == 0)) {
                fprintf(stderr, "error: -e requires a single-character argument or 'none'\n");
                return 1;
            }
            escape_char = (strcmp(argv[1], "none") == 0) ? 0 : argv[1][0];
            argc -= 2;
            argv += 2;
        } else if (!strcmp(argv[0], "-T") || !strcmp(argv[0], "-t")) {
            if (argv[0][1] == 't') {
                ++t_arg_count;
            } else {
                t_arg_count = -1;
            }
            --argc;
            ++argv;
        } else if (!strcmp(argv[0], "-x")) {
            use_shell_protocol = false;
            --argc;
            ++argv;
        } else if (!strcmp(argv[0], "-n")) {
            close_stdin();
            --argc;
            ++argv;
        } else {
            break;
        }
    }
    if (!use_shell_protocol && t_arg_count != 0) {
        if (!CanUseFeature(features, kFeatureShell2)) {
            fprintf(stderr, "error: target doesn't support PTY args -Tt\n");
        } else {
            fprintf(stderr, "error: PTY args -Tt cannot be used with -x\n");
        }
        return 1;
    }
    std::string shell_type_arg;
    if (CanUseFeature(features, kFeatureShell2)) {
        if (t_arg_count < 0) {
            shell_type_arg = kShellServiceArgRaw;
        } else if (t_arg_count == 0) {
            if (!unix_isatty(STDIN_FILENO)) {
                shell_type_arg = kShellServiceArgRaw;
            }
        } else if (t_arg_count == 1) {
            if (!unix_isatty(STDIN_FILENO)) {
                fprintf(stderr,
                        "Remote PTY will not be allocated because stdin is not a terminal.\n"
                        "Use multiple -t options to force remote PTY allocation.\n");
                shell_type_arg = kShellServiceArgRaw;
            } else {
                shell_type_arg = kShellServiceArgPty;
            }
        } else {
            shell_type_arg = kShellServiceArgPty;
        }
    }
    std::string command;
    if (argc) {
        command = android::base::Join(std::vector<const char*>(argv, argv + argc), ' ');
    }
    return RemoteShell(use_shell_protocol, shell_type_arg, escape_char, command);
}
static int adb_download_buffer(const char *service, const char *fn, const void* data, unsigned sz,
                               bool show_progress)
{
    std::string error;
    int fd = adb_connect(android::base::StringPrintf("%s:%d", service, sz), &error);
    if (fd < 0) {
        fprintf(stderr,"error: %s\n", error.c_str());
        return -1;
    }
    int opt = CHUNK_SIZE;
    opt = adb_setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const void *) &opt, sizeof(opt));
    unsigned total = sz;
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data);
    if (show_progress) {
        const char* x = strrchr(service, ':');
        if (x) service = x + 1;
    }
    while (sz > 0) {
        unsigned xfer = (sz > CHUNK_SIZE) ? CHUNK_SIZE : sz;
        if (!WriteFdExactly(fd, ptr, xfer)) {
            std::string error;
            adb_status(fd, &error);
            fprintf(stderr,"* failed to write data '%s' *\n", error.c_str());
            adb_close(fd);
            return -1;
        }
        sz -= xfer;
        ptr += xfer;
        if (show_progress) {
            printf("sending: '%s' %4d%%    \r", fn, (int)(100LL - ((100LL * sz) / (total))));
            fflush(stdout);
        }
    }
    if (show_progress) {
        printf("\n");
    }
    if (!adb_status(fd, &error)) {
        fprintf(stderr,"* error response '%s' *\n", error.c_str());
        adb_close(fd);
        return -1;
    }
    adb_close(fd);
    return 0;
}
#define SIDELOAD_HOST_BLOCK_SIZE (CHUNK_SIZE)
static int adb_sideload_host(const char* fn) {
    fprintf(stderr, "loading: '%s'...\n", fn);
    std::string content;
    if (!android::base::ReadFileToString(fn, &content)) {
        fprintf(stderr, "failed: %s\n", strerror(errno));
        return -1;
    }
    const uint8_t* data = reinterpret_cast<const uint8_t*>(content.data());
    unsigned sz = content.size();
    fprintf(stderr, "connecting...\n");
    std::string service =
            android::base::StringPrintf("sideload-host:%d:%d", sz, SIDELOAD_HOST_BLOCK_SIZE);
    std::string error;
    unique_fd fd(adb_connect(service, &error));
    if (fd < 0) {
        fprintf(stderr, "falling back to older sideload method...\n");
        return adb_download_buffer("sideload", fn, data, sz, true);
    }
    int opt = SIDELOAD_HOST_BLOCK_SIZE;
    adb_setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &opt, sizeof(opt));
    size_t xfer = 0;
    int last_percent = -1;
    while (true) {
        char buf[9];
        if (!ReadFdExactly(fd, buf, 8)) {
            fprintf(stderr, "* failed to read command: %s\n", strerror(errno));
            return -1;
        }
        buf[8] = '\0';
        if (strcmp("DONEDONE", buf) == 0) {
            printf("\rTotal xfer: %.2fx%*s\n",
                   (double)xfer / (sz ? sz : 1), (int)strlen(fn)+10, "");
            return 0;
        }
        int block = strtol(buf, NULL, 10);
        size_t offset = block * SIDELOAD_HOST_BLOCK_SIZE;
        if (offset >= sz) {
            fprintf(stderr, "* attempt to read block %d past end\n", block);
            return -1;
        }
        const uint8_t* start = data + offset;
        size_t offset_end = offset + SIDELOAD_HOST_BLOCK_SIZE;
        size_t to_write = SIDELOAD_HOST_BLOCK_SIZE;
        if (offset_end > sz) {
            to_write = sz - offset;
        }
        if (!WriteFdExactly(fd, start, to_write)) {
            adb_status(fd, &error);
            fprintf(stderr,"* failed to write data '%s' *\n", error.c_str());
            return -1;
        }
        xfer += to_write;
        int percent = (int)(xfer * 47LL / (sz ? sz : 1));
        if (percent != last_percent) {
            printf("\rserving: '%s'  (~%d%%)    ", fn, percent);
            fflush(stdout);
            last_percent = percent;
        }
    }
}
static int ppp(int argc, const char** argv) {
#if defined(_WIN32)
    fprintf(stderr, "error: adb %s not implemented on Win32\n", argv[0]);
    return -1;
#else
    if (argc < 2) {
        fprintf(stderr, "usage: adb %s <adb service name> [ppp opts]\n",
                argv[0]);
        return 1;
    }
    const char* adb_service_name = argv[1];
    std::string error;
    int fd = adb_connect(adb_service_name, &error);
    if (fd < 0) {
        fprintf(stderr,"Error: Could not open adb service: %s. Error: %s\n",
                adb_service_name, error.c_str());
        return 1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("from fork()");
        return 1;
    } else if (pid == 0) {
        int err;
        int i;
        const char **ppp_args;
        ppp_args = (const char **) alloca(sizeof(char *) * argc + 1);
        ppp_args[0] = "pppd";
        for (i = 2 ; i < argc ; i++) {
            ppp_args[i - 1] = argv[i];
        }
        ppp_args[i-1] = NULL;
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        adb_close(STDERR_FILENO);
        adb_close(fd);
        err = execvp("pppd", (char * const *)ppp_args);
        if (err < 0) {
            perror("execing pppd");
        }
        exit(-1);
    } else {
        adb_close(fd);
        return 0;
    }
#endif
}
static bool wait_for_device(const char* service, TransportType t, const char* serial) {
    std::vector<std::string> components = android::base::Split(service, "-");
    if (components.size() < 3 || components.size() > 4) {
        fprintf(stderr, "adb: couldn't parse 'wait-for' command: %s\n", service);
        return false;
    }
    if (components.size() == 3) {
        auto it = components.begin() + 2;
        if (t == kTransportUsb) {
            components.insert(it, "usb");
        } else if (t == kTransportLocal) {
            components.insert(it, "local");
        } else {
            components.insert(it, "any");
        }
    } else if (components[2] != "any" && components[2] != "local" && components[2] != "usb") {
        fprintf(stderr, "adb: unknown type %s; expected 'any', 'local', or 'usb'\n",
                components[2].c_str());
        return false;
    }
    if (components[3] != "any" && components[3] != "bootloader" && components[3] != "device" &&
        components[3] != "recovery" && components[3] != "sideload") {
        fprintf(stderr,
                "adb: unknown state %s; "
                "expected 'any', 'bootloader', 'device', 'recovery', or 'sideload'\n",
                components[3].c_str());
        return false;
    }
    std::string cmd = format_host_command(android::base::Join(components, "-").c_str(), t, serial);
    return adb_command(cmd);
}
static bool adb_root(const char* command) {
    std::string error;
    unique_fd fd(adb_connect(android::base::StringPrintf("%s:", command), &error));
    if (fd < 0) {
        fprintf(stderr, "adb: unable to connect for %s: %s\n", command, error.c_str());
        return false;
    }
    char buf[256];
    char* cur = buf;
    ssize_t bytes_left = sizeof(buf);
    while (bytes_left > 0) {
        ssize_t bytes_read = adb_read(fd, cur, bytes_left);
        if (bytes_read == 0) {
            break;
        } else if (bytes_read < 0) {
            fprintf(stderr, "adb: error while reading for %s: %s\n", command, strerror(errno));
            return false;
        }
        cur += bytes_read;
        bytes_left -= bytes_read;
    }
    if (bytes_left == 0) {
        fprintf(stderr, "adb: unexpected output length for %s\n", command);
        return false;
    }
    fflush(stdout);
    WriteFdExactly(STDOUT_FILENO, buf, sizeof(buf) - bytes_left);
    if (cur != buf && strstr(buf, "restarting") == nullptr) {
        return true;
    }
    adb_sleep_ms(3000);
    return true;
}
    int fd;
    bool use_shell_protocol = false;
    while (true) {
        bool attempt_connection = true;
        if (!disable_shell_protocol) {
            FeatureSet features;
            std::string error;
            if (adb_get_feature_set(&features, &error)) {
                use_shell_protocol = CanUseFeature(features, kFeatureShell2);
            } else {
                attempt_connection = false;
            }
        }
        if (attempt_connection) {
            std::string error;
            std::string service_string = ShellServiceString(use_shell_protocol, "", command);
            fd = adb_connect(service_string, &error);
            if (fd >= 0) {
                break;
            }
        }
        fprintf(stderr, "- waiting for device -\n");
        if (!wait_for_device("wait-for-device", transport_type, serial)) {
            return 1;
        }
    }
    int exit_code = read_and_dump(fd, use_shell_protocol, callback);
    if (adb_close(fd) < 0) {
        PLOG(ERROR) << "failure closing FD " << fd;
    }
    return exit_code;
}
static int bugreport(TransportType transport_type, const char* serial, int argc,
                     const char** argv) {
    if (argc == 1) return send_shell_command(transport_type, serial, "bugreport", false);
    if (argc != 2) return usage();
    std::string dest_file = argv[1];
    if (!android::base::EndsWith(argv[1], ".zip")) {
        dest_file += ".zip";
    }
    std::string output;
    fprintf(stderr, "Bugreport is in progress and it could take minutes to complete.\n"
            "Please be patient and do not cancel or disconnect your device until it completes.\n");
    int status = send_shell_command(transport_type, serial, "bugreportz", false, &output, nullptr);
    if (status != 0 || output.empty()) return status;
    output = android::base::Trim(output);
    if (android::base::StartsWith(output, BUGZ_OK_PREFIX)) {
        const char* zip_file = &output[strlen(BUGZ_OK_PREFIX)];
        std::vector<const char*> srcs{zip_file};
        status = do_sync_pull(srcs, dest_file.c_str(), true, dest_file.c_str()) ? 0 : 1;
        if (status != 0) {
            fprintf(stderr, "Could not copy file '%s' to '%s'\n", zip_file, dest_file.c_str());
        }
        return status;
    }
    if (android::base::StartsWith(output, BUGZ_FAIL_PREFIX)) {
        const char* error_message = &output[strlen(BUGZ_FAIL_PREFIX)];
        fprintf(stderr, "Device failed to take a zipped bugreport: %s\n", error_message);
        return -1;
    }
    fprintf(stderr, "Unexpected string (%s) returned by bugreportz, "
            "device probably does not support -z option\n", output.c_str());
    return -1;
}
static int logcat(TransportType transport, const char* serial, int argc, const char** argv) {
    char* log_tags = getenv("ANDROID_LOG_TAGS");
    std::string quoted = escape_arg(log_tags == nullptr ? "" : log_tags);
    std::string cmd = "export ANDROID_LOG_TAGS=\"" + quoted + "\"; exec logcat";
    if (!strcmp(argv[0], "longcat")) {
        cmd += " -v long";
    }
    --argc;
    ++argv;
    while (argc-- > 0) {
        cmd += " " + escape_arg(*argv++);
    }
    return send_shell_command(transport, serial, cmd, true);
}
static int backup(int argc, const char** argv) {
    const char* filename = "backup.ab";
    for (int i = 1; i < argc; i++) {
        if (!strcmp("-f", argv[i])) {
            if (i == argc-1) {
                fprintf(stderr, "adb: backup -f passed with no filename.\n");
                return EXIT_FAILURE;
            }
            filename = argv[i+1];
            for (int j = i+2; j <= argc; ) {
                argv[i++] = argv[j++];
            }
            argc -= 2;
            argv[argc] = NULL;
        }
    }
    if (argc < 2) {
        fprintf(stderr, "adb: backup either needs a list of packages or -all/-shared.\n");
        return EXIT_FAILURE;
    }
    adb_unlink(filename);
    int outFd = adb_creat(filename, 0640);
    if (outFd < 0) {
        fprintf(stderr, "adb: backup unable to create file '%s': %s\n", filename, strerror(errno));
        return EXIT_FAILURE;
    }
    std::string cmd = "backup:";
    --argc;
    ++argv;
    while (argc-- > 0) {
        cmd += " " + escape_arg(*argv++);
    }
    D("backup. filename=%s cmd=%s", filename, cmd.c_str());
    std::string error;
    int fd = adb_connect(cmd, &error);
    if (fd < 0) {
        fprintf(stderr, "adb: unable to connect for backup: %s\n", error.c_str());
        adb_close(outFd);
        return EXIT_FAILURE;
    }
    printf("Now unlock your device and confirm the backup operation...\n");
    fflush(stdout);
    copy_to_file(fd, outFd);
    adb_close(fd);
    adb_close(outFd);
    return EXIT_SUCCESS;
}
static int restore(int argc, const char** argv) {
    if (argc != 2) return usage();
    const char* filename = argv[1];
    int tarFd = adb_open(filename, O_RDONLY);
    if (tarFd < 0) {
        fprintf(stderr, "adb: unable to open file %s: %s\n", filename, strerror(errno));
        return -1;
    }
    std::string error;
    int fd = adb_connect("restore:", &error);
    if (fd < 0) {
        fprintf(stderr, "adb: unable to connect for restore: %s\n", error.c_str());
        adb_close(tarFd);
        return -1;
    }
    printf("Now unlock your device and confirm the restore operation.\n");
    copy_to_file(tarFd, fd);
    copy_to_file(fd, STDOUT_FILENO);
    adb_close(fd);
    adb_close(tarFd);
    return 0;
}
static std::string find_product_out_path(const std::string& hint) {
    if (hint.empty()) {
        return "";
    }
    if (adb_is_absolute_host_path(hint.c_str())) {
        return hint;
    }
    if (hint.find_first_of(OS_PATH_SEPARATORS) != std::string::npos) {
        std::string cwd;
        if (!getcwd(&cwd)) {
            fprintf(stderr, "adb: getcwd failed: %s\n", strerror(errno));
            return "";
        }
        return android::base::StringPrintf("%s%c%s", cwd.c_str(), OS_PATH_SEPARATOR, hint.c_str());
    }
    char* top = getenv("ANDROID_BUILD_TOP");
    if (top == nullptr) {
        fprintf(stderr, "adb: ANDROID_BUILD_TOP not set!\n");
        return "";
    }
    std::string path = top;
    path += OS_PATH_SEPARATOR_STR;
    path += "out";
    path += OS_PATH_SEPARATOR_STR;
    path += "target";
    path += OS_PATH_SEPARATOR_STR;
    path += "product";
    path += OS_PATH_SEPARATOR_STR;
    path += hint;
    if (!directory_exists(path)) {
        fprintf(stderr, "adb: Couldn't find a product dir based on -p %s; "
                        "\"%s\" doesn't exist\n", hint.c_str(), path.c_str());
        return "";
    }
    return path;
}
static void parse_push_pull_args(const char** arg, int narg,
                                 std::vector<const char*>* srcs,
                                 const char** dst, bool* copy_attrs) {
    *copy_attrs = false;
    srcs->clear();
    bool ignore_flags = false;
    while (narg > 0) {
        if (ignore_flags || *arg[0] != '-') {
            srcs->push_back(*arg);
        } else {
            if (!strcmp(*arg, "-p")) {
            } else if (!strcmp(*arg, "-a")) {
                *copy_attrs = true;
            } else if (!strcmp(*arg, "--")) {
                ignore_flags = true;
            } else {
                fprintf(stderr, "adb: unrecognized option '%s'\n", *arg);
                exit(1);
            }
        }
        ++arg;
        --narg;
    }
    if (srcs->size() > 1) {
        *dst = srcs->back();
        srcs->pop_back();
    }
}
static int adb_connect_command(const std::string& command) {
    std::string error;
    int fd = adb_connect(command, &error);
    if (fd < 0) {
        fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    read_and_dump(fd);
    adb_close(fd);
    return 0;
}
static int adb_query_command(const std::string& command) {
    std::string result;
    std::string error;
    if (!adb_query(command, &result, &error)) {
        fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    printf("%s\n", result.c_str());
    return 0;
}
static bool _is_valid_ack_reply_fd(const int ack_reply_fd) {
#ifdef _WIN32
    const HANDLE ack_reply_handle = cast_int_to_handle(ack_reply_fd);
    return (GetStdHandle(STD_INPUT_HANDLE) != ack_reply_handle) &&
           (GetStdHandle(STD_OUTPUT_HANDLE) != ack_reply_handle) &&
           (GetStdHandle(STD_ERROR_HANDLE) != ack_reply_handle);
#else
    return ack_reply_fd > 2;
#endif
}
int adb_commandline(int argc, const char **argv) {
    int no_daemon = 0;
    int is_daemon = 0;
    int is_server = 0;
    int r;
    TransportType transport_type = kTransportAny;
    int ack_reply_fd = -1;
#if !defined(_WIN32)
    signal(SIGPIPE, SIG_IGN);
#endif
    char* ANDROID_PRODUCT_OUT = getenv("ANDROID_PRODUCT_OUT");
    if (ANDROID_PRODUCT_OUT != nullptr) {
        gProductOutPath = ANDROID_PRODUCT_OUT;
    }
    const char* server_port_str = getenv("ANDROID_ADB_SERVER_PORT");
    int server_port = DEFAULT_ADB_PORT;
    if (server_port_str && strlen(server_port_str) > 0) {
        server_port = strtol(server_port_str, nullptr, 0);
        if (server_port <= 0 || server_port > 65535) {
            fprintf(stderr,
                    "adb: Env var ANDROID_ADB_SERVER_PORT must be a positive number less than 65536. Got \"%s\"\n",
                    server_port_str);
            return usage();
        }
    }
    const char* serial = nullptr;
    while (argc > 0) {
        if (!strcmp(argv[0],"server")) {
            is_server = 1;
        } else if (!strcmp(argv[0],"nodaemon")) {
            no_daemon = 1;
        } else if (!strcmp(argv[0], "fork-server")) {
            is_daemon = 1;
        } else if (!strcmp(argv[0], "--reply-fd")) {
            if (argc < 2) return usage();
            const char* reply_fd_str = argv[1];
            argc--;
            argv++;
            ack_reply_fd = strtol(reply_fd_str, nullptr, 10);
            if (!_is_valid_ack_reply_fd(ack_reply_fd)) {
                fprintf(stderr, "adb: invalid reply fd \"%s\"\n", reply_fd_str);
                return usage();
            }
        } else if (!strncmp(argv[0], "-p", 2)) {
            const char* product = nullptr;
            if (argv[0][2] == '\0') {
                if (argc < 2) return usage();
                product = argv[1];
                argc--;
                argv++;
            } else {
                product = argv[0] + 2;
            }
            gProductOutPath = find_product_out_path(product);
            if (gProductOutPath.empty()) {
                fprintf(stderr, "adb: could not resolve \"-p %s\"\n", product);
                return usage();
            }
        } else if (argv[0][0]=='-' && argv[0][1]=='s') {
            if (isdigit(argv[0][2])) {
                serial = argv[0] + 2;
            } else {
                if (argc < 2 || argv[0][2] != '\0') return usage();
                serial = argv[1];
                argc--;
                argv++;
            }
        } else if (!strcmp(argv[0],"-d")) {
            transport_type = kTransportUsb;
        } else if (!strcmp(argv[0],"-e")) {
            transport_type = kTransportLocal;
        } else if (!strcmp(argv[0],"-a")) {
            gListenAll = 1;
        } else if (!strncmp(argv[0], "-H", 2)) {
            const char *hostname = NULL;
            if (argv[0][2] == '\0') {
                if (argc < 2) return usage();
                hostname = argv[1];
                argc--;
                argv++;
            } else {
                hostname = argv[0] + 2;
            }
            adb_set_tcp_name(hostname);
        } else if (!strncmp(argv[0], "-P", 2)) {
            if (argv[0][2] == '\0') {
                if (argc < 2) return usage();
                server_port_str = argv[1];
                argc--;
                argv++;
            } else {
                server_port_str = argv[0] + 2;
            }
            if (strlen(server_port_str) > 0) {
                server_port = (int) strtol(server_port_str, NULL, 0);
                if (server_port <= 0 || server_port > 65535) {
                    fprintf(stderr,
                            "adb: port number must be a positive number less than 65536. Got \"%s\"\n",
                            server_port_str);
                    return usage();
                }
            } else {
                fprintf(stderr,
                "adb: port number must be a positive number less than 65536. Got empty string.\n");
                return usage();
            }
        } else {
            break;
        }
        argc--;
        argv++;
    }
    if (transport_type == kTransportAny && serial == nullptr) {
        serial = getenv("ANDROID_SERIAL");
    }
    adb_set_transport(transport_type, serial);
    adb_set_tcp_specifics(server_port);
    if (is_server) {
        if (no_daemon || is_daemon) {
            if (is_daemon && (ack_reply_fd == -1)) {
                fprintf(stderr, "reply fd for adb server to client communication not specified.\n");
                return usage();
            }
            r = adb_server_main(is_daemon, server_port, ack_reply_fd);
        } else {
            r = launch_server(server_port);
        }
        if (r) {
            fprintf(stderr,"* could not start server *\n");
        }
        return r;
    }
    if (argc == 0) {
        return usage();
    }
    if (!strncmp(argv[0], "wait-for-", strlen("wait-for-"))) {
        const char* service = argv[0];
        if (!wait_for_device(service, transport_type, serial)) {
            return 1;
        }
        if (argc == 1) {
            return 0;
        }
        argc--;
        argv++;
    }
    if (!strcmp(argv[0], "devices")) {
        const char *listopt;
        if (argc < 2) {
            listopt = "";
        } else if (argc == 2 && !strcmp(argv[1], "-l")) {
            listopt = argv[1];
        } else {
            fprintf(stderr, "Usage: adb devices [-l]\n");
            return 1;
        }
        std::string query = android::base::StringPrintf("host:%s%s", argv[0], listopt);
        printf("List of devices attached\n");
        return adb_query_command(query);
    }
    else if (!strcmp(argv[0], "connect")) {
        if (argc != 2) {
            fprintf(stderr, "Usage: adb connect <host>[:<port>]\n");
            return 1;
        }
        std::string query = android::base::StringPrintf("host:connect:%s", argv[1]);
        return adb_query_command(query);
    }
    else if (!strcmp(argv[0], "disconnect")) {
        if (argc > 2) {
            fprintf(stderr, "Usage: adb disconnect [<host>[:<port>]]\n");
            return 1;
        }
        std::string query = android::base::StringPrintf("host:disconnect:%s",
                                                        (argc == 2) ? argv[1] : "");
        return adb_query_command(query);
    }
    else if (!strcmp(argv[0], "emu")) {
        return adb_send_emulator_command(argc, argv, serial);
    }
    else if (!strcmp(argv[0], "shell")) {
        return adb_shell(argc, argv);
    }
    else if (!strcmp(argv[0], "exec-in") || !strcmp(argv[0], "exec-out")) {
        int exec_in = !strcmp(argv[0], "exec-in");
        if (argc < 2) {
            fprintf(stderr, "Usage: adb %s command\n", argv[0]);
            return 1;
        }
        std::string cmd = "exec:";
        cmd += argv[1];
        argc -= 2;
        argv += 2;
        while (argc-- > 0) {
            cmd += " " + escape_arg(*argv++);
        }
        std::string error;
        int fd = adb_connect(cmd, &error);
        if (fd < 0) {
            fprintf(stderr, "error: %s\n", error.c_str());
            return -1;
        }
        if (exec_in) {
            copy_to_file(STDIN_FILENO, fd);
        } else {
            copy_to_file(fd, STDOUT_FILENO);
        }
        adb_close(fd);
        return 0;
    }
    else if (!strcmp(argv[0], "kill-server")) {
        std::string error;
        int fd = _adb_connect("host:kill", &error);
        if (fd == -2) {
            fprintf(stderr,"* server not running *\n");
            return 0;
        } else if (fd == -1) {
            fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        } else {
            ReadOrderlyShutdown(fd);
            adb_close(fd);
            return 0;
        }
    }
    else if (!strcmp(argv[0], "sideload")) {
        if (argc != 2) return usage();
        if (adb_sideload_host(argv[1])) {
            return 1;
        } else {
            return 0;
        }
    }
    else if (!strcmp(argv[0], "tcpip") && argc > 1) {
        return adb_connect_command(android::base::StringPrintf("tcpip:%s", argv[1]));
    }
    else if (!strcmp(argv[0], "remount") ||
             !strcmp(argv[0], "reboot") ||
             !strcmp(argv[0], "reboot-bootloader") ||
             !strcmp(argv[0], "usb") ||
             !strcmp(argv[0], "disable-verity") ||
             !strcmp(argv[0], "enable-verity")) {
        std::string command;
        if (!strcmp(argv[0], "reboot-bootloader")) {
            command = "reboot:bootloader";
        } else if (argc > 1) {
            command = android::base::StringPrintf("%s:%s", argv[0], argv[1]);
        } else {
            command = android::base::StringPrintf("%s:", argv[0]);
        }
        return adb_connect_command(command);
    } else if (!strcmp(argv[0], "root") || !strcmp(argv[0], "unroot")) {
        return adb_root(argv[0]) ? 0 : 1;
    } else if (!strcmp(argv[0], "bugreport")) {
        return bugreport(transport_type, serial, argc, argv);
    } else if (!strcmp(argv[0], "forward") || !strcmp(argv[0], "reverse")) {
        bool reverse = !strcmp(argv[0], "reverse");
        ++argv;
        --argc;
        if (argc < 1) return usage();
        std::string host_prefix;
        if (reverse) {
            host_prefix = "reverse";
        } else {
            if (serial) {
                host_prefix = android::base::StringPrintf("host-serial:%s", serial);
            } else if (transport_type == kTransportUsb) {
                host_prefix = "host-usb";
            } else if (transport_type == kTransportLocal) {
                host_prefix = "host-local";
            } else {
                host_prefix = "host";
            }
        }
        std::string cmd, error;
        if (strcmp(argv[0], "--list") == 0) {
            if (argc != 1) return usage();
            return adb_query_command(host_prefix + ":list-forward");
        } else if (strcmp(argv[0], "--remove-all") == 0) {
            if (argc != 1) return usage();
            cmd = host_prefix + ":killforward-all";
        } else if (strcmp(argv[0], "--remove") == 0) {
            if (argc != 2) return usage();
            cmd = host_prefix + ":killforward:" + argv[1];
        } else if (strcmp(argv[0], "--no-rebind") == 0) {
            if (argc != 3) return usage();
            if (forward_targets_are_valid(argv[1], argv[2], &error)) {
                cmd = host_prefix + ":forward:norebind:" + argv[1] + ";" + argv[2];
            }
        } else {
            if (argc != 2) return usage();
            if (forward_targets_are_valid(argv[0], argv[1], &error)) {
                cmd = host_prefix + ":forward:" + argv[0] + ";" + argv[1];
            }
        }
        if (!error.empty()) {
            fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
        int fd = adb_connect(cmd, &error);
        if (fd < 0 || !adb_status(fd, &error)) {
            adb_close(fd);
            fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
        std::string resolved_port;
        if (ReadProtocolString(fd, &resolved_port, &error) && !resolved_port.empty()) {
            printf("%s\n", resolved_port.c_str());
        }
        ReadOrderlyShutdown(fd);
        return 0;
    }
    else if (!strcmp(argv[0], "ls")) {
        if (argc != 2) return usage();
        return do_sync_ls(argv[1]) ? 0 : 1;
    }
    else if (!strcmp(argv[0], "push")) {
        bool copy_attrs = false;
        std::vector<const char*> srcs;
        const char* dst = nullptr;
        parse_push_pull_args(&argv[1], argc - 1, &srcs, &dst, &copy_attrs);
        if (srcs.empty() || !dst) return usage();
        return do_sync_push(srcs, dst) ? 0 : 1;
    }
    else if (!strcmp(argv[0], "pull")) {
        bool copy_attrs = false;
        std::vector<const char*> srcs;
        const char* dst = ".";
        parse_push_pull_args(&argv[1], argc - 1, &srcs, &dst, &copy_attrs);
        if (srcs.empty()) return usage();
        return do_sync_pull(srcs, dst, copy_attrs) ? 0 : 1;
    }
    else if (!strcmp(argv[0], "install")) {
        if (argc < 2) return usage();
        FeatureSet features;
        std::string error;
        if (!adb_get_feature_set(&features, &error)) {
            fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
        if (CanUseFeature(features, kFeatureCmd)) {
            return install_app(transport_type, serial, argc, argv);
        }
        return install_app_legacy(transport_type, serial, argc, argv);
    }
    else if (!strcmp(argv[0], "install-multiple")) {
        if (argc < 2) return usage();
        return install_multiple_app(transport_type, serial, argc, argv);
    }
    else if (!strcmp(argv[0], "uninstall")) {
        if (argc < 2) return usage();
        FeatureSet features;
        std::string error;
        if (!adb_get_feature_set(&features, &error)) {
            fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
        if (CanUseFeature(features, kFeatureCmd)) {
            return uninstall_app(transport_type, serial, argc, argv);
        }
        return uninstall_app_legacy(transport_type, serial, argc, argv);
    }
    else if (!strcmp(argv[0], "sync")) {
        std::string src;
        bool list_only = false;
        if (argc < 2) {
            src = "";
        } else if (argc >= 2 && strcmp(argv[1], "-l") == 0) {
            list_only = true;
            if (argc == 3) {
                src = argv[2];
            } else {
                src = "";
            }
        } else if (argc == 2) {
            src = argv[1];
        } else {
            return usage();
        }
        if (src != "" &&
            src != "system" && src != "data" && src != "vendor" && src != "oem") {
            return usage();
        }
        std::string system_src_path = product_file("system");
        std::string data_src_path = product_file("data");
        std::string vendor_src_path = product_file("vendor");
        std::string oem_src_path = product_file("oem");
        bool okay = true;
        if (okay && (src.empty() || src == "system")) {
            okay = do_sync_sync(system_src_path, "/system", list_only);
        }
        if (okay && (src.empty() || src == "vendor") && directory_exists(vendor_src_path)) {
            okay = do_sync_sync(vendor_src_path, "/vendor", list_only);
        }
        if (okay && (src.empty() || src == "oem") && directory_exists(oem_src_path)) {
            okay = do_sync_sync(oem_src_path, "/oem", list_only);
        }
        if (okay && (src.empty() || src == "data")) {
            okay = do_sync_sync(data_src_path, "/data", list_only);
        }
        return okay ? 0 : 1;
    }
    else if (!strcmp(argv[0],"get-state") ||
        !strcmp(argv[0],"get-serialno") ||
        !strcmp(argv[0],"get-devpath"))
    {
        return adb_query_command(format_host_command(argv[0], transport_type, serial));
    }
    else if (!strcmp(argv[0],"logcat") || !strcmp(argv[0],"lolcat") || !strcmp(argv[0],"longcat")) {
        return logcat(transport_type, serial, argc, argv);
    }
    else if (!strcmp(argv[0],"ppp")) {
        return ppp(argc, argv);
    }
    else if (!strcmp(argv[0], "start-server")) {
        std::string error;
        const int result = adb_connect("host:start-server", &error);
        if (result < 0) {
            fprintf(stderr, "error: %s\n", error.c_str());
        }
        return result;
    }
    else if (!strcmp(argv[0], "backup")) {
        return backup(argc, argv);
    }
    else if (!strcmp(argv[0], "restore")) {
        return restore(argc, argv);
    }
    else if (!strcmp(argv[0], "keygen")) {
        if (argc < 2) return usage();
        adb_trace_enable(AUTH);
        return adb_auth_keygen(argv[1]);
    }
    else if (!strcmp(argv[0], "jdwp")) {
        return adb_connect_command("jdwp");
    }
    else if (!strcmp(argv[0], "track-jdwp")) {
        return adb_connect_command("track-jdwp");
    }
    else if (!strcmp(argv[0], "track-devices")) {
        return adb_connect_command("host:track-devices");
    }
    else if (!strcmp(argv[0], "help") || !strcmp(argv[0], "/?")) {
        help();
        return 0;
    }
    else if (!strcmp(argv[0], "version")) {
        fprintf(stdout, "%s", adb_version().c_str());
        return 0;
    }
    else if (!strcmp(argv[0], "features")) {
        FeatureSet features;
        std::string error;
        if (!adb_get_feature_set(&features, &error)) {
            fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
        for (const std::string& name : features) {
            if (CanUseFeature(features, name)) {
                printf("%s\n", name.c_str());
            }
        }
        return 0;
    } else if (!strcmp(argv[0], "reconnect")) {
        if (argc == 1) {
            return adb_query_command("host:reconnect");
        } else if (argc == 2 && !strcmp(argv[1], "device")) {
            std::string err;
            adb_connect("reconnect", &err);
            return 0;
        }
    }
    usage();
    return 1;
}
static int uninstall_app(TransportType transport, const char* serial, int argc, const char** argv) {
    std::string cmd = "cmd package";
    while (argc-- > 0) {
        if (strcmp(*argv, "-k") == 0) {
            printf(
                "The -k option uninstalls the application while retaining the data/cache.\n"
                "At the moment, there is no way to remove the remaining data.\n"
                "You will have to reinstall the application with the same signature, and fully uninstall it.\n"
                "If you truly wish to continue, execute 'adb shell cmd package uninstall -k'.\n");
            return EXIT_FAILURE;
        }
        cmd += " " + escape_arg(*argv++);
    }
    return send_shell_command(transport, serial, cmd, false);
}
static int install_app(TransportType transport, const char* serial, int argc, const char** argv) {
    const char* file = argv[argc - 1];
    const char* dot = strrchr(file, '.');
    bool found_apk = false;
    struct stat sb;
    if (dot && !strcasecmp(dot, ".apk")) {
        if (stat(file, &sb) == -1 || !S_ISREG(sb.st_mode)) {
            fprintf(stderr, "Invalid APK file: %s\n", file);
            return EXIT_FAILURE;
        }
        found_apk = true;
    }
    if (!found_apk) {
        fprintf(stderr, "Missing APK file\n");
        return EXIT_FAILURE;
    }
    int localFd = adb_open(file, O_RDONLY);
    if (localFd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", file, strerror(errno));
        return 1;
    }
    std::string error;
    std::string cmd = "exec:cmd package";
    while (argc-- > 1) {
        cmd += " " + escape_arg(std::string(*argv++));
    }
    cmd += " " + android::base::StringPrintf("-S %" PRIu64, static_cast<uint64_t>(sb.st_size));
    int remoteFd = adb_connect(cmd, &error);
    if (remoteFd < 0) {
        fprintf(stderr, "Connect error for write: %s\n", error.c_str());
        adb_close(localFd);
        return 1;
    }
    char buf[BUFSIZ];
    copy_to_file(localFd, remoteFd);
    read_status_line(remoteFd, buf, sizeof(buf));
    adb_close(localFd);
    adb_close(remoteFd);
    if (strncmp("Success", buf, 7)) {
        fprintf(stderr, "Failed to install %s: %s", file, buf);
        return 1;
    }
    fputs(buf, stderr);
    return 0;
}
static int install_multiple_app(TransportType transport, const char* serial, int argc,
                                const char** argv)
{
    int i;
    struct stat sb;
    uint64_t total_size = 0;
    int first_apk = -1;
    for (i = argc - 1; i >= 0; i--) {
        const char* file = argv[i];
        const char* dot = strrchr(file, '.');
        if (dot && !strcasecmp(dot, ".apk")) {
            if (stat(file, &sb) == -1 || !S_ISREG(sb.st_mode)) {
                fprintf(stderr, "Invalid APK file: %s\n", file);
                return EXIT_FAILURE;
            }
            total_size += sb.st_size;
            first_apk = i;
        } else {
            break;
        }
    }
    if (first_apk == -1) {
        fprintf(stderr, "Missing APK file\n");
        return 1;
    }
    std::string cmd = android::base::StringPrintf("exec:pm install-create -S %" PRIu64, total_size);
    for (i = 1; i < first_apk; i++) {
        cmd += " " + escape_arg(argv[i]);
    }
    std::string error;
    int fd = adb_connect(cmd, &error);
    if (fd < 0) {
        fprintf(stderr, "Connect error for create: %s\n", error.c_str());
        return EXIT_FAILURE;
    }
    char buf[BUFSIZ];
    read_status_line(fd, buf, sizeof(buf));
    adb_close(fd);
    int session_id = -1;
    if (!strncmp("Success", buf, 7)) {
        char* start = strrchr(buf, '[');
        char* end = strrchr(buf, ']');
        if (start && end) {
            *end = '\0';
            session_id = strtol(start + 1, NULL, 10);
        }
    }
    if (session_id < 0) {
        fprintf(stderr, "Failed to create session\n");
        fputs(buf, stderr);
        return EXIT_FAILURE;
    }
    int success = 1;
    for (i = first_apk; i < argc; i++) {
        const char* file = argv[i];
        if (stat(file, &sb) == -1) {
            fprintf(stderr, "Failed to stat %s\n", file);
            success = 0;
            goto finalize_session;
        }
        std::string cmd = android::base::StringPrintf(
                "exec:pm install-write -S %" PRIu64 " %d %d_%s -",
                static_cast<uint64_t>(sb.st_size), session_id, i, adb_basename(file).c_str());
        int localFd = adb_open(file, O_RDONLY);
        if (localFd < 0) {
            fprintf(stderr, "Failed to open %s: %s\n", file, strerror(errno));
            success = 0;
            goto finalize_session;
        }
        std::string error;
        int remoteFd = adb_connect(cmd, &error);
        if (remoteFd < 0) {
            fprintf(stderr, "Connect error for write: %s\n", error.c_str());
            adb_close(localFd);
            success = 0;
            goto finalize_session;
        }
        copy_to_file(localFd, remoteFd);
        read_status_line(remoteFd, buf, sizeof(buf));
        adb_close(localFd);
        adb_close(remoteFd);
        if (strncmp("Success", buf, 7)) {
            fprintf(stderr, "Failed to write %s\n", file);
            fputs(buf, stderr);
            success = 0;
            goto finalize_session;
        }
    }
finalize_session:
    std::string service =
            android::base::StringPrintf("exec:pm install-%s %d",
                                        success ? "commit" : "abandon", session_id);
    fd = adb_connect(service, &error);
    if (fd < 0) {
        fprintf(stderr, "Connect error for finalize: %s\n", error.c_str());
        return EXIT_FAILURE;
    }
    read_status_line(fd, buf, sizeof(buf));
    adb_close(fd);
    if (!strncmp("Success", buf, 7)) {
        fputs(buf, stderr);
        return 0;
    } else {
        fprintf(stderr, "Failed to finalize session\n");
        fputs(buf, stderr);
        return EXIT_FAILURE;
    }
}
static int pm_command(TransportType transport, const char* serial, int argc, const char** argv) {
    std::string cmd = "pm";
    while (argc-- > 0) {
        cmd += " " + escape_arg(*argv++);
    }
    return send_shell_command(transport, serial, cmd, false);
}
static int uninstall_app_legacy(TransportType transport, const char* serial, int argc, const char** argv) {
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-k")) {
            printf(
                "The -k option uninstalls the application while retaining the data/cache.\n"
                "At the moment, there is no way to remove the remaining data.\n"
                "You will have to reinstall the application with the same signature, and fully uninstall it.\n"
                "If you truly wish to continue, execute 'adb shell pm uninstall -k'\n.");
            return EXIT_FAILURE;
        }
    }
    return pm_command(transport, serial, argc, argv);
}
static int delete_file(TransportType transport, const char* serial, const std::string& filename) {
    std::string cmd = "rm -f " + escape_arg(filename);
    return send_shell_command(transport, serial, cmd, false);
}
static int install_app_legacy(TransportType transport, const char* serial, int argc, const char** argv) {
    static const char *const DATA_DEST = "/data/local/tmp/%s";
    static const char *const SD_DEST = "/sdcard/tmp/%s";
    const char* where = DATA_DEST;
    int i;
    struct stat sb;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s")) {
            where = SD_DEST;
        }
    }
    int last_apk = -1;
    for (i = argc - 1; i >= 0; i--) {
        const char* file = argv[i];
        const char* dot = strrchr(file, '.');
        if (dot && !strcasecmp(dot, ".apk")) {
            if (stat(file, &sb) == -1 || !S_ISREG(sb.st_mode)) {
                fprintf(stderr, "Invalid APK file: %s\n", file);
                return EXIT_FAILURE;
            }
            last_apk = i;
            break;
        }
    }
    if (last_apk == -1) {
        fprintf(stderr, "Missing APK file\n");
        return EXIT_FAILURE;
    }
    int result = -1;
    std::vector<const char*> apk_file = {argv[last_apk]};
    std::string apk_dest = android::base::StringPrintf(
        where, adb_basename(argv[last_apk]).c_str());
    if (!do_sync_push(apk_file, apk_dest.c_str())) goto cleanup_apk;
    argv[last_apk] = apk_dest.c_str();
    result = pm_command(transport, serial, argc, argv);
cleanup_apk:
    delete_file(transport, serial, apk_dest);
    return result;
}
