#ifndef TMUX_H
#define TMUX_H 
#define PROTOCOL_VERSION 8
#include <sys/time.h>
#include <sys/uio.h>
#include <event.h>
<<<<<<< HEAD
||||||| 21062d74
#include <getopt.h>
#include <imsg.h>
=======
#include <imsg.h>
>>>>>>> 0a1a88d6
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <termios.h>
#ifdef HAVE_UTEMPTER
#include <utempter.h>
#endif
#include "array.h"
#include "compat.h"
extern char *__progname;
extern char **environ;
#define PROMPT_HISTORY 100
#define PANE_MINIMUM 2
#define NAME_INTERVAL 500
#define UTF8_SIZE 9
#define fatal(msg) log_fatal("%s: %s", __func__, msg);
#define fatalx(msg) log_fatalx("%s: %s", __func__, msg);
#define unused __attribute__ ((unused))
#define printflike(a,b) __attribute__ ((format (printf, a, b)))
#ifndef nitems
#define nitems(_a) (sizeof((_a)) / sizeof((_a)[0]))
#endif
#define BELL_NONE 0
#define BELL_ANY 1
#define BELL_CURRENT 2
#define KEYC_NONE 0xfff
#define KEYC_BASE 0x1000
#define KEYC_ESCAPE 0x2000
#define KEYC_CTRL 0x4000
#define KEYC_SHIFT 0x8000
#define KEYC_PREFIX 0x10000
#define KEYC_MASK_MOD (KEYC_ESCAPE|KEYC_CTRL|KEYC_SHIFT|KEYC_PREFIX)
#define KEYC_MASK_KEY (~KEYC_MASK_MOD)
enum key_code {
 KEYC_MOUSE = KEYC_BASE,
 KEYC_BSPACE,
 KEYC_F1,
 KEYC_F2,
 KEYC_F3,
 KEYC_F4,
 KEYC_F5,
 KEYC_F6,
 KEYC_F7,
 KEYC_F8,
 KEYC_F9,
 KEYC_F10,
 KEYC_F11,
 KEYC_F12,
 KEYC_IC,
 KEYC_DC,
 KEYC_HOME,
 KEYC_END,
 KEYC_NPAGE,
 KEYC_PPAGE,
 KEYC_BTAB,
 KEYC_UP,
 KEYC_DOWN,
 KEYC_LEFT,
 KEYC_RIGHT,
 KEYC_KP_SLASH,
 KEYC_KP_STAR,
 KEYC_KP_MINUS,
 KEYC_KP_SEVEN,
 KEYC_KP_EIGHT,
 KEYC_KP_NINE,
 KEYC_KP_PLUS,
 KEYC_KP_FOUR,
 KEYC_KP_FIVE,
 KEYC_KP_SIX,
 KEYC_KP_ONE,
 KEYC_KP_TWO,
 KEYC_KP_THREE,
 KEYC_KP_ENTER,
 KEYC_KP_ZERO,
 KEYC_KP_PERIOD,
 KEYC_FOCUS_IN,
 KEYC_FOCUS_OUT,
};
enum tty_code_code {
 TTYC_AX = 0,
 TTYC_ACSC,
 TTYC_BEL,
 TTYC_BLINK,
 TTYC_BOLD,
 TTYC_CIVIS,
 TTYC_CLEAR,
 TTYC_CNORM,
 TTYC_COLORS,
 TTYC_CR,
 TTYC_CS,
 TTYC_CSR,
 TTYC_CUB,
 TTYC_CUB1,
 TTYC_CUD,
 TTYC_CUD1,
 TTYC_CUF,
 TTYC_CUF1,
 TTYC_CUP,
 TTYC_CUU,
 TTYC_CUU1,
 TTYC_DCH,
 TTYC_DCH1,
 TTYC_DIM,
 TTYC_DL,
 TTYC_DL1,
 TTYC_E3,
 TTYC_ECH,
 TTYC_EL,
 TTYC_EL1,
 TTYC_ENACS,
 TTYC_FSL,
 TTYC_HOME,
 TTYC_HPA,
 TTYC_ICH,
 TTYC_ICH1,
 TTYC_IL,
 TTYC_IL1,
 TTYC_INVIS,
 TTYC_IS1,
 TTYC_IS2,
 TTYC_IS3,
 TTYC_KCBT,
 TTYC_KCUB1,
 TTYC_KCUD1,
 TTYC_KCUF1,
 TTYC_KCUU1,
 TTYC_KDC2,
 TTYC_KDC3,
 TTYC_KDC4,
 TTYC_KDC5,
 TTYC_KDC6,
 TTYC_KDC7,
 TTYC_KDCH1,
 TTYC_KDN2,
 TTYC_KDN3,
 TTYC_KDN4,
 TTYC_KDN5,
 TTYC_KDN6,
 TTYC_KDN7,
 TTYC_KEND,
 TTYC_KEND2,
 TTYC_KEND3,
 TTYC_KEND4,
 TTYC_KEND5,
 TTYC_KEND6,
 TTYC_KEND7,
 TTYC_KF1,
 TTYC_KF10,
 TTYC_KF11,
 TTYC_KF12,
 TTYC_KF13,
 TTYC_KF14,
 TTYC_KF15,
 TTYC_KF16,
 TTYC_KF17,
 TTYC_KF18,
 TTYC_KF19,
 TTYC_KF2,
 TTYC_KF20,
 TTYC_KF21,
 TTYC_KF22,
 TTYC_KF23,
 TTYC_KF24,
 TTYC_KF25,
 TTYC_KF26,
 TTYC_KF27,
 TTYC_KF28,
 TTYC_KF29,
 TTYC_KF3,
 TTYC_KF30,
 TTYC_KF31,
 TTYC_KF32,
 TTYC_KF33,
 TTYC_KF34,
 TTYC_KF35,
 TTYC_KF36,
 TTYC_KF37,
 TTYC_KF38,
 TTYC_KF39,
 TTYC_KF4,
 TTYC_KF40,
 TTYC_KF41,
 TTYC_KF42,
 TTYC_KF43,
 TTYC_KF44,
 TTYC_KF45,
 TTYC_KF46,
 TTYC_KF47,
 TTYC_KF48,
 TTYC_KF49,
 TTYC_KF5,
 TTYC_KF50,
 TTYC_KF51,
 TTYC_KF52,
 TTYC_KF53,
 TTYC_KF54,
 TTYC_KF55,
 TTYC_KF56,
 TTYC_KF57,
 TTYC_KF58,
 TTYC_KF59,
 TTYC_KF6,
 TTYC_KF60,
 TTYC_KF61,
 TTYC_KF62,
 TTYC_KF63,
 TTYC_KF7,
 TTYC_KF8,
 TTYC_KF9,
 TTYC_KHOM2,
 TTYC_KHOM3,
 TTYC_KHOM4,
 TTYC_KHOM5,
 TTYC_KHOM6,
 TTYC_KHOM7,
 TTYC_KHOME,
 TTYC_KIC2,
 TTYC_KIC3,
 TTYC_KIC4,
 TTYC_KIC5,
 TTYC_KIC6,
 TTYC_KIC7,
 TTYC_KICH1,
 TTYC_KLFT2,
 TTYC_KLFT3,
 TTYC_KLFT4,
 TTYC_KLFT5,
 TTYC_KLFT6,
 TTYC_KLFT7,
 TTYC_KMOUS,
 TTYC_KNP,
 TTYC_KNXT2,
 TTYC_KNXT3,
 TTYC_KNXT4,
 TTYC_KNXT5,
 TTYC_KNXT6,
 TTYC_KNXT7,
 TTYC_KPP,
 TTYC_KPRV2,
 TTYC_KPRV3,
 TTYC_KPRV4,
 TTYC_KPRV5,
 TTYC_KPRV6,
 TTYC_KPRV7,
 TTYC_KRIT2,
 TTYC_KRIT3,
 TTYC_KRIT4,
 TTYC_KRIT5,
 TTYC_KRIT6,
 TTYC_KRIT7,
 TTYC_KUP2,
 TTYC_KUP3,
 TTYC_KUP4,
 TTYC_KUP5,
 TTYC_KUP6,
 TTYC_KUP7,
 TTYC_MS,
 TTYC_OP,
 TTYC_REV,
 TTYC_RI,
 TTYC_RMACS,
 TTYC_RMCUP,
 TTYC_RMKX,
 TTYC_SE,
 TTYC_SETAB,
 TTYC_SETAF,
 TTYC_SGR0,
 TTYC_SITM,
 TTYC_SMACS,
 TTYC_SMCUP,
 TTYC_SMKX,
 TTYC_SMSO,
 TTYC_SMUL,
 TTYC_SS,
 TTYC_TSL,
 TTYC_VPA,
 TTYC_XENL,
 TTYC_XT,
};
#define NTTYCODE (TTYC_XT + 1)
enum tty_code_type {
 TTYCODE_NONE = 0,
 TTYCODE_STRING,
 TTYCODE_NUMBER,
 TTYCODE_FLAG,
};
struct tty_code {
 enum tty_code_type type;
 union {
  char *string;
  int number;
  int flag;
 } value;
};
struct tty_term_code_entry {
 enum tty_code_code code;
 enum tty_code_type type;
 const char *name;
};
ARRAY_DECL(causelist, char *);
enum msgtype {
 MSG_VERSION = 12,
 MSG_IDENTIFY_FLAGS = 100,
 MSG_IDENTIFY_TERM,
 MSG_IDENTIFY_TTYNAME,
 MSG_IDENTIFY_CWD,
 MSG_IDENTIFY_STDIN,
 MSG_IDENTIFY_ENVIRON,
 MSG_IDENTIFY_DONE,
 MSG_COMMAND = 200,
 MSG_DETACH,
 MSG_DETACHKILL,
 MSG_EXIT,
 MSG_EXITED,
 MSG_EXITING,
 MSG_LOCK,
 MSG_READY,
 MSG_RESIZE,
 MSG_SHELL,
 MSG_SHUTDOWN,
 MSG_STDERR,
 MSG_STDIN,
 MSG_STDOUT,
 MSG_SUSPEND,
 MSG_UNLOCK,
 MSG_WAKEUP,
};
struct msg_command_data {
 int argc;
};
struct msg_stdin_data {
 ssize_t size;
 char data[BUFSIZ];
};
struct msg_stdout_data {
 ssize_t size;
 char data[BUFSIZ];
};
struct msg_stderr_data {
 ssize_t size;
 char data[BUFSIZ];
};
enum mode_key_cmd {
 MODEKEY_NONE,
 MODEKEY_OTHER,
 MODEKEYEDIT_BACKSPACE,
 MODEKEYEDIT_CANCEL,
 MODEKEYEDIT_COMPLETE,
 MODEKEYEDIT_CURSORLEFT,
 MODEKEYEDIT_CURSORRIGHT,
 MODEKEYEDIT_DELETE,
 MODEKEYEDIT_DELETELINE,
 MODEKEYEDIT_DELETETOENDOFLINE,
 MODEKEYEDIT_DELETEWORD,
 MODEKEYEDIT_ENDOFLINE,
 MODEKEYEDIT_ENTER,
 MODEKEYEDIT_HISTORYDOWN,
 MODEKEYEDIT_HISTORYUP,
 MODEKEYEDIT_NEXTSPACE,
 MODEKEYEDIT_NEXTSPACEEND,
 MODEKEYEDIT_NEXTWORD,
 MODEKEYEDIT_NEXTWORDEND,
 MODEKEYEDIT_PASTE,
 MODEKEYEDIT_PREVIOUSSPACE,
 MODEKEYEDIT_PREVIOUSWORD,
 MODEKEYEDIT_STARTOFLINE,
 MODEKEYEDIT_SWITCHMODE,
 MODEKEYEDIT_SWITCHMODEAPPEND,
 MODEKEYEDIT_SWITCHMODEAPPENDLINE,
 MODEKEYEDIT_SWITCHMODEBEGINLINE,
 MODEKEYEDIT_SWITCHMODECHANGELINE,
 MODEKEYEDIT_SWITCHMODESUBSTITUTE,
 MODEKEYEDIT_SWITCHMODESUBSTITUTELINE,
 MODEKEYEDIT_TRANSPOSECHARS,
 MODEKEYCHOICE_BACKSPACE,
 MODEKEYCHOICE_BOTTOMLINE,
 MODEKEYCHOICE_CANCEL,
 MODEKEYCHOICE_CHOOSE,
 MODEKEYCHOICE_DOWN,
 MODEKEYCHOICE_ENDOFLIST,
 MODEKEYCHOICE_PAGEDOWN,
 MODEKEYCHOICE_PAGEUP,
 MODEKEYCHOICE_SCROLLDOWN,
 MODEKEYCHOICE_SCROLLUP,
 MODEKEYCHOICE_STARTNUMBERPREFIX,
 MODEKEYCHOICE_STARTOFLIST,
 MODEKEYCHOICE_TOPLINE,
 MODEKEYCHOICE_TREE_COLLAPSE,
 MODEKEYCHOICE_TREE_COLLAPSE_ALL,
 MODEKEYCHOICE_TREE_EXPAND,
 MODEKEYCHOICE_TREE_EXPAND_ALL,
 MODEKEYCHOICE_TREE_TOGGLE,
 MODEKEYCHOICE_UP,
 MODEKEYCOPY_APPENDSELECTION,
 MODEKEYCOPY_BACKTOINDENTATION,
 MODEKEYCOPY_BOTTOMLINE,
 MODEKEYCOPY_CANCEL,
 MODEKEYCOPY_CLEARSELECTION,
 MODEKEYCOPY_COPYPIPE,
 MODEKEYCOPY_COPYLINE,
 MODEKEYCOPY_COPYENDOFLINE,
 MODEKEYCOPY_COPYSELECTION,
 MODEKEYCOPY_DOWN,
 MODEKEYCOPY_ENDOFLINE,
 MODEKEYCOPY_GOTOLINE,
 MODEKEYCOPY_HALFPAGEDOWN,
 MODEKEYCOPY_HALFPAGEUP,
 MODEKEYCOPY_HISTORYBOTTOM,
 MODEKEYCOPY_HISTORYTOP,
 MODEKEYCOPY_JUMP,
 MODEKEYCOPY_JUMPAGAIN,
 MODEKEYCOPY_JUMPREVERSE,
 MODEKEYCOPY_JUMPBACK,
 MODEKEYCOPY_JUMPTO,
 MODEKEYCOPY_JUMPTOBACK,
 MODEKEYCOPY_LEFT,
 MODEKEYCOPY_MIDDLELINE,
 MODEKEYCOPY_NEXTPAGE,
 MODEKEYCOPY_NEXTSPACE,
 MODEKEYCOPY_NEXTSPACEEND,
 MODEKEYCOPY_NEXTWORD,
 MODEKEYCOPY_NEXTWORDEND,
 MODEKEYCOPY_OTHEREND,
 MODEKEYCOPY_PREVIOUSPAGE,
 MODEKEYCOPY_PREVIOUSSPACE,
 MODEKEYCOPY_PREVIOUSWORD,
 MODEKEYCOPY_RECTANGLETOGGLE,
 MODEKEYCOPY_RIGHT,
 MODEKEYCOPY_SCROLLDOWN,
 MODEKEYCOPY_SCROLLUP,
 MODEKEYCOPY_SEARCHAGAIN,
 MODEKEYCOPY_SEARCHDOWN,
 MODEKEYCOPY_SEARCHREVERSE,
 MODEKEYCOPY_SEARCHUP,
 MODEKEYCOPY_SELECTLINE,
 MODEKEYCOPY_STARTNAMEDBUFFER,
 MODEKEYCOPY_STARTNUMBERPREFIX,
 MODEKEYCOPY_STARTOFLINE,
 MODEKEYCOPY_STARTSELECTION,
 MODEKEYCOPY_TOPLINE,
 MODEKEYCOPY_UP,
};
struct mode_key_entry {
 int key;
 int mode;
 enum mode_key_cmd cmd;
};
struct mode_key_data {
 struct mode_key_tree *tree;
 int mode;
};
#define MODEKEY_EMACS 0
#define MODEKEY_VI 1
struct mode_key_binding {
 int key;
 int mode;
 enum mode_key_cmd cmd;
 const char *arg;
 RB_ENTRY(mode_key_binding) entry;
};
RB_HEAD(mode_key_tree, mode_key_binding);
struct mode_key_cmdstr {
 enum mode_key_cmd cmd;
 const char *name;
};
struct mode_key_table {
 const char *name;
 const struct mode_key_cmdstr *cmdstr;
 struct mode_key_tree *tree;
 const struct mode_key_entry *table;
};
#define MODE_CURSOR 0x1
#define MODE_INSERT 0x2
#define MODE_KCURSOR 0x4
#define MODE_KKEYPAD 0x8
#define MODE_WRAP 0x10
#define MODE_MOUSE_STANDARD 0x20
#define MODE_MOUSE_BUTTON 0x40
#define MODE_MOUSE_UTF8 0x100
#define MODE_MOUSE_SGR 0x200
#define MODE_BRACKETPASTE 0x400
#define MODE_FOCUSON 0x800
#define ALL_MOUSE_MODES (MODE_MOUSE_STANDARD|MODE_MOUSE_BUTTON)
struct utf8_data {
 u_char data[UTF8_SIZE];
 size_t have;
 size_t size;
 u_int width;
};
#define GRID_ATTR_BRIGHT 0x1
#define GRID_ATTR_DIM 0x2
#define GRID_ATTR_UNDERSCORE 0x4
#define GRID_ATTR_BLINK 0x8
#define GRID_ATTR_REVERSE 0x10
#define GRID_ATTR_HIDDEN 0x20
#define GRID_ATTR_ITALICS 0x40
#define GRID_ATTR_CHARSET 0x80
#define GRID_FLAG_FG256 0x1
#define GRID_FLAG_BG256 0x2
#define GRID_FLAG_PADDING 0x4
#define GRID_LINE_WRAPPED 0x1
struct grid_cell {
 u_char attr;
 u_char flags;
 u_char fg;
 u_char bg;
 u_char xstate;
 u_char xdata[UTF8_SIZE];
} __packed;
struct grid_line {
 u_int cellsize;
 struct grid_cell *celldata;
 int flags;
} __packed;
struct grid {
 int flags;
#define GRID_HISTORY 0x1
 u_int sx;
 u_int sy;
 u_int hsize;
 u_int hlimit;
 struct grid_line *linedata;
};
struct options_entry {
 char *name;
 enum {
  OPTIONS_STRING,
  OPTIONS_NUMBER,
  OPTIONS_STYLE
 } type;
 char *str;
 long long num;
 struct grid_cell style;
 RB_ENTRY(options_entry) entry;
};
struct options {
 RB_HEAD(options_tree, options_entry) tree;
 struct options *parent;
};
struct job {
 char *cmd;
 pid_t pid;
 int status;
 int fd;
 struct bufferevent *event;
 void (*callbackfn)(struct job *);
 void (*freefn)(void *);
 void *data;
 LIST_ENTRY(job) lentry;
};
LIST_HEAD(joblist, job);
struct screen_sel {
 int flag;
 int rectflag;
 int modekeys;
 u_int sx;
 u_int sy;
 u_int ex;
 u_int ey;
 struct grid_cell cell;
};
struct screen {
 char *title;
 struct grid *grid;
 u_int cx;
 u_int cy;
 u_int cstyle;
 char *ccolour;
 u_int rupper;
 u_int rlower;
 int mode;
 bitstr_t *tabs;
 struct screen_sel sel;
};
struct screen_write_ctx {
 struct window_pane *wp;
 struct screen *s;
};
#define screen_size_x(s) ((s)->grid->sx)
#define screen_size_y(s) ((s)->grid->sy)
#define screen_hsize(s) ((s)->grid->hsize)
#define screen_hlimit(s) ((s)->grid->hlimit)
struct input_cell {
 struct grid_cell cell;
 int set;
 int g0set;
 int g1set;
};
struct input_ctx {
 struct window_pane *wp;
 struct screen_write_ctx ctx;
 struct input_cell cell;
 struct input_cell old_cell;
 u_int old_cx;
 u_int old_cy;
 u_char interm_buf[4];
 size_t interm_len;
 u_char param_buf[64];
 size_t param_len;
#define INPUT_BUF_START 32
#define INPUT_BUF_LIMIT 1048576
 u_char *input_buf;
 size_t input_len;
 size_t input_space;
 int param_list[24];
 u_int param_list_len;
 struct utf8_data utf8data;
 int ch;
 int flags;
#define INPUT_DISCARD 0x1
 const struct input_state *state;
 struct evbuffer *since_ground;
};
struct session;
struct window;
struct mouse_event;
struct window_mode {
 struct screen *(*init)(struct window_pane *);
 void (*free)(struct window_pane *);
 void (*resize)(struct window_pane *, u_int, u_int);
 void (*key)(struct window_pane *, struct session *, int);
 void (*mouse)(struct window_pane *,
      struct session *, struct mouse_event *);
 void (*timer)(struct window_pane *);
};
struct window_choose_data {
 struct client *start_client;
 struct session *start_session;
 u_int idx;
 int type;
#define TREE_OTHER 0x0
#define TREE_WINDOW 0x1
#define TREE_SESSION 0x2
 struct session *tree_session;
 struct winlink *wl;
 int pane_id;
 char *ft_template;
 struct format_tree *ft;
 char *command;
};
struct window_choose_mode_item {
 struct window_choose_data *wcd;
 char *name;
 int pos;
 int state;
#define TREE_EXPANDED 0x1
};
struct window_pane {
 u_int id;
 u_int active_point;
 struct window *window;
 struct layout_cell *layout_cell;
 struct layout_cell *saved_layout_cell;
 u_int sx;
 u_int sy;
 u_int xoff;
 u_int yoff;
 int flags;
#define PANE_REDRAW 0x1
#define PANE_DROP 0x2
#define PANE_FOCUSED 0x4
#define PANE_RESIZE 0x8
#define PANE_FOCUSPUSH 0x10
#define PANE_INPUTOFF 0x20
 int argc;
 char **argv;
 char *shell;
 int cwd;
 pid_t pid;
 char tty[TTY_NAME_MAX];
 u_int changes;
 struct event changes_timer;
 u_int changes_redraw;
 int fd;
 struct bufferevent *event;
 struct input_ctx ictx;
 int pipe_fd;
 struct bufferevent *pipe_event;
 size_t pipe_off;
 struct screen *screen;
 struct screen base;
 u_int saved_cx;
 u_int saved_cy;
 struct grid *saved_grid;
 struct grid_cell saved_cell;
 const struct window_mode *mode;
 void *modedata;
 TAILQ_ENTRY(window_pane) entry;
 RB_ENTRY(window_pane) tree_entry;
};
TAILQ_HEAD(window_panes, window_pane);
RB_HEAD(window_pane_tree, window_pane);
ARRAY_DECL(window_pane_list, struct window_pane *);
struct window {
 u_int id;
 char *name;
 struct event name_timer;
 struct timeval silence_timer;
 struct window_pane *active;
 struct window_pane *last;
 struct window_panes panes;
 int lastlayout;
 struct layout_cell *layout_root;
 struct layout_cell *saved_layout_root;
 u_int sx;
 u_int sy;
 int flags;
#define WINDOW_BELL 0x1
#define WINDOW_ACTIVITY 0x2
#define WINDOW_REDRAW 0x4
#define WINDOW_SILENCE 0x8
#define WINDOW_ZOOMED 0x10
#define WINDOW_ALERTFLAGS (WINDOW_BELL|WINDOW_ACTIVITY|WINDOW_SILENCE)
 struct options options;
 u_int references;
};
ARRAY_DECL(windows, struct window *);
struct winlink {
 int idx;
 struct window *window;
 size_t status_width;
 struct grid_cell status_cell;
 char *status_text;
 int flags;
#define WINLINK_BELL 0x1
#define WINLINK_ACTIVITY 0x2
#define WINLINK_SILENCE 0x4
#define WINLINK_ALERTFLAGS (WINLINK_BELL|WINLINK_ACTIVITY|WINLINK_SILENCE)
 RB_ENTRY(winlink) entry;
 TAILQ_ENTRY(winlink) sentry;
};
RB_HEAD(winlinks, winlink);
TAILQ_HEAD(winlink_stack, winlink);
enum layout_type {
 LAYOUT_LEFTRIGHT,
 LAYOUT_TOPBOTTOM,
 LAYOUT_WINDOWPANE
};
TAILQ_HEAD(layout_cells, layout_cell);
struct layout_cell {
 enum layout_type type;
 struct layout_cell *parent;
 u_int sx;
 u_int sy;
 u_int xoff;
 u_int yoff;
 struct window_pane *wp;
 struct layout_cells cells;
 TAILQ_ENTRY(layout_cell) entry;
};
struct paste_buffer {
 char *data;
 size_t size;
 char *name;
 int automatic;
 u_int order;
 RB_ENTRY(paste_buffer) name_entry;
 RB_ENTRY(paste_buffer) time_entry;
};
struct environ_entry {
 char *name;
 char *value;
 RB_ENTRY(environ_entry) entry;
};
RB_HEAD(environ, environ_entry);
struct session_group {
 TAILQ_HEAD(, session) sessions;
 TAILQ_ENTRY(session_group) entry;
};
TAILQ_HEAD(session_groups, session_group);
struct session {
 u_int id;
 char *name;
 int cwd;
 struct timeval creation_time;
 struct timeval activity_time;
 struct timeval last_activity_time;
 u_int sx;
 u_int sy;
 struct winlink *curw;
 struct winlink_stack lastw;
 struct winlinks windows;
 struct options options;
#define SESSION_UNATTACHED 0x1
 int flags;
 u_int attached;
 struct termios *tio;
 struct environ environ;
 int references;
 TAILQ_ENTRY(session) gentry;
 RB_ENTRY(session) entry;
};
RB_HEAD(sessions, session);
ARRAY_DECL(sessionslist, struct session *);
struct tty_key {
 char ch;
 int key;
 struct tty_key *left;
 struct tty_key *right;
 struct tty_key *next;
};
struct tty_term {
 char *name;
 u_int references;
 char acs[UCHAR_MAX + 1][2];
 struct tty_code codes[NTTYCODE];
#define TERM_256COLOURS 0x1
#define TERM_EARLYWRAP 0x2
 int flags;
 LIST_ENTRY(tty_term) entry;
};
LIST_HEAD(tty_terms, tty_term);
#define MOUSE_MASK_BUTTONS 3
#define MOUSE_MASK_SHIFT 4
#define MOUSE_MASK_META 8
#define MOUSE_MASK_CTRL 16
#define MOUSE_MASK_DRAG 32
#define MOUSE_MASK_WHEEL 64
#define MOUSE_WHEEL_UP 0
#define MOUSE_WHEEL_DOWN 1
#define MOUSE_WHEEL_SCALE 3
#define MOUSE_EVENT_DOWN 0x1
#define MOUSE_EVENT_DRAG 0x2
#define MOUSE_EVENT_UP 0x4
#define MOUSE_EVENT_CLICK 0x8
#define MOUSE_EVENT_WHEEL 0x10
#define MOUSE_RESIZE_PANE 0x1
struct mouse_event {
 u_int xb;
 u_int x;
 u_int lx;
 u_int sx;
 u_int y;
 u_int ly;
 u_int sy;
 u_int sgr;
 u_int sgr_xb;
 u_int sgr_rel;
 u_int button;
 u_int clicks;
 u_int scroll;
 int wheel;
 int event;
 int flags;
};
struct tty {
 struct client *client;
 char *path;
 u_int class;
 u_int sx;
 u_int sy;
 u_int cx;
 u_int cy;
 u_int cstyle;
 char *ccolour;
 int mode;
 u_int rlower;
 u_int rupper;
 char *termname;
 struct tty_term *term;
 int fd;
 struct bufferevent *event;
 int log_fd;
 struct termios tio;
 struct grid_cell cell;
#define TTY_NOCURSOR 0x1
#define TTY_FREEZE 0x2
#define TTY_TIMER 0x4
#define TTY_UTF8 0x8
#define TTY_STARTED 0x10
#define TTY_OPENED 0x20
#define TTY_FOCUS 0x40
 int flags;
 int term_flags;
 struct mouse_event mouse;
 struct event key_timer;
 struct tty_key *key_tree;
};
struct tty_ctx {
 struct window_pane *wp;
 const struct grid_cell *cell;
 u_int num;
 void *ptr;
 u_int ocx;
 u_int ocy;
 u_int orupper;
 u_int orlower;
 u_int xoff;
 u_int yoff;
 struct grid_cell last_cell;
 u_int last_width;
};
struct message_entry {
 char *msg;
 time_t msg_time;
};
struct status_out {
 char *cmd;
 char *out;
 RB_ENTRY(status_out) entry;
};
RB_HEAD(status_out_tree, status_out);
struct client {
 struct imsgbuf ibuf;
 int fd;
 struct event event;
 int retval;
 struct timeval creation_time;
 struct timeval activity_time;
 struct environ environ;
 char *title;
 int cwd;
 char *term;
 char *ttyname;
 struct tty tty;
 void (*stdin_callback)(struct client *, int, void *);
 void *stdin_callback_data;
 struct evbuffer *stdin_data;
 int stdin_closed;
 struct evbuffer *stdout_data;
 struct evbuffer *stderr_data;
 struct event repeat_timer;
 struct status_out_tree status_old;
 struct status_out_tree status_new;
 struct timeval status_timer;
 struct screen status;
#define CLIENT_TERMINAL 0x1
#define CLIENT_PREFIX 0x2
#define CLIENT_EXIT 0x4
#define CLIENT_REDRAW 0x8
#define CLIENT_STATUS 0x10
#define CLIENT_REPEAT 0x20
#define CLIENT_SUSPENDED 0x40
#define CLIENT_BAD 0x80
#define CLIENT_IDENTIFY 0x100
#define CLIENT_DEAD 0x200
#define CLIENT_BORDERS 0x400
#define CLIENT_READONLY 0x800
#define CLIENT_REDRAWWINDOW 0x1000
#define CLIENT_CONTROL 0x2000
#define CLIENT_CONTROLCONTROL 0x4000
#define CLIENT_FOCUSED 0x8000
#define CLIENT_UTF8 0x10000
#define CLIENT_256COLOURS 0x20000
#define CLIENT_IDENTIFIED 0x40000
 int flags;
 struct event identify_timer;
 char *message_string;
 struct event message_timer;
 ARRAY_DECL(, struct message_entry) message_log;
 char *prompt_string;
 char *prompt_buffer;
 size_t prompt_index;
 int (*prompt_callbackfn)(void *, const char *);
 void (*prompt_freefn)(void *);
 void *prompt_data;
 u_int prompt_hindex;
#define PROMPT_SINGLE 0x1
 int prompt_flags;
 struct mode_key_data prompt_mdata;
 struct session *session;
 struct session *last_session;
 int wlmouse;
 struct cmd_q *cmdq;
 int references;
};
ARRAY_DECL(clients, struct client *);
struct args_entry {
 u_char flag;
 char *value;
 RB_ENTRY(args_entry) entry;
};
RB_HEAD(args_tree, args_entry);
struct args {
 struct args_tree tree;
 int argc;
 char **argv;
};
struct cmd {
 const struct cmd_entry *entry;
 struct args *args;
 char *file;
 u_int line;
#define CMD_CONTROL 0x1
 int flags;
 TAILQ_ENTRY(cmd) qentry;
};
struct cmd_list {
 int references;
 TAILQ_HEAD(, cmd) list;
};
enum cmd_retval {
 CMD_RETURN_ERROR = -1,
 CMD_RETURN_NORMAL = 0,
 CMD_RETURN_WAIT,
 CMD_RETURN_STOP
};
struct cmd_q_item {
 struct cmd_list *cmdlist;
 TAILQ_ENTRY(cmd_q_item) qentry;
};
TAILQ_HEAD(cmd_q_items, cmd_q_item);
struct cmd_q {
 int references;
 int dead;
 struct client *client;
 int client_exit;
 struct cmd_q_items queue;
 struct cmd_q_item *item;
 struct cmd *cmd;
 time_t time;
 u_int number;
 void (*emptyfn)(struct cmd_q *);
 void *data;
 TAILQ_ENTRY(cmd_q) waitentry;
};
struct cmd_entry {
 const char *name;
 const char *alias;
 const char *args_template;
 int args_lower;
 int args_upper;
 const char *usage;
#define CMD_STARTSERVER 0x1
#define CMD_CANTNEST 0x2
#define CMD_READONLY 0x4
 int flags;
 enum cmd_retval (*exec)(struct cmd *, struct cmd_q *);
};
struct key_binding {
 int key;
 struct cmd_list *cmdlist;
 int can_repeat;
 RB_ENTRY(key_binding) entry;
};
RB_HEAD(key_bindings, key_binding);
enum options_table_type {
 OPTIONS_TABLE_STRING,
 OPTIONS_TABLE_NUMBER,
 OPTIONS_TABLE_KEY,
 OPTIONS_TABLE_COLOUR,
 OPTIONS_TABLE_ATTRIBUTES,
 OPTIONS_TABLE_FLAG,
 OPTIONS_TABLE_CHOICE,
 OPTIONS_TABLE_STYLE
};
struct options_table_entry {
 const char *name;
 enum options_table_type type;
 u_int minimum;
 u_int maximum;
 const char **choices;
 const char *default_str;
 long long default_num;
 const char *style;
};
struct format_entry {
 char *key;
 char *value;
 RB_ENTRY(format_entry) entry;
};
RB_HEAD(format_tree, format_entry);
#define CMD_TARGET_PANE_USAGE "[-t target-pane]"
#define CMD_TARGET_WINDOW_USAGE "[-t target-window]"
#define CMD_TARGET_SESSION_USAGE "[-t target-session]"
#define CMD_TARGET_CLIENT_USAGE "[-t target-client]"
#define CMD_SRCDST_PANE_USAGE "[-s src-pane] [-t dst-pane]"
#define CMD_SRCDST_WINDOW_USAGE "[-s src-window] [-t dst-window]"
#define CMD_SRCDST_SESSION_USAGE "[-s src-session] [-t dst-session]"
#define CMD_SRCDST_CLIENT_USAGE "[-s src-client] [-t dst-client]"
#define CMD_BUFFER_USAGE "[-b buffer-name]"
extern struct options global_options;
extern struct options global_s_options;
extern struct options global_w_options;
extern struct environ global_environ;
extern struct event_base *ev_base;
extern char *cfg_file;
extern char *shell_cmd;
extern int debug_level;
extern time_t start_time;
extern char socket_path[PATH_MAX];
extern int login_shell;
extern char *environ_path;
void logfile(const char *);
const char *getshell(void);
int checkshell(const char *);
int areshell(const char *);
void setblocking(int, int);
__dead void shell_exec(const char *, const char *);
extern struct cmd_q *cfg_cmd_q;
extern int cfg_finished;
extern int cfg_references;
extern struct causelist cfg_causes;
extern struct client *cfg_client;
int load_cfg(const char *, struct cmd_q *, char **);
void cfg_default_done(struct cmd_q *);
void cfg_show_causes(struct session *);
int format_cmp(struct format_entry *, struct format_entry *);
RB_PROTOTYPE(format_tree, format_entry, entry, format_cmp);
struct format_tree *format_create(void);
void format_free(struct format_tree *);
void printflike(3, 4) format_add(struct format_tree *, const char *,
       const char *, ...);
