#include "dsh.h"

void seize_tty(pid_t callingprocess_pgid); /* Grab control of the terminal for the calling process pgid.  */
void continue_job(job_t *j); /* resume a stopped job */
void spawn_job(job_t *j, bool fg); /* spawn a new job */
void call_getcwd();
void edit_background_ps();

void add_new_job(job_t *new_job);
process_t *find_process(int pid);
job_t *find_job(int job_id);
void wait_pid_help(job_t *j, bool fg);
bool free_job(job_t *j);
job_t *get_last_suspended(job_t* curr);

job_t *first_job;
bool first = true;
job_t *jobs_list = NULL;

/* Sets the process group id for a given job and process */
int set_child_pgid(job_t *j, process_t *p)
{
  if (j->pgid < 0) { /* first child: use its pid for job pgid */
    j->pgid = p->pid;
  }
  return(setpgid(p->pid,j->pgid));
}

/* Creates the context for a new child by setting the pid, pgid and tcsetpgrp */
void new_child(job_t *j, process_t *p, bool fg)
{
 /* establish a new process group, and put the child in
  * foreground if requested
  */

 /* Put the process into the process group and give the process
  * group the terminal, if appropriate.  This has to be done both by
  * the dsh and in the individual child processes because of
  * potential race conditions.  
  * */

  p->pid = getpid();

  /* also establish child process group in child to avoid race (if parent has not done it yet). */
  set_child_pgid(j, p);
  if(fg) {// if fg is set
    seize_tty(j->pgid); // assign the terminal
  }
  /* Set the handling for job control signals back to the default. */
  signal(SIGTTOU, SIG_DFL);
}

/* Spawning a process with job control. fg is true if the 
 * newly-created process is to be placed in the foreground. 
 * (This implicitly puts the calling process in the background, 
 * so watch out for tty I/O after doing this.) pgid is -1 to 
 * create a new job, in which case the returned pid is also the 
 * pgid of the new job.  Else pgid specifies an existing job's 
 * pgid: this feature is used to start the second or 
 * subsequent processes in a pipeline.
 * */

 void spawn_job(job_t *j, bool fg) 
 {
  pid_t pid;
  process_t *p;
  int prev_fd[2];

  add_new_job(j);

  for(p = j->first_process; p; p = p->next) {

    /* YOUR CODE HERE? */
    /* Builtin commands are already taken care earlier */

    int next_fd[2];
    if (p->next != NULL) {
      pipe(next_fd);
    }

    switch (pid = fork()) {
      case -1: /* fork failure */
        perror("Error: failed to fork");
        exit(EXIT_FAILURE);

      case 0: /* child process  */
        p->pid = getpid();      
        /* YOUR CODE HERE?  Child-side code for new process. */
        new_child(j, p, fg);

        // piping
        if (p!= j->first_process){
          close(prev_fd[1]);
          dup2(prev_fd[0], STDIN_FILENO);
          close(prev_fd[0]);
        }

        if (p->next!= NULL){
          close(next_fd[0]);
          dup2(next_fd[1],STDOUT_FILENO);
          close(next_fd[1]);
        }


        // I/O Redirection
        int newInFD;
        int newOutFD;
        if (p->ifile != NULL && p->ofile != NULL) {
          newInFD = open(p->ifile, O_RDONLY);
          dup2(newInFD, STDIN_FILENO);
          newOutFD = open(p->ofile, O_APPEND | O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IXUSR);
          dup2(newOutFD, STDOUT_FILENO);
          execvp(p->argv[0], p->argv);
          close(newInFD);
          close(newOutFD);
        } else if (p->ifile != NULL) {
          newInFD = open(p->ifile, O_RDONLY);
          dup2(newInFD, STDIN_FILENO);
          execvp(p->argv[0], p->argv);
          close(newInFD);
        } else if (p->ofile != NULL) {
          newOutFD = open(p->ofile, O_APPEND | O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | S_IXUSR);
          dup2(newOutFD, STDOUT_FILENO);
          execvp(p->argv[0], p->argv);
          close(newOutFD);
        } else {
          execvp(p->argv[0], p->argv);          
        }

        perror("New child should have done an exec");
        
        exit(EXIT_FAILURE);  /* NOT REACHED */
        break;    /* NOT REACHED */

      default: /* parent */
        /* establish child process group */

        p->pid = pid;
        set_child_pgid(j, p);

        if (p->pid == j->pgid) printf("%d(Launched): %s\n", j->pgid, j->commandinfo);

        if (p != j->first_process) {
          close(prev_fd[0]);
          close(prev_fd[1]);
        }
          prev_fd[0] = next_fd[0];
          prev_fd[1] = next_fd[1];
        /* YOUR CODE HERE?  Parent-side code for new process.  */
    }
  }
  wait_pid_help(j, fg);
}

