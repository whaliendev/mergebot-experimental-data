#ifndef __ADB_H
#define __ADB_H 
#include <limits.h>
#define MAX_PAYLOAD 4096
#define A_SYNC 0x434e5953
#define A_CNXN 0x4e584e43
#define A_OPEN 0x4e45504f
#define A_OKAY 0x59414b4f
#define A_CLSE 0x45534c43
#define A_WRTE 0x45545257
#define A_VERSION 0x01000000
#define ADB_VERSION_MAJOR 1
#define ADB_VERSION_MINOR 0
#define ADB_SERVER_VERSION 22
#define ADB_SERVER_VERSION 21
#define ADB_SERVER_VERSION 25
typedef struct amessage amessage;
typedef struct apacket apacket;
typedef struct asocket asocket;
typedef struct alistener alistener;
typedef struct aservice aservice;
typedef struct atransport atransport;
typedef struct adisconnect adisconnect;
typedef struct usb_handle usb_handle;
struct amessage {
unsigned command;
unsigned arg0;
unsigned arg1;
unsigned data_length;
unsigned data_check;
unsigned magic;
};
struct apacket
{
    apacket *next;
    unsigned len;
    unsigned char *ptr;
    amessage msg;
    unsigned char data[MAX_PAYLOAD];
};
struct asocket {
    asocket *next;
    asocket *prev;
    unsigned id;
    int closing;
    asocket *peer;
    fdevent fde;
    int fd;
    apacket *pkt_first;
    apacket *pkt_last;
    int (*enqueue)(asocket *s, apacket *pkt);
    void (*ready)(asocket *s);
    void (*close)(asocket *s);
    void *extra;
    atransport *transport;
};
struct adisconnect
{
    void (*func)(void* opaque, atransport* t);
    void* opaque;
    adisconnect* next;
    adisconnect* prev;
};
typedef enum transport_type {
        kTransportUsb,
        kTransportLocal,
        kTransportAny,
        kTransportHost,
} transport_type;
struct atransport
{
    atransport *next;
    atransport *prev;
    int (*read_from_remote)(apacket *p, atransport *t);
    int (*write_to_remote)(apacket *p, atransport *t);
    void (*close)(atransport *t);
    void (*kick)(atransport *t);
    int fd;
    int transport_socket;
    fdevent transport_fde;
    int ref_count;
    unsigned sync_token;
    int connection_state;
    transport_type type;
    usb_handle *usb;
    int sfd;
    char *serial;
    char *product;
    int kicked;
    adisconnect disconnects;
};
struct alistener
{
    alistener *next;
    alistener *prev;
    fdevent fde;
    int fd;
    const char *local_name;
    const char *connect_to;
    atransport *transport;
    adisconnect disconnect;
};
void print_packet(const char *label, apacket *p);
asocket *find_local_socket(unsigned id);
void install_local_socket(asocket *s);
void remove_socket(asocket *s);
void close_all_sockets(atransport *t);
#define LOCAL_CLIENT_PREFIX "emulator-"
asocket *create_local_socket(int fd);
asocket *create_local_service_socket(const char *destination);
asocket *create_remote_socket(unsigned id, atransport *t);
void connect_to_remote(asocket *s, const char *destination);
void connect_to_smartsocket(asocket *s);
void fatal(const char *fmt, ...);
void fatal_errno(const char *fmt, ...);
void handle_packet(apacket *p, atransport *t);
void send_packet(apacket *p, atransport *t);
void get_my_path(char s[PATH_MAX]);
int launch_server();
int adb_main(int is_daemon);
void init_transport_registration(void);
int list_transports(char *buf, size_t bufsize);
void update_transports(void);
asocket* create_device_tracker(void);
atransport *acquire_one_transport(int state, transport_type ttype, const char* serial, char **error_out);
void add_transport_disconnect( atransport* t, adisconnect* dis );
void remove_transport_disconnect( atransport* t, adisconnect* dis );
void run_transport_disconnects( atransport* t );
void kick_transport( atransport* t );
int init_socket_transport(atransport *t, int s, int port, int local);
void init_usb_transport(atransport *t, usb_handle *usb, int state);
void close_usb_devices();
void register_socket_transport(int s, const char *serial, int port, int local);
int service_to_fd(const char *name);
apacket *get_apacket(void);
void put_apacket(apacket *p);
int check_header(apacket *p);
int check_data(apacket *p);
int readx(int fd, void *ptr, size_t len);
int writex(int fd, const void *ptr, size_t len);
#define ADB_TRACE 1
typedef enum {
    TRACE_ADB = 0,
    TRACE_SOCKETS,
    TRACE_PACKETS,
    TRACE_TRANSPORT,
    TRACE_RWX,
    TRACE_USB,
    TRACE_SYNC,
    TRACE_SYSDEPS,
    TRACE_JDWP,
} AdbTrace;
#define ADB_PORT 5037
#define ADB_LOCAL_TRANSPORT_PORT 5555
#define ADB_CLASS 0xff
#define ADB_SUBCLASS 0x42
#define ADB_PROTOCOL 0x1
void local_init(int port);
int local_connect(int port);
void usb_init();
void usb_cleanup();
int usb_write(usb_handle *h, const void *data, int len);
int usb_read(usb_handle *h, void *data, int len);
int usb_close(usb_handle *h);
void usb_kick(usb_handle *h);
unsigned host_to_le32(unsigned n);
int adb_commandline(int argc, char **argv);
int connection_state(atransport *t);
#define CS_ANY -1
#define CS_OFFLINE 0
#define CS_BOOTLOADER 1
#define CS_DEVICE 2
#define CS_HOST 3
#define CS_RECOVERY 4
#define CS_NOPERM 5
extern int HOST;
#define CHUNK_SIZE (64*1024)
int sendfailmsg(int fd, const char *reason);
int handle_host_request(char *service, transport_type ttype, char* serial, int reply_fd, asocket *s);
void register_usb_transport(usb_handle *h, const char *serial, unsigned writeable);
void unregister_usb_transport(usb_handle *usb);
#endif