const char *format_find(struct format_tree *, const char *);
char *format_expand(struct format_tree *, const char *);
void format_session(struct format_tree *, struct session *);
void format_client(struct format_tree *, struct client *);
void format_window(struct format_tree *, struct window *);
void format_winlink(struct format_tree *, struct session *,
       struct winlink *);
void format_window_pane(struct format_tree *,
       struct window_pane *);
void format_paste_buffer(struct format_tree *,
       struct paste_buffer *, int);
extern const struct mode_key_table mode_key_tables[];
extern struct mode_key_tree mode_key_tree_vi_edit;
extern struct mode_key_tree mode_key_tree_vi_choice;
extern struct mode_key_tree mode_key_tree_vi_copy;
extern struct mode_key_tree mode_key_tree_emacs_edit;
extern struct mode_key_tree mode_key_tree_emacs_choice;
extern struct mode_key_tree mode_key_tree_emacs_copy;
int mode_key_cmp(struct mode_key_binding *, struct mode_key_binding *);
RB_PROTOTYPE(mode_key_tree, mode_key_binding, entry, mode_key_cmp);
const char *mode_key_tostring(const struct mode_key_cmdstr *,
     enum mode_key_cmd);
enum mode_key_cmd mode_key_fromstring(const struct mode_key_cmdstr *,
     const char *);