/* Sends SIGCONT signal to wake up the blocked job */
void continue_job(job_t *j) 
{
  if(kill(-j->pgid, SIGCONT) < 0)
    perror("kill(SIGCONT)");
}

/* Wait for child in foreground to finish and exit */
void wait_pid_help(job_t *j, bool fg) {
  if (fg) {
    int status, pid;
    while((pid = waitpid(-1, &status, WUNTRACED)) > 0) {
      process_t *p = find_process(pid);
      if(WIFEXITED(status)) { //ctrl d
        p->completed = true;
        fflush(stdout);
      }
      else if (WIFSIGNALED(status)) { //ctrl c
        p->completed = true;
        fflush(stdout);
      }
      else if (WIFSTOPPED(status)) { //ctrl z
        p->stopped = true;
        j->notified = true;
        j->bg = true;
      }

      if(job_is_stopped(j) && isatty(STDIN_FILENO)) {
        seize_tty(getpid());
        break;
      }
    }
  }
}

job_t *find_job(int job_id) {
  job_t *jobs = jobs_list;

  while(jobs != NULL) {
    if(jobs -> pgid == job_id) {
      return jobs;
    }
    jobs = jobs -> next;
  }
  return NULL;
}

process_t *find_process(int pid) {
  job_t *jobs = jobs_list;
  while(jobs != NULL) {
    process_t *process = jobs -> first_process;
    while(process != NULL) {
      if(process -> pid == pid)
        return process;
      process = process -> next;
    }
    jobs = jobs -> next;
  }

  return NULL;
}

void edit_background_ps() {
  int pid, status;
  while ((pid = waitpid(WAIT_ANY, &status, WNOHANG | WUNTRACED)) > 0) {
    process_t *p = find_process(pid);
    p->status = status;
    if (WIFEXITED(status)) {
        p->completed = true; 
    }
    if (WIFSTOPPED(status)) { 
      p->stopped = true; 
    }
    if (WIFCONTINUED(status)) { 
      p->stopped = false; 
    }
    if (WIFSIGNALED(status)) { 
      p->completed = true; 
    }
  }
}
/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 * it immediately.  
 */
