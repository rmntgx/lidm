#include <grp.h>
#include <pwd.h>
#include <security/pam_misc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "auth.h"
#include "config.h"
#include "sessions.h"
#include "ui.h"
#include "unistd.h"
#include "util.h"

int pam_conversation(int num_msg,
                     const struct pam_message** msg,
                     struct pam_response** resp,
                     void* appdata_ptr) {
  struct pam_response* reply =
      (struct pam_response*)malloc(sizeof(struct pam_response) * num_msg);
  for (size_t i = 0; i < num_msg; i++) {
    reply[i].resp = NULL;
    reply[i].resp_retcode = 0;
    if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF ||
        msg[i]->msg_style == PAM_PROMPT_ECHO_ON) {
      char* input = (char*)appdata_ptr;
      reply[i].resp = strdup(input);
    }
  }
  *resp = reply;
  return PAM_SUCCESS;
}

#define CHECK_PAM_RET(call) \
  ret = (call);             \
  if (ret != PAM_SUCCESS) { \
    pam_end(pamh, ret);     \
    return NULL;            \
  }

void clear_screen() {
  printf("\x1b[H\x1b[J");
}

pam_handle_t* get_pamh(char* user, char* passwd) {
  pam_handle_t* pamh = NULL;
  struct pam_conv pamc = {pam_conversation, (void*)passwd};
  int ret;

  CHECK_PAM_RET(pam_start("login", user, &pamc, &pamh))
  CHECK_PAM_RET(pam_authenticate(pamh, 0))
  CHECK_PAM_RET(pam_acct_mgmt(pamh, 0))
  CHECK_PAM_RET(pam_setcred(pamh, PAM_ESTABLISH_CRED))
  CHECK_PAM_RET(pam_open_session(pamh, 0))
  CHECK_PAM_RET(pam_setcred(pamh, PAM_REINITIALIZE_CRED))

  return pamh;
}
#undef CHECK_PAM_RET

void* shmalloc(size_t size) {
  return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS,
              -1, 0);
}

void sourceFileTry(char* file) {
  FILE* file2source = fopen(file, "r");
  if (file2source == NULL) return;

  char* line = NULL;
  size_t len = 0;
  ssize_t read;

  while ((read = getline(&line, &len, file2source)) != -1) {
    if (read == 0 || (read > 0 && *line == '#')) continue;
    if (line[read - 1] == '\n') line[read - 1] = '\0';

    /* printf("Retrieved line of length %zu:\n", read); */
    /* printf("%s\n", line); */
    for (size_t i = 1; i < read; i++) {
      if (line[i] == '=') {
        /* printf("FOUND '='!\n"); */
        line[i] = '\0';
        setenv(line, &line[i + 1], 1);
        break;
      }
    }
  }

  if (line) free(line);
  fclose(file2source);
}

void moarEnv(char* user,
             struct session session,
             struct passwd* pw,
             struct behavior* behavior) {
  if (chdir(pw->pw_dir) == -1) print_errno("can't chdir to user home");

  setenv("HOME", pw->pw_dir, true);
  setenv("USER", pw->pw_name, true);
  setenv("SHELL", pw->pw_shell, true);
  // TERM
  setenv("LOGNAME", pw->pw_name, true);
  // MAIL?

  // PATH?

  char* xdg_session_type;
  if (session.type == SHELL) xdg_session_type = "tty";
  if (session.type == XORG) xdg_session_type = "x11";
  if (session.type == WAYLAND) xdg_session_type = "wayland";
  setenv("XDG_SESSION_TYPE", xdg_session_type, true);

  printf("\n\n\n\n\x1b[1m");
  for (size_t i = 0; i < behavior->source.length; i++) {
    /* printf("DEBUG(source)!!!! %d %s\n", i, (char*)vec_get(&behavior->source,
     * i)); */
    sourceFileTry((char*)vec_get(&behavior->source, i));
  }
  /* printf("\n"); */
  if (pw->pw_dir) {
    uint home_len = strlen(pw->pw_dir);
    for (size_t i = 0; i < behavior->user_source.length; i++) {
      char* file2sourcepath = (char*)vec_get(&behavior->user_source, i);
      char* newbuf =
          malloc(home_len + strlen(file2sourcepath) + 2); // nullbyte and slash
      if (newbuf == NULL) continue;                       // can't bother
      strcpy(newbuf, pw->pw_dir);
      newbuf[home_len] = '/'; // assume pw_dir doesn't start with '/' :P
      strcpy(&newbuf[home_len + 1], file2sourcepath);

      /* printf("DEBUG(user_source)!!!! %d %s\n", i, newbuf); */
      sourceFileTry(newbuf);
      free(newbuf);
    }
  }

  /*char *buf;*/
  /*size_t bsize = snprintf(NULL, 0, "/run/user/%d", pw->pw_uid) + 1;*/
  /*buf = malloc(bsize);*/
  /*snprintf(buf, bsize, "/run/user/%d", pw->pw_uid);*/
  /*setenv("XDG_RUNTIME_DIR", buf, true);*/
  /*setenv("XDG_SESSION_CLASS", "user", true);*/
  /*setenv("XDG_SESSION_ID", "1", true);*/
  /*setenv("XDG_SESSION_DESKTOP", , true);*/
  /*setenv("XDG_SEAT", "seat0", true);*/
}