const struct mode_key_table *mode_key_findtable(const char *);
void mode_key_init_trees(void);
void mode_key_init(struct mode_key_data *, struct mode_key_tree *);
enum mode_key_cmd mode_key_lookup(struct mode_key_data *, int, const char **);
void notify_enable(void);
void notify_disable(void);
void notify_input(struct window_pane *, struct evbuffer *);
void notify_window_layout_changed(struct window *);
void notify_window_unlinked(struct session *, struct window *);
void notify_window_linked(struct session *, struct window *);
void notify_window_renamed(struct window *);
void notify_attached_session_changed(struct client *);
void notify_session_renamed(struct session *);
void notify_session_created(struct session *);
void notify_session_closed(struct session *);
int options_cmp(struct options_entry *, struct options_entry *);
RB_PROTOTYPE(options_tree, options_entry, entry, options_cmp);
void options_init(struct options *, struct options *);
void options_free(struct options *);
struct options_entry *options_find1(struct options *, const char *);
struct options_entry *options_find(struct options *, const char *);
void options_remove(struct options *, const char *);
struct options_entry *printflike(3, 4) options_set_string(struct options *,
     const char *, const char *, ...);
char *options_get_string(struct options *, const char *);
struct options_entry *options_set_number(struct options *, const char *,
     long long);
