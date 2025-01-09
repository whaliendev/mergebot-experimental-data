#ifndef _LMKD_H_
#define _LMKD_H_ 
#include <arpa/inet.h>
#include <sys/cdefs.h>
#include <sys/types.h>
__BEGIN_DECLS
enum lmk_cmd {
    LMK_TARGET = 0,
    LMK_PROCPRIO,
    LMK_PROCREMOVE,
<<<<<<< HEAD
    LMK_PROCPURGE,
    LMK_GETKILLCNT,
||||||| 83cda925e
=======
    LMK_PROCPURGE,
>>>>>>> 5d1d32dc
};
#define MAX_TARGETS 6
#define CTRL_PACKET_MAX_SIZE (sizeof(int) * (MAX_TARGETS * 2 + 1))
typedef int LMKD_CTRL_PACKET[CTRL_PACKET_MAX_SIZE / sizeof(int)];
static inline enum lmk_cmd lmkd_pack_get_cmd(LMKD_CTRL_PACKET pack) {
    return (enum lmk_cmd)ntohl(pack[0]);
}
struct lmk_target {
    int minfree;
    int oom_adj_score;
};
static inline void lmkd_pack_get_target(LMKD_CTRL_PACKET packet, int target_idx,
                                        struct lmk_target* target) {
    target->minfree = ntohl(packet[target_idx * 2 + 1]);
    target->oom_adj_score = ntohl(packet[target_idx * 2 + 2]);
}
static inline size_t lmkd_pack_set_target(LMKD_CTRL_PACKET packet, struct lmk_target* targets,
                                          size_t target_cnt) {
    int idx = 0;
    packet[idx++] = htonl(LMK_TARGET);
    while (target_cnt) {
        packet[idx++] = htonl(targets->minfree);
        packet[idx++] = htonl(targets->oom_adj_score);
        targets++;
        target_cnt--;
    }
    return idx * sizeof(int);
}
struct lmk_procprio {
    pid_t pid;
    uid_t uid;
    int oomadj;
};
static inline void lmkd_pack_get_procprio(LMKD_CTRL_PACKET packet, struct lmk_procprio* params) {
    params->pid = (pid_t)ntohl(packet[1]);
    params->uid = (uid_t)ntohl(packet[2]);
    params->oomadj = ntohl(packet[3]);
}
static inline size_t lmkd_pack_set_procprio(LMKD_CTRL_PACKET packet, struct lmk_procprio* params) {
    packet[0] = htonl(LMK_PROCPRIO);
    packet[1] = htonl(params->pid);
    packet[2] = htonl(params->uid);
    packet[3] = htonl(params->oomadj);
    return 4 * sizeof(int);
}
struct lmk_procremove {
    pid_t pid;
};
static inline void lmkd_pack_get_procremove(LMKD_CTRL_PACKET packet,
                                            struct lmk_procremove* params) {
    params->pid = (pid_t)ntohl(packet[1]);
}
static inline size_t lmkd_pack_set_procremove(LMKD_CTRL_PACKET packet,
                                              struct lmk_procprio* params) {
    packet[0] = htonl(LMK_PROCREMOVE);
    packet[1] = htonl(params->pid);
    return 2 * sizeof(int);
}
<<<<<<< HEAD
static inline size_t lmkd_pack_set_procpurge(LMKD_CTRL_PACKET packet) {
    packet[0] = htonl(LMK_PROCPURGE);
    return sizeof(int);
}
struct lmk_getkillcnt {
    int min_oomadj;
    int max_oomadj;
};
static inline void lmkd_pack_get_getkillcnt(LMKD_CTRL_PACKET packet,
                                            struct lmk_getkillcnt* params) {
    params->min_oomadj = ntohl(packet[1]);
    params->max_oomadj = ntohl(packet[2]);
}
static inline size_t lmkd_pack_set_getkillcnt(LMKD_CTRL_PACKET packet,
                                              struct lmk_getkillcnt* params) {
    packet[0] = htonl(LMK_GETKILLCNT);
    packet[1] = htonl(params->min_oomadj);
    packet[2] = htonl(params->max_oomadj);
    return 3 * sizeof(int);
}
static inline size_t lmkd_pack_set_getkillcnt_repl(LMKD_CTRL_PACKET packet, int kill_cnt) {
    packet[0] = htonl(LMK_GETKILLCNT);
    packet[1] = htonl(kill_cnt);
    return 2 * sizeof(int);
}
||||||| 83cda925e
=======
inline size_t lmkd_pack_set_procpurge(LMKD_CTRL_PACKET packet) {
    packet[0] = htonl(LMK_PROCPURGE);
    return sizeof(int);
}
>>>>>>> 5d1d32dc
__END_DECLS
#endif
