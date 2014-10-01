#include "dsh.h"

void seize_tty(pid_t callingprocess_pgid); /* Grab control of the terminal for the calling process pgid.  */
void continue_job(job_t *j); /* resume a stopped job */
void spawn_job(job_t *j, bool fg); /* spawn a new job */
void call_getcwd();

void add_new_job(job_t *new_job);
process_t *find_process(int pid);
job_t *find_job(int job_id);
job_t* getLastSuspendedJob();
void wait_pid_help(job_t *j, bool fg);
bool free_job(job_t *j);

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

  if(fg) // if fg is set
    seize_tty(j->pgid); // assign the terminal

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
  int next_fd[2];
  int prev_fd[2];

  add_new_job(j);

  for(p = j->first_process; p; p = p->next) {

    /* YOUR CODE HERE? */
    /* Builtin commands are already taken care earlier */

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
        print_job(j);

        new_child(j, p, fg);

        // piping
        if (p!= j->first_process){
          dup2(prev_fd[0], STDIN_FILENO);
          close(prev_fd[0]);
          close(prev_fd[1]);
        }

        if (p->next!= NULL){
          dup2(next_fd[1],STDOUT_FILENO);
          close(next_fd[1]);
          close(next_fd[0]);
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

        if (p != j->first_process) {
          close(prev_fd[0]);
          close(prev_fd[1]);
        }
        // if(fg){
        //   // wait(NULL);
        //   wait_pid_help(j);
        // }      
        if (p->next != NULL) {
          prev_fd[0] = next_fd[0];
          prev_fd[1] = next_fd[1];
        }
        /* YOUR CODE HERE?  Parentq-side code for new process.  */
    }
    wait_pid_help(j, fg);
  }
}

/* Sends SIGCONT signal to wake up the blocked job */
void continue_job(job_t *j) 
{
  if(kill(-j->pgid, SIGCONT) < 0)
    perror("kill(SIGCONT)");
}

/* Wait for child in foreground to finish and exit */
void wait_pid_help(job_t *j, bool fg) {
  int status, pid;
  while((pid = waitpid(-1, &status, WUNTRACED)) > 0) {
    process_t *p = find_process(pid);
    if(WIFEXITED(status) || WIFSIGNALED(status)) {
      p->completed = true;
      fflush(stdout);
    } else if (WIFSTOPPED(status)) {
        p->stopped = true;
        j->notified = true;
      //j->bg = true;
    }

    if(job_is_stopped(j) && isatty(STDIN_FILENO)) {
        seize_tty(getpid());
      break;
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

job_t *getLastSuspendedJob() {
  job_t *jobs = jobs_list;
  job_t* lastSuspended = NULL;

  while(jobs != NULL) {

    if(jobs->notified) {
      lastSuspended = jobs;
    }
    jobs = jobs->next;
  }
  return lastSuspended;
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

    job_t *job = jobs_list;
    int job_count = 1;

    while(job != NULL) {
      printf("[%d] (%d)", job_count, job->pgid);

      if(job_is_completed(job)) {
        printf(" Job completed: ");
      } else if(job_is_stopped(job)){
        printf(" Job stopped: ");
      } else {
        printf(" Job running in");
        if(job -> bg) {
          printf(" bg: ");
        }
        else {
          printf(" fg: ");
        }
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
          jobs_list = job;
        }
        else {
          prev -> next = job -> next;
          free_job(prev);
          job = prev -> next;
        }
      }
      else {
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
    int job_id = atoi(argv[1]);
    job_t *job = find_job(job_id);

    fflush(stdout);
    continue_job(job);
    job -> bg = true;
    job -> notified = false;

    return true;
  }
  else if (!strcmp("fg", argv[0])) {
    int job_id;
    job_t *job;

    // if no arguments specified, continue last job stopped
    if(argc == 1) {
      job = getLastSuspendedJob();
      job_id = job->pgid;
    }
    else {
      //printf("getting job\n");
      job_id = atoi(argv[1]);
      job = find_job(job_id);
    }

    fflush(stdout);
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
  new_job->next = NULL;
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
  return "dsh$ ";
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
        /* Your code goes here */
        /* You need to loop through jobs list since a command line can contain ;*/
        /* Check for built-in commands */
        /* If not built-in */
            /* If job j runs in foreground */
            /* spawn_job(j,true) */
            /* else */
            /* spawn_job(j,false) */


    job_t * current_job = j;
    while (current_job != NULL) {
      int argc = current_job->first_process->argc;
      char** argv = current_job->first_process->argv;
      if (!builtin_cmd(current_job, argc, argv)) {        
        spawn_job(current_job, !(current_job->bg));
      }
      // printf("test\n");
      current_job = current_job->next;
    }

        /* Only for debugging purposes to show parser output; turn off in the
         * final code */
   // if(PRINT_INFO) print_job(j);    
  }
}