long long options_get_number(struct options *, const char *);
struct options_entry *options_set_style(struct options *, const char *,
     const char *, int);
struct grid_cell *options_get_style(struct options *, const char *);
extern const struct options_table_entry server_options_table[];
extern const struct options_table_entry session_options_table[];
extern const struct options_table_entry window_options_table[];
void options_table_populate_tree(const struct options_table_entry *,
     struct options *);
const char *options_table_print_entry(const struct options_table_entry *,
     struct options_entry *, int);
int options_table_find(const char *, const struct options_table_entry **,
     const struct options_table_entry **);
extern struct joblist all_jobs;
struct job *job_run(const char *, struct session *,
     void (*)(struct job *), void (*)(void *), void *);
void job_free(struct job *);
void job_died(struct job *, int);
int environ_cmp(struct environ_entry *, struct environ_entry *);
RB_PROTOTYPE(environ, environ_entry, entry, environ_cmp);
void environ_init(struct environ *);
void environ_free(struct environ *);
void environ_copy(struct environ *, struct environ *);
struct environ_entry *environ_find(struct environ *, const char *);
void environ_set(struct environ *, const char *, const char *);
void environ_put(struct environ *, const char *);
void environ_unset(struct environ *, const char *);
void environ_update(const char *, struct environ *, struct environ *);
void environ_push(struct environ *);
void tty_init_termios(int, struct termios *, struct bufferevent *);
void tty_raw(struct tty *, const char *);
void tty_attributes(struct tty *, const struct grid_cell *);
void tty_reset(struct tty *);
void tty_region_pane(struct tty *, const struct tty_ctx *, u_int, u_int);
void tty_region(struct tty *, u_int, u_int);
void tty_cursor_pane(struct tty *, const struct tty_ctx *, u_int, u_int);
void tty_cursor(struct tty *, u_int, u_int);
void tty_putcode(struct tty *, enum tty_code_code);
void tty_putcode1(struct tty *, enum tty_code_code, int);
void tty_putcode2(struct tty *, enum tty_code_code, int, int);
void tty_putcode_ptr1(struct tty *, enum tty_code_code, const void *);
void tty_putcode_ptr2(struct tty *, enum tty_code_code, const void *,
     const void *);