bool launch(char* user,
            char* passwd,
            struct session session,
            void (*cb)(void),
            struct behavior* behavior) {
  struct passwd* pw = getpwnam(user);
  if (pw == NULL) {
    print_err("could not get user info");
    return false;
  }

  pam_handle_t* pamh = get_pamh(user, passwd);
  if (pamh == NULL) {
    print_err("error on pam authentication");
    return false;
  }

  bool* reach_session = shmalloc(sizeof(bool));
  if (reach_session == NULL) {
    perror("error allocating shared memory");
    return false;
  }
  *reach_session = false;

  uint pid = fork();
  if (pid == 0) { // child
    char* TERM = NULL;
    char* _GETTERM = getenv("TERM");
    if (_GETTERM != NULL) strcln(&TERM, _GETTERM);
    if (clearenv() != 0) {
      print_errno("clearenv");
      _exit(EXIT_FAILURE);
    }

    char** envlist = pam_getenvlist(pamh);
    if (envlist == NULL) {
      print_errno("pam_getenvlist");
      _exit(EXIT_FAILURE);
    }
    for (size_t i = 0; envlist[i] != NULL; i++) {
      putenv(envlist[i]);
    }
    // FIXME: path hotfix
    putenv("PATH=/bin:/usr/bin");
    if (TERM != NULL) {
      setenv("TERM", TERM, true);
      free(TERM);
    }

    free(envlist);
    moarEnv(user, session, pw, behavior);

    // TODO: chown stdin to user
    // does it inherit stdin from parent and
    // does parent need to reclaim it after
    // this dies?

    if (setgid(pw->pw_gid) == -1) {
      print_errno("setgid");
      _exit(EXIT_FAILURE);
    }
    if (initgroups(user, pw->pw_gid) == -1) {
      print_errno("initgroups");
      _exit(EXIT_FAILURE);
    }

    if (setuid(pw->pw_uid) == -1) {
      perror("setuid");
      _exit(EXIT_FAILURE);
    }

    if (cb != NULL) cb();

    *reach_session = true;

    // TODO: these will be different due to TryExec
    // and, Exec/TryExec might contain spaces as args
    printf("\x1b[0m");
    if (session.type == SHELL) {
      clear_screen();
      fflush(stdout);
      execlp(session.exec, session.exec, NULL);
    } else if (session.type == XORG || session.type == WAYLAND) {
      clear_screen();
      fflush(stdout);
      execlp(session.exec, session.exec, NULL);
    }
    perror("execl error");
    fprintf(stderr, "failure calling session\n");
  } else {
    waitpid(pid, NULL, 0);

    pam_setcred(pamh, PAM_DELETE_CRED);
    pam_close_session(pamh, 0);
    pam_end(pamh, PAM_SUCCESS);

    if (*reach_session == false) {
      return false;
    } else
      exit(0);
  }

  return true;
}
