#ifndef AUXO_CRED_H
#define AUXO_CRED_H
/* Users and groups (kern/cred.c): a task's user and group ids, who owns a file and what its
 * mode lets each of them do, set-user-ID programs, the accounts in /etc/passwd, /etc/group
 * and /etc/shadow, and the session's user (who the desktop and the Terminal run as). */
#include <stdint.h>
#include <stddef.h>

struct ucred {
    uint32_t uid, euid, suid, fsuid;
    uint32_t gid, egid, sgid, fsgid;
    int      ngroups;
    uint32_t groups[32];
};

struct task;
struct node;

/* capabilities that stand in for ownership */
#define CAP_CHOWN            0
#define CAP_DAC_OVERRIDE     1
#define CAP_DAC_READ_SEARCH  2
#define CAP_FOWNER           3
#define CAP_FSETID           4
#define CAP_KILL             5
#define CAP_SETGID           6
#define CAP_SETUID           7
#define CAP_MKNOD           27

/* may: 0 or -EACCES. mask: 4 read, 2 write, 1 execute / search. t = 0: the kernel itself. */
int  cred_perm(struct task *t, struct node *n, int mask);
int  cred_perm_as(const struct ucred *c, uint64_t caps, struct node *n, int mask);
int  cred_ingroup(const struct ucred *c, uint32_t gid);
int  cred_owner(struct task *t, struct node *n);             /* owns it, or CAP_FOWNER */
int  cred_sticky(struct task *t, struct node *dir, struct node *victim);   /* 0 or -EPERM */
int  cred_may_delete(struct task *t, struct node *dir, struct node *victim);  /* w+x on dir, sticky */
int  cred_kill(struct task *t, struct task *target, int sig);  /* may t signal target? */
void cred_new_node(struct node *dir, struct node *n);        /* owner of a node being made (fs.c) */
void cred_chown_tree(struct node *n, uint32_t uid, uint32_t gid);
long cred_syscall(struct task *t, long nr, uint64_t a, uint64_t b, uint64_t c);   /* set/get*id, chown */
long cred_chmod(struct task *t, struct node *n, uint32_t mode);

/* execve: what the program runs as. Call cred_exec_prepare before the new image is built
 * (it says whether AT_SECURE applies), cred_exec_commit once there's no going back. */
struct cred_exec { uint32_t euid, egid; int secure, setid; };
void cred_exec_prepare(struct task *t, struct node *n, int nosuid, struct cred_exec *ce);
void cred_exec_commit(struct task *t, const struct cred_exec *ce);

/* programs started by the kernel (Terminal, desktop apps): the session's user, or root */
void cred_spawn(struct task *t, int as_root);

/* accounts */
struct account { char name[33]; uint32_t uid, gid; char home[96], shell[64], gecos[64]; };
int  account_by_name(const char *name, struct account *a);   /* 0 or -1 */
int  account_by_uid(uint32_t uid, struct account *a);
int  account_groups(const char *name, uint32_t gid, uint32_t *out, int max);
int  account_password_set(const char *name);                  /* 1 if it has a password */
int  account_check_password(const char *name, const char *pw);   /* 1 if right */
int  account_set_password(const char *name, const char *pw);
int  account_rename(const char *old, const char *new_name, const char *gecos);
int  account_in_group(const char *name, const char *group);

/* the session */
void        session_user_init(void);        /* after the root filesystem is up */
extern volatile int session_ready;          /* set once that has run (or there is no root filesystem) */
const char *session_user(void);             /* "auxo" … */
const char *session_home(void);             /* "/home/auxo" */
uint32_t    session_uid(void);
uint32_t    session_gid(void);
const struct ucred *session_cred(void);
const char *home_file(char *buf, size_t n, const char *rel);
int         session_may(const char *path, int mask);    /* the desktop's own apps, as the user */   /* "<home>/rel" */
#define HOME_FILE(rel) ({ static char hf_[160]; home_file(hf_, sizeof hf_, rel); })
void        session_set_user(const char *name);   /* after the installer renames the account */

/* SHA-512 crypt ("$6$salt$hash"), the way /etc/shadow keeps passwords */
void sha512_crypt(const char *pw, const char *salt, int rounds, char *out, size_t n);

#endif