void tty_puts(struct tty *, const char *);
void tty_putc(struct tty *, u_char);
void tty_putn(struct tty *, const void *, size_t, u_int);
void tty_init(struct tty *, struct client *, int, char *);
int tty_resize(struct tty *);
int tty_set_size(struct tty *, u_int, u_int);
void tty_set_class(struct tty *, u_int);
void tty_start_tty(struct tty *);
void tty_stop_tty(struct tty *);
void tty_set_title(struct tty *, const char *);
void tty_update_mode(struct tty *, int, struct screen *);
void tty_force_cursor_colour(struct tty *, const char *);
void tty_draw_line(struct tty *, struct screen *, u_int, u_int, u_int);
int tty_open(struct tty *, char **);
void tty_close(struct tty *);
void tty_free(struct tty *);
void tty_write(
     void (*)(struct tty *, const struct tty_ctx *), struct tty_ctx *);
void tty_cmd_alignmenttest(struct tty *, const struct tty_ctx *);
void tty_cmd_cell(struct tty *, const struct tty_ctx *);
void tty_cmd_clearendofline(struct tty *, const struct tty_ctx *);
void tty_cmd_clearendofscreen(struct tty *, const struct tty_ctx *);
void tty_cmd_clearline(struct tty *, const struct tty_ctx *);
void tty_cmd_clearscreen(struct tty *, const struct tty_ctx *);
void tty_cmd_clearstartofline(struct tty *, const struct tty_ctx *);
void tty_cmd_clearstartofscreen(struct tty *, const struct tty_ctx *);
void tty_cmd_deletecharacter(struct tty *, const struct tty_ctx *);
void tty_cmd_clearcharacter(struct tty *, const struct tty_ctx *);
void tty_cmd_deleteline(struct tty *, const struct tty_ctx *);
void tty_cmd_erasecharacter(struct tty *, const struct tty_ctx *);
void tty_cmd_insertcharacter(struct tty *, const struct tty_ctx *);
void tty_cmd_insertline(struct tty *, const struct tty_ctx *);
void tty_cmd_linefeed(struct tty *, const struct tty_ctx *);
void tty_cmd_utf8character(struct tty *, const struct tty_ctx *);
void tty_cmd_reverseindex(struct tty *, const struct tty_ctx *);
void tty_cmd_setselection(struct tty *, const struct tty_ctx *);
void tty_cmd_rawstring(struct tty *, const struct tty_ctx *);
void tty_bell(struct tty *);
extern struct tty_terms tty_terms;
extern const struct tty_term_code_entry tty_term_codes[NTTYCODE];
struct tty_term *tty_term_find(char *, int, char **);
void tty_term_free(struct tty_term *);
int tty_term_has(struct tty_term *, enum tty_code_code);
const char *tty_term_string(struct tty_term *, enum tty_code_code);
const char *tty_term_string1(struct tty_term *, enum tty_code_code, int);
const char *tty_term_string2(
       struct tty_term *, enum tty_code_code, int, int);
const char *tty_term_ptr1(
       struct tty_term *, enum tty_code_code, const void *);
const char *tty_term_ptr2(struct tty_term *, enum tty_code_code,
       const void *, const void *);
int tty_term_number(struct tty_term *, enum tty_code_code);
int tty_term_flag(struct tty_term *, enum tty_code_code);
const char *tty_acs_get(struct tty *, u_char);
void tty_keys_build(struct tty *);
void tty_keys_free(struct tty *);
int tty_keys_next(struct tty *);
struct paste_buffer *paste_walk(struct paste_buffer *);
struct paste_buffer *paste_get_top(void);
struct paste_buffer *paste_get_name(const char *);
int paste_free_top(void);
int paste_free_name(const char *);
void paste_add(char *, size_t);
int paste_rename(const char *, const char *, char **);
int paste_set(char *, size_t, const char *, char **);
char *paste_make_sample(struct paste_buffer *, int);
void paste_send_pane(struct paste_buffer *, struct window_pane *,
       const char *, int);
int args_cmp(struct args_entry *, struct args_entry *);
RB_PROTOTYPE(args_tree, args_entry, entry, args_cmp);
struct args *args_create(int, ...);
struct args *args_parse(const char *, int, char **);
void args_free(struct args *);
size_t args_print(struct args *, char *, size_t);
int args_has(struct args *, u_char);
void args_set(struct args *, u_char, const char *);
const char *args_get(struct args *, u_char);
long long args_strtonum(
      struct args *, u_char, long long, long long, char **);
int cmd_pack_argv(int, char **, char *, size_t);
int cmd_unpack_argv(char *, size_t, int, char ***);
char **cmd_copy_argv(int, char **);
void cmd_free_argv(int, char **);
char *cmd_stringify_argv(int, char **);
struct cmd *cmd_parse(int, char **, const char *, u_int, char **);
size_t cmd_print(struct cmd *, char *, size_t);
struct session *cmd_current_session(struct cmd_q *, int);
struct client *cmd_current_client(struct cmd_q *);
struct client *cmd_find_client(struct cmd_q *, const char *, int);
struct session *cmd_find_session(struct cmd_q *, const char *, int);
struct winlink *cmd_find_window(struct cmd_q *, const char *,
       struct session **);
int cmd_find_index(struct cmd_q *, const char *,
       struct session **);
struct winlink *cmd_find_pane(struct cmd_q *, const char *, struct session **,
       struct window_pane **);