bool builtin_cmd(job_t *last_job, int argc, char **argv) 
{
  if (!strcmp(argv[0], "quit")) {
    /* Your code here */
    exit(EXIT_SUCCESS);
    // Flags for last_job?
    last_job->first_process->completed = true;
    last_job->first_process->status = 0;
    return true;
  }
  else if (!strcmp("jobs", argv[0])) {

    edit_background_ps();

    job_t *job = jobs_list;
    job = jobs_list;
    int job_count = 1;

    while(job != NULL) {
      printf("%d", job->pgid);

      if(job_is_completed(job)) {
        printf("(Completed): ");
      } else if(job_is_stopped(job)){
        printf("(Stopped): ");
      } else {
        printf("(Running):");
      }

      printf("%s\n", job -> commandinfo);
      job_count++;
      job = job -> next;
    }

    // remove completed jobs
    job = jobs_list;
    job_t *prev;
    while(job != NULL) {
      if(job_is_completed(job)) {
        if(job == jobs_list) {
          job = jobs_list -> next;          
          free_job(jobs_list);
          jobs_list = job;
          first_job = jobs_list;
        }
        else {
          prev -> next = job -> next;
          free_job(job);
          job = prev -> next;
        }
      }
      else{
        prev = job;
        job = job->next;
      }
    }


    fflush(stdout);
    return true;
  }
  else if (!strcmp("cd", argv[0])) {
    if(argc == 1){
      if(chdir(getenv("HOME")) < 0) {
        // error...
        return false;
      }
      call_getcwd();
      return true;
    }

    if (chdir(argv[1]) < 0) {
      // error...
      return false;
    }
    call_getcwd();
    return true;
  }
  else if (!strcmp("bg", argv[0])) {
    int job_id;
    job_t *job;

    if(argc == 1) {
      return true;
    }
    else {
      job_id = atoi(argv[1]);
      job = find_job(job_id);
    }

    continue_job(job);
    job -> bg = true;
    job -> notified = false;
    process_t* p = find_process(job->pgid);
    p->stopped = false;

    return true;
  }
  else if (!strcmp("fg", argv[0])) {
    int job_id;
    job_t *job;

    // if no arguments specified, continue last job stopped
    if(argc == 1) {
      job = get_last_suspended(first_job);
      if (job == NULL) return true;
      job_id = job->pgid;
    }
    else {
      job_id = atoi(argv[1]);
      job = find_job(job_id);
      if (job == NULL) return true;
    }

    continue_job(job);
    job -> bg = false;
    job -> notified = false;
    process_t* p = find_process(job->pgid);
    p->stopped = false;

    seize_tty(job_id);
    wait_pid_help(job, true);

    return true;
  }
  return false;       /* not a builtin command */
}

job_t* get_last_suspended(job_t *curr){
  job_t* last_suspended;
  while(curr != NULL) {
    if (job_is_stopped(curr)) {
      last_suspended = curr;
    }
    curr = curr->next;
  }
  return last_suspended;
}

void add_new_job(job_t *new_job) {
  if(new_job == NULL) {
    return;
  }

  if(jobs_list == NULL) {
    jobs_list = new_job;
  }
  else {
    job_t *curr = jobs_list;
    while(curr -> next != NULL) {
      curr = curr -> next;
    }
    curr -> next = new_job;
  }
}

void call_getcwd ()
{
  char * cwd;
  cwd = getcwd(0, 0);
  printf("dir: %s\n", cwd);
  free(cwd);
}

/* Build prompt messaage */
char* promptmsg() 
{
  /* Modify this to include pid */
  char prompt[20];
  char pid[10];
  strcpy(prompt,"dsh-");
  snprintf(pid, 10,"%d",getpid());
  strcat(prompt, pid);
  return strcat(prompt,"$ ");
}

int main() 
{
  init_dsh();
  DEBUG("Successfully initialized\n");

  while(1) {
    job_t *j = NULL;
    if(!(j = readcmdline(promptmsg()))) {
      if (feof(stdin)) { /* End of file (ctrl-d) */
        fflush(stdout);
        printf("\n");
        exit(EXIT_SUCCESS);
      }
      continue; /* NOOP; user entered return or spaces with return */
    }
    if (first) {
      first_job = j;
      first = false;
    }
    job_t * current_job = j;
    job_t * new_job = j;

    while (current_job != NULL) {
      new_job = current_job;
      current_job = current_job -> next;
      new_job->next = NULL;
      
      int argc = new_job->first_process->argc;
      char** argv = new_job->first_process->argv;
      if (!builtin_cmd(new_job, argc, argv)) {
        spawn_job(new_job, !(new_job->bg));
      }
    }
  }
}