char *cmd_template_replace(const char *, const char *, int);
struct window *cmd_lookup_windowid(const char *);
struct window_pane *cmd_lookup_paneid(const char *);
extern const struct cmd_entry *cmd_table[];
extern const struct cmd_entry cmd_attach_session_entry;
extern const struct cmd_entry cmd_bind_key_entry;
extern const struct cmd_entry cmd_break_pane_entry;
extern const struct cmd_entry cmd_capture_pane_entry;
extern const struct cmd_entry cmd_choose_buffer_entry;
extern const struct cmd_entry cmd_choose_client_entry;
extern const struct cmd_entry cmd_choose_session_entry;
extern const struct cmd_entry cmd_choose_tree_entry;
extern const struct cmd_entry cmd_choose_window_entry;
extern const struct cmd_entry cmd_clear_history_entry;
extern const struct cmd_entry cmd_clock_mode_entry;
extern const struct cmd_entry cmd_command_prompt_entry;
extern const struct cmd_entry cmd_confirm_before_entry;
extern const struct cmd_entry cmd_copy_mode_entry;
extern const struct cmd_entry cmd_delete_buffer_entry;
extern const struct cmd_entry cmd_detach_client_entry;
extern const struct cmd_entry cmd_display_message_entry;
extern const struct cmd_entry cmd_display_panes_entry;
extern const struct cmd_entry cmd_down_pane_entry;
extern const struct cmd_entry cmd_find_window_entry;
extern const struct cmd_entry cmd_has_session_entry;
extern const struct cmd_entry cmd_if_shell_entry;
extern const struct cmd_entry cmd_join_pane_entry;
extern const struct cmd_entry cmd_kill_pane_entry;
extern const struct cmd_entry cmd_kill_server_entry;
extern const struct cmd_entry cmd_kill_session_entry;
extern const struct cmd_entry cmd_kill_window_entry;
extern const struct cmd_entry cmd_last_pane_entry;
extern const struct cmd_entry cmd_last_window_entry;
extern const struct cmd_entry cmd_link_window_entry;
extern const struct cmd_entry cmd_list_buffers_entry;
extern const struct cmd_entry cmd_list_clients_entry;
extern const struct cmd_entry cmd_list_commands_entry;
extern const struct cmd_entry cmd_list_keys_entry;
extern const struct cmd_entry cmd_list_panes_entry;
extern const struct cmd_entry cmd_list_sessions_entry;
extern const struct cmd_entry cmd_list_windows_entry;
extern const struct cmd_entry cmd_load_buffer_entry;
extern const struct cmd_entry cmd_lock_client_entry;
extern const struct cmd_entry cmd_lock_server_entry;
extern const struct cmd_entry cmd_lock_session_entry;
extern const struct cmd_entry cmd_move_pane_entry;
extern const struct cmd_entry cmd_move_window_entry;
extern const struct cmd_entry cmd_new_session_entry;
extern const struct cmd_entry cmd_new_window_entry;
extern const struct cmd_entry cmd_next_layout_entry;
extern const struct cmd_entry cmd_next_window_entry;
extern const struct cmd_entry cmd_paste_buffer_entry;
extern const struct cmd_entry cmd_pipe_pane_entry;
extern const struct cmd_entry cmd_previous_layout_entry;
extern const struct cmd_entry cmd_previous_window_entry;
extern const struct cmd_entry cmd_refresh_client_entry;
extern const struct cmd_entry cmd_rename_session_entry;
extern const struct cmd_entry cmd_rename_window_entry;
extern const struct cmd_entry cmd_resize_pane_entry;
extern const struct cmd_entry cmd_respawn_pane_entry;
extern const struct cmd_entry cmd_respawn_window_entry;
extern const struct cmd_entry cmd_rotate_window_entry;
extern const struct cmd_entry cmd_run_shell_entry;
extern const struct cmd_entry cmd_save_buffer_entry;
extern const struct cmd_entry cmd_select_layout_entry;
extern const struct cmd_entry cmd_select_pane_entry;
extern const struct cmd_entry cmd_select_window_entry;
extern const struct cmd_entry cmd_send_keys_entry;
extern const struct cmd_entry cmd_send_prefix_entry;
extern const struct cmd_entry cmd_server_info_entry;
extern const struct cmd_entry cmd_set_buffer_entry;
extern const struct cmd_entry cmd_set_environment_entry;
extern const struct cmd_entry cmd_set_option_entry;
extern const struct cmd_entry cmd_set_window_option_entry;
extern const struct cmd_entry cmd_show_buffer_entry;
extern const struct cmd_entry cmd_show_environment_entry;
extern const struct cmd_entry cmd_show_messages_entry;
extern const struct cmd_entry cmd_show_options_entry;
extern const struct cmd_entry cmd_show_window_options_entry;
extern const struct cmd_entry cmd_source_file_entry;
extern const struct cmd_entry cmd_split_window_entry;
extern const struct cmd_entry cmd_start_server_entry;
extern const struct cmd_entry cmd_suspend_client_entry;
extern const struct cmd_entry cmd_swap_pane_entry;
extern const struct cmd_entry cmd_swap_window_entry;
extern const struct cmd_entry cmd_switch_client_entry;
extern const struct cmd_entry cmd_unbind_key_entry;
extern const struct cmd_entry cmd_unlink_window_entry;
extern const struct cmd_entry cmd_up_pane_entry;
extern const struct cmd_entry cmd_wait_for_entry;
enum cmd_retval cmd_attach_session(struct cmd_q *, const char *, int, int,
       const char *);
struct cmd_list *cmd_list_parse(int, char **, const char *, u_int, char **);
void cmd_list_free(struct cmd_list *);
size_t cmd_list_print(struct cmd_list *, char *, size_t);
struct cmd_q *cmdq_new(struct client *);
int cmdq_free(struct cmd_q *);
void printflike(2, 3) cmdq_print(struct cmd_q *, const char *, ...);
void printflike(2, 3) cmdq_error(struct cmd_q *, const char *, ...);
int cmdq_guard(struct cmd_q *, const char *, int);
void cmdq_run(struct cmd_q *, struct cmd_list *);
void cmdq_append(struct cmd_q *, struct cmd_list *);
int cmdq_continue(struct cmd_q *);
void cmdq_flush(struct cmd_q *);
int cmd_string_parse(const char *, struct cmd_list **, const char *,
     u_int, char **);
void cmd_wait_for_flush(void);
int client_main(int, char **, int);
extern struct key_bindings key_bindings;
int key_bindings_cmp(struct key_binding *, struct key_binding *);
RB_PROTOTYPE(key_bindings, key_binding, entry, key_bindings_cmp);
struct key_binding *key_bindings_lookup(int);
void key_bindings_add(int, int, struct cmd_list *);
void key_bindings_remove(int);
void key_bindings_init(void);
void key_bindings_dispatch(struct key_binding *, struct client *);
int key_string_lookup_string(const char *);
const char *key_string_lookup_key(int);
extern struct clients clients;
extern struct clients dead_clients;
int server_start(int, char *);
void server_update_socket(void);
void server_add_accept(int);
void server_client_handle_key(struct client *, int);
void server_client_create(int);
int server_client_open(struct client *, char **);
void server_client_lost(struct client *);
void server_client_callback(int, short, void *);
void server_client_status_timer(void);
void server_client_loop(void);
void server_window_loop(void);
void server_fill_environ(struct session *, struct environ *);
void server_write_ready(struct client *);
int server_write_client(struct client *, enum msgtype, const void *,
      size_t);
void server_write_session(struct session *, enum msgtype, const void *,
      size_t);
void server_redraw_client(struct client *);
void server_status_client(struct client *);
void server_redraw_session(struct session *);
void server_redraw_session_group(struct session *);
void server_status_session(struct session *);
void server_status_session_group(struct session *);
void server_redraw_window(struct window *);
void server_redraw_window_borders(struct window *);
void server_status_window(struct window *);
void server_lock(void);
void server_lock_session(struct session *);
void server_lock_client(struct client *);
int server_unlock(const char *);
void server_kill_window(struct window *);
int server_link_window(struct session *,
      struct winlink *, struct session *, int, int, int, char **);
void server_unlink_window(struct session *, struct winlink *);
void server_destroy_pane(struct window_pane *);
void server_destroy_session_group(struct session *);
void server_destroy_session(struct session *);
void server_check_unattached(void);
void server_set_identify(struct client *);
void server_clear_identify(struct client *);
void server_update_event(struct client *);
void server_push_stdout(struct client *);
void server_push_stderr(struct client *);
int server_set_stdin_callback(struct client *, void (*)(struct client *,
      int, void *), void *, char **);
void server_unzoom_window(struct window *);
int status_out_cmp(struct status_out *, struct status_out *);
RB_PROTOTYPE(status_out_tree, status_out, entry, status_out_cmp);
int status_at_line(struct client *);
void status_free_jobs(struct status_out_tree *);
void status_update_jobs(struct client *);
void status_set_window_at(struct client *, u_int);
int status_redraw(struct client *);
char *status_replace(struct client *, struct session *, struct winlink *,
      struct window_pane *, const char *, time_t, int);
void printflike(2, 3) status_message_set(struct client *, const char *, ...);
void status_message_clear(struct client *);
int status_message_redraw(struct client *);
void status_prompt_set(struct client *, const char *, const char *,
      int (*)(void *, const char *), void (*)(void *), void *, int);
void status_prompt_clear(struct client *);
int status_prompt_redraw(struct client *);
void status_prompt_key(struct client *, int);
void status_prompt_update(struct client *, const char *, const char *);
void recalculate_sizes(void);
void input_init(struct window_pane *);
void input_free(struct window_pane *);
void input_parse(struct window_pane *);
void input_key(struct window_pane *, int);
void input_mouse(struct window_pane *, struct session *,
      struct mouse_event *);
char *xterm_keys_lookup(int);
int xterm_keys_find(const char *, size_t, size_t *, int *);
void colour_set_fg(struct grid_cell *, int);
void colour_set_bg(struct grid_cell *, int);
const char *colour_tostring(int);
int colour_fromstring(const char *);
u_char colour_256to16(u_char);
const char *attributes_tostring(u_char);
int attributes_fromstring(const char *);
extern const struct grid_cell grid_default_cell;
extern const struct grid_cell grid_marker_cell;
struct grid *grid_create(u_int, u_int, u_int);
void grid_destroy(struct grid *);
int grid_compare(struct grid *, struct grid *);
void grid_collect_history(struct grid *);
void grid_scroll_history(struct grid *);
void grid_scroll_history_region(struct grid *, u_int, u_int);
void grid_expand_line(struct grid *, u_int, u_int);
const struct grid_cell *grid_peek_cell(struct grid *, u_int, u_int);
const struct grid_line *grid_peek_line(struct grid *, u_int);
struct grid_cell *grid_get_cell(struct grid *, u_int, u_int);
void grid_set_cell(struct grid *, u_int, u_int, const struct grid_cell *);
void grid_clear(struct grid *, u_int, u_int, u_int, u_int);
void grid_clear_lines(struct grid *, u_int, u_int);
void grid_move_lines(struct grid *, u_int, u_int, u_int);
void grid_move_cells(struct grid *, u_int, u_int, u_int, u_int);
char *grid_string_cells(struct grid *, u_int, u_int, u_int,
      struct grid_cell **, int, int, int);
void grid_duplicate_lines(
      struct grid *, u_int, struct grid *, u_int, u_int);
u_int grid_reflow(struct grid *, struct grid *, u_int);
u_int grid_cell_width(const struct grid_cell *);
void grid_cell_get(const struct grid_cell *, struct utf8_data *);
void grid_cell_set(struct grid_cell *, const struct utf8_data *);
void grid_cell_one(struct grid_cell *, u_char);
const struct grid_cell *grid_view_peek_cell(struct grid *, u_int, u_int);
struct grid_cell *grid_view_get_cell(struct grid *, u_int, u_int);
void grid_view_set_cell(
      struct grid *, u_int, u_int, const struct grid_cell *);
void grid_view_clear_history(struct grid *);
void grid_view_clear(struct grid *, u_int, u_int, u_int, u_int);
void grid_view_scroll_region_up(struct grid *, u_int, u_int);
void grid_view_scroll_region_down(struct grid *, u_int, u_int);
void grid_view_insert_lines(struct grid *, u_int, u_int);
void grid_view_insert_lines_region(struct grid *, u_int, u_int, u_int);
void grid_view_delete_lines(struct grid *, u_int, u_int);
void grid_view_delete_lines_region(struct grid *, u_int, u_int, u_int);
void grid_view_insert_cells(struct grid *, u_int, u_int, u_int);
void grid_view_delete_cells(struct grid *, u_int, u_int, u_int);
char *grid_view_string_cells(struct grid *, u_int, u_int, u_int);
void screen_write_start(
      struct screen_write_ctx *, struct window_pane *, struct screen *);
void screen_write_stop(struct screen_write_ctx *);
void screen_write_reset(struct screen_write_ctx *);
size_t printflike(2, 3) screen_write_cstrlen(int, const char *, ...);
void printflike(5, 6) screen_write_cnputs(struct screen_write_ctx *,
      ssize_t, struct grid_cell *, int, const char *, ...);
size_t printflike(2, 3) screen_write_strlen(int, const char *, ...);
void printflike(3, 4) screen_write_puts(struct screen_write_ctx *,
      struct grid_cell *, const char *, ...);
void printflike(5, 6) screen_write_nputs(struct screen_write_ctx *,
      ssize_t, struct grid_cell *, int, const char *, ...);
void screen_write_vnputs(struct screen_write_ctx *,
      ssize_t, struct grid_cell *, int, const char *, va_list);
void screen_write_putc(
      struct screen_write_ctx *, struct grid_cell *, u_char);
void screen_write_copy(struct screen_write_ctx *,
      struct screen *, u_int, u_int, u_int, u_int);
void screen_write_backspace(struct screen_write_ctx *);
void screen_write_mode_set(struct screen_write_ctx *, int);
void screen_write_mode_clear(struct screen_write_ctx *, int);
void screen_write_cursorup(struct screen_write_ctx *, u_int);
void screen_write_cursordown(struct screen_write_ctx *, u_int);
void screen_write_cursorright(struct screen_write_ctx *, u_int);
void screen_write_cursorleft(struct screen_write_ctx *, u_int);
void screen_write_alignmenttest(struct screen_write_ctx *);
void screen_write_insertcharacter(struct screen_write_ctx *, u_int);
void screen_write_deletecharacter(struct screen_write_ctx *, u_int);
void screen_write_clearcharacter(struct screen_write_ctx *, u_int);
void screen_write_insertline(struct screen_write_ctx *, u_int);
void screen_write_deleteline(struct screen_write_ctx *, u_int);
void screen_write_clearline(struct screen_write_ctx *);
void screen_write_clearendofline(struct screen_write_ctx *);
void screen_write_clearstartofline(struct screen_write_ctx *);
void screen_write_cursormove(struct screen_write_ctx *, u_int, u_int);
void screen_write_reverseindex(struct screen_write_ctx *);
void screen_write_scrollregion(struct screen_write_ctx *, u_int, u_int);
void screen_write_linefeed(struct screen_write_ctx *, int);
void screen_write_linefeedscreen(struct screen_write_ctx *, int);
void screen_write_carriagereturn(struct screen_write_ctx *);
void screen_write_clearendofscreen(struct screen_write_ctx *);
void screen_write_clearstartofscreen(struct screen_write_ctx *);
void screen_write_clearscreen(struct screen_write_ctx *);
void screen_write_clearhistory(struct screen_write_ctx *);
void screen_write_cell(struct screen_write_ctx *, const struct grid_cell *);
void screen_write_setselection(struct screen_write_ctx *, u_char *, u_int);
void screen_write_rawstring(struct screen_write_ctx *, u_char *, u_int);
void screen_redraw_screen(struct client *, int, int, int);
void screen_redraw_pane(struct client *, struct window_pane *);
void screen_init(struct screen *, u_int, u_int, u_int);
void screen_reinit(struct screen *);
void screen_free(struct screen *);
void screen_reset_tabs(struct screen *);
void screen_set_cursor_style(struct screen *, u_int);
void screen_set_cursor_colour(struct screen *, const char *);
void screen_set_title(struct screen *, const char *);
void screen_resize(struct screen *, u_int, u_int, int);
void screen_set_selection(struct screen *,
      u_int, u_int, u_int, u_int, u_int, struct grid_cell *);
void screen_clear_selection(struct screen *);
int screen_check_selection(struct screen *, u_int, u_int);
void screen_reflow(struct screen *, u_int);
extern struct windows windows;
extern struct window_pane_tree all_window_panes;
int winlink_cmp(struct winlink *, struct winlink *);
RB_PROTOTYPE(winlinks, winlink, entry, winlink_cmp);
int window_pane_cmp(struct window_pane *, struct window_pane *);
RB_PROTOTYPE(window_pane_tree, window_pane, tree_entry, window_pane_cmp);
struct winlink *winlink_find_by_index(struct winlinks *, int);
struct winlink *winlink_find_by_window(struct winlinks *, struct window *);
struct winlink *winlink_find_by_window_id(struct winlinks *, u_int);
int winlink_next_index(struct winlinks *, int);
u_int winlink_count(struct winlinks *);
struct winlink *winlink_add(struct winlinks *, int);
void winlink_set_window(struct winlink *, struct window *);
void winlink_remove(struct winlinks *, struct winlink *);
struct winlink *winlink_next(struct winlink *);
struct winlink *winlink_previous(struct winlink *);
struct winlink *winlink_next_by_number(struct winlink *, struct session *,
       int);
struct winlink *winlink_previous_by_number(struct winlink *, struct session *,
       int);
void winlink_stack_push(struct winlink_stack *, struct winlink *);
void winlink_stack_remove(struct winlink_stack *, struct winlink *);
int window_index(struct window *, u_int *);
struct window *window_find_by_id(u_int);
struct window *window_create1(u_int, u_int);
struct window *window_create(const char *, int, char **, const char *,
       const char *, int, struct environ *, struct termios *,
       u_int, u_int, u_int, char **);
void window_destroy(struct window *);
struct window_pane *window_get_active_at(struct window *, u_int, u_int);
void window_set_active_at(struct window *, u_int, u_int);
struct window_pane *window_find_string(struct window *, const char *);
void window_set_active_pane(struct window *, struct window_pane *);
struct window_pane *window_add_pane(struct window *, u_int);
void window_resize(struct window *, u_int, u_int);
int window_zoom(struct window_pane *);
int window_unzoom(struct window *);
void window_lost_pane(struct window *, struct window_pane *);
void window_remove_pane(struct window *, struct window_pane *);
struct window_pane *window_pane_at_index(struct window *, u_int);
struct window_pane *window_pane_next_by_number(struct window *,
          struct window_pane *, u_int);
struct window_pane *window_pane_previous_by_number(struct window *,
          struct window_pane *, u_int);
int window_pane_index(struct window_pane *, u_int *);
u_int window_count_panes(struct window *);
void window_destroy_panes(struct window *);
struct window_pane *window_pane_find_by_id(u_int);
struct window_pane *window_pane_create(struct window *, u_int, u_int, u_int);
void window_pane_destroy(struct window_pane *);
void window_pane_timer_start(struct window_pane *);
int window_pane_spawn(struct window_pane *, int, char **,
       const char *, const char *, int, struct environ *,
       struct termios *, char **);
void window_pane_resize(struct window_pane *, u_int, u_int);
void window_pane_alternate_on(struct window_pane *,
       struct grid_cell *, int);
void window_pane_alternate_off(struct window_pane *,
       struct grid_cell *, int);
int window_pane_set_mode(
       struct window_pane *, const struct window_mode *);
void window_pane_reset_mode(struct window_pane *);
void window_pane_key(struct window_pane *, struct session *, int);
void window_pane_mouse(struct window_pane *,
       struct session *, struct mouse_event *);
int window_pane_visible(struct window_pane *);
char *window_pane_search(
       struct window_pane *, const char *, u_int *);
char *window_printable_flags(struct session *, struct winlink *);
struct window_pane *window_pane_find_up(struct window_pane *);
struct window_pane *window_pane_find_down(struct window_pane *);
struct window_pane *window_pane_find_left(struct window_pane *);
struct window_pane *window_pane_find_right(struct window_pane *);
void window_set_name(struct window *, const char *);
void window_remove_ref(struct window *);
void winlink_clear_flags(struct winlink *);
u_int layout_count_cells(struct layout_cell *);
struct layout_cell *layout_create_cell(struct layout_cell *);
void layout_free_cell(struct layout_cell *);
void layout_print_cell(struct layout_cell *, const char *, u_int);
void layout_destroy_cell(struct layout_cell *, struct layout_cell **);
void layout_set_size(
       struct layout_cell *, u_int, u_int, u_int, u_int);
void layout_make_leaf(
       struct layout_cell *, struct window_pane *);
void layout_make_node(struct layout_cell *, enum layout_type);
void layout_fix_offsets(struct layout_cell *);
void layout_fix_panes(struct window *, u_int, u_int);
u_int layout_resize_check(struct layout_cell *, enum layout_type);
void layout_resize_adjust(
       struct layout_cell *, enum layout_type, int);
void layout_init(struct window *, struct window_pane *);
void layout_free(struct window *);
void layout_resize(struct window *, u_int, u_int);
void layout_resize_pane(struct window_pane *, enum layout_type,
       int);
void layout_resize_pane_to(struct window_pane *, enum layout_type,
       u_int);
void layout_resize_pane_mouse(struct client *);
void layout_assign_pane(struct layout_cell *, struct window_pane *);
struct layout_cell *layout_split_pane(
       struct window_pane *, enum layout_type, int, int);
void layout_close_pane(struct window_pane *);
char *layout_dump(struct window *);
int layout_parse(struct window *, const char *);
const char *layout_set_name(u_int);
int layout_set_lookup(const char *);
u_int layout_set_select(struct window *, u_int);
u_int layout_set_next(struct window *);
u_int layout_set_previous(struct window *);
void layout_set_active_changed(struct window *);
extern const struct window_mode window_clock_mode;
extern const char window_clock_table[14][5][5];
extern const struct window_mode window_copy_mode;
void window_copy_init_from_pane(struct window_pane *);
void window_copy_init_for_output(struct window_pane *);
void printflike(2, 3) window_copy_add(struct window_pane *, const char *, ...);
void window_copy_vadd(struct window_pane *, const char *, va_list);
void window_copy_pageup(struct window_pane *);
extern const struct window_mode window_choose_mode;
void window_choose_add(struct window_pane *,
    struct window_choose_data *);
void window_choose_ready(struct window_pane *,
       u_int, void (*)(struct window_choose_data *));
struct window_choose_data *window_choose_data_create (int,
       struct client *, struct session *);
void window_choose_data_free(struct window_choose_data *);
void window_choose_data_run(struct window_choose_data *);
struct window_choose_data *window_choose_add_window(struct window_pane *,
   struct client *, struct session *, struct winlink *,
   const char *, const char *, u_int);
struct window_choose_data *window_choose_add_session(struct window_pane *,
   struct client *, struct session *, const char *,
   const char *, u_int);
struct window_choose_data *window_choose_add_item(struct window_pane *,
   struct client *, struct winlink *, const char *,
   const char *, u_int);
void window_choose_expand_all(struct window_pane *);
void window_choose_collapse_all(struct window_pane *);
void window_choose_set_current(struct window_pane *, u_int);
void queue_window_name(struct window *);
char *default_window_name(struct window *);
char *format_window_name(struct window *);
char *parse_window_name(const char *);
void set_signals(void(*)(int, short, void *));
void clear_signals(int);
void control_callback(struct client *, int, void *);
void printflike(2, 3) control_write(struct client *, const char *, ...);
void control_write_buffer(struct client *, struct evbuffer *);
void control_notify_input(struct client *, struct window_pane *,
     struct evbuffer *);
void control_notify_window_layout_changed(struct window *);
void control_notify_window_unlinked(struct session *, struct window *);
void control_notify_window_linked(struct session *, struct window *);
void control_notify_window_renamed(struct window *);
void control_notify_attached_session_changed(struct client *);
void control_notify_session_renamed(struct session *);
void control_notify_session_created(struct session *);
void control_notify_session_close(struct session *);
extern struct sessions sessions;
extern struct sessions dead_sessions;
extern struct session_groups session_groups;
int session_cmp(struct session *, struct session *);
RB_PROTOTYPE(sessions, session, entry, session_cmp);
int session_alive(struct session *);
struct session *session_find(const char *);
struct session *session_find_by_id(u_int);
struct session *session_create(const char *, int, char **, const char *,
       int, struct environ *, struct termios *, int, u_int,
       u_int, char **);
void session_destroy(struct session *);
int session_check_name(const char *);
void session_update_activity(struct session *);
struct session *session_next_session(struct session *);
struct session *session_previous_session(struct session *);
struct winlink *session_new(struct session *, const char *, int, char **,
       const char *, int, int, char **);
struct winlink *session_attach(struct session *, struct window *, int,
       char **);
int session_detach(struct session *, struct winlink *);
struct winlink *session_has(struct session *, struct window *);
int session_next(struct session *, int);
int session_previous(struct session *, int);
int session_select(struct session *, int);
int session_last(struct session *);
int session_set_current(struct session *, struct winlink *);
struct session_group *session_group_find(struct session *);
u_int session_group_index(struct session_group *);
void session_group_add(struct session *, struct session *);
void session_group_remove(struct session *);
void session_group_synchronize_to(struct session *);
void session_group_synchronize_from(struct session *);
void session_group_synchronize1(struct session *, struct session *);
void session_renumber_windows(struct session *);
void utf8_build(void);
void utf8_set(struct utf8_data *, u_char);
int utf8_open(struct utf8_data *, u_char);
int utf8_append(struct utf8_data *, u_char);
u_int utf8_combine(const struct utf8_data *);
u_int utf8_split2(u_int, u_char *);
int utf8_strvis(char *, const char *, size_t, int);
struct utf8_data *utf8_fromcstr(const char *);
char *utf8_tocstr(struct utf8_data *);
u_int utf8_cstrwidth(const char *);
char *utf8_trimcstr(const char *, u_int);
char *osdep_get_name(int, char *);
char *osdep_get_cwd(int);
struct event_base *osdep_event_init(void);
void log_open(const char *);
void log_close(void);
void printflike(1, 2) log_debug(const char *, ...);
__dead void printflike(1, 2) log_fatal(const char *, ...);
__dead void printflike(1, 2) log_fatalx(const char *, ...);
char *xstrdup(const char *);
void *xcalloc(size_t, size_t);
void *xmalloc(size_t);
void *xrealloc(void *, size_t);
void *xreallocarray(void *, size_t, size_t);
int printflike(2, 3) xasprintf(char **, const char *, ...);
int xvasprintf(char **, const char *, va_list);
int printflike(3, 4) xsnprintf(char *, size_t, const char *, ...);
int xvsnprintf(char *, size_t, const char *, va_list);
int style_parse(const struct grid_cell *,
       struct grid_cell *, const char *);
const char *style_tostring(struct grid_cell *);
void style_update_new(struct options *, const char *, const char *);
void style_update_old(struct options *, const char *,
       struct grid_cell *);
void style_apply(struct grid_cell *, struct options *,
       const char *);
void style_apply_update(struct grid_cell *, struct options *,
       const char *);
#endif
