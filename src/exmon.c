#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <limits.h>
#include <stdarg.h>
#include <uni_util.h>
#include "exmon_conf.h"

typedef struct{
	char **child_cmd;
	pid_t pid;
	time_t abend_start;
	int abend_cnt;
	int fdout;
	int fderr;
} child_stat_t;

static exmon_conf_t exmon_conf;

static child_stat_t* exmon_init(char **argv);
static void exmon_loop(child_stat_t *child_stat);
static void exmon_fini(child_stat_t *child_stat);
static int handle_log(void);
static int handle_child(child_stat_t *child_stat);
static int handle_pipes(child_stat_t *child_stat);
static int handle_pipe(int fd);
static int startup_child(child_stat_t *child_stat);
static void lprintf(const char *fmt,...);
static ssize_t log_write(const char *ptr,int len);
static int get_log_prefix(char *dest);
static void usage(void);

int main(int argc,char **argv)
{
	int rc = EXIT_SUCCESS;
	child_stat_t *child_stat = NULL;

	if( argc < 2 ){
		usage();
		rc = EXIT_FAILURE;
		goto finally;
	}

	child_stat = exmon_init(argv);
	if( NULL == child_stat ){
		rc = EXIT_FAILURE;
		goto finally;
	}

	exmon_loop(child_stat);

 finally:
	exmon_fini(child_stat);
	return(rc);
}

static child_stat_t* exmon_init(char **argv)
{
	child_stat_t *ptr = NULL;
	struct sigaction sigact;

	memset(&sigact,'\0',sizeof(sigact));
	sigact.sa_handler = SIG_DFL;
	sigact.sa_flags = SA_RESTART|SA_NOCLDSTOP;
	sigaction(SIGCHLD,&sigact,NULL);

	memset(&sigact,'\0',sizeof(sigact));
	sigact.sa_handler = SIG_IGN;
	sigact.sa_flags = SA_RESTART|SA_NOCLDSTOP;
	sigaction(SIGPIPE,&sigact,NULL);

	if( !load_exmon_conf(&exmon_conf) ){
		return(NULL);
	}

	ptr = (child_stat_t*)malloc(sizeof(child_stat_t));
	if( NULL == ptr ){
		lprintf("ERROR: malloc: %s\n",strerror(errno));
		return(NULL);
	}

	ptr->child_cmd = &argv[1];
	ptr->pid = -1;
	ptr->abend_start = 0;
	ptr->abend_cnt = 0;
	ptr->fdout = -1;
	ptr->fderr = -1;
	handle_log();
	return(ptr);
}

static void exmon_loop(child_stat_t *child_stat)
{
	int ret;

	ret = startup_child(child_stat);
	while(ret){
		nax_msleep(1);
		handle_log();
		handle_pipes(child_stat);
		ret = handle_child(child_stat);
	}
}

static void exmon_fini(child_stat_t *child_stat)
{
	if( child_stat != NULL ){
		if( child_stat->fderr >= 0 ){
			close(child_stat->fderr);
			child_stat->fderr = -1;
		}

		if( child_stat->fdout >= 0 ){
			close(child_stat->fdout);
			child_stat->fdout = -1;
		}

		free(child_stat);
	}
}

static int handle_log(void)
{
	static time_t log_tm;
	int fd = -1;
	char path[PATH_MAX+1];
	time_t now;
	struct tm ltm;
	struct tm prev_tm;

	now = time(NULL);
	localtime_r(&now,&ltm);
	localtime_r(&log_tm,&prev_tm);
	if( ltm.tm_year == prev_tm.tm_year &&
		ltm.tm_mon == prev_tm.tm_mon &&
		ltm.tm_mday == prev_tm.tm_mday ){
		return(1);
	}
	log_tm = now;

	sprintf(path,"%s/%s.%04d%02d%02d",exmon_conf.log_dir,exmon_conf.log_fname,ltm.tm_year+1900,ltm.tm_mon+1,ltm.tm_mday);
	fd = open(path,O_WRONLY|O_APPEND|O_CREAT,0644);
	if( fd < 0 ){
		lprintf("ERROR: can't open %s: %s\n",path,strerror(errno));
		return(0);
	}

	dup2(fd,STDERR_FILENO);
	close(fd);
	return(1);
}

static int handle_child(child_stat_t *child_stat)
{
	pid_t pid;
	int status;
	time_t now;

	if( child_stat->fderr >= 0 || child_stat->fdout >= 0 ){
		return(1);
	}

	pid = waitpid(child_stat->pid,&status,WNOHANG);
	if( pid <= 0 ){
		return(1);
	}

	if( WIFEXITED(status) ){
		fprintf(stderr,"INFO: %d exited, status=%d\n",pid,WEXITSTATUS(status));
		if( 0 == status ){
			return(0);
		}
	}
	else if( WIFSTOPPED(status) ){
		return(1);
	}
	else if( WIFCONTINUED(status) ){
		return(1);
	}
	else if( WIFSIGNALED(status) ){
		fprintf(stderr,"INFO: %d killed by signal %d\n",pid,WTERMSIG(status));
	}

	now = time(NULL);

	// abend expire 
	if( 0 == child_stat->abend_cnt || (now - child_stat->abend_start > exmon_conf.abend_expire) ){
		child_stat->abend_cnt = 0;
		child_stat->abend_start = now;
	}

	child_stat->abend_cnt++;
	if( child_stat->abend_cnt > exmon_conf.abend_limit ){
		lprintf("ERROR: child abend %d\n",child_stat->abend_cnt);
		return(0);
	}

	return( startup_child(child_stat) );
}

static int handle_pipes(child_stat_t *child_stat)
{
	int ret;

	ret = handle_pipe(child_stat->fderr);
	if( 0 == ret ){
		close(child_stat->fderr); child_stat->fderr = -1;
	}

	ret = handle_pipe(child_stat->fdout);
	if( 0 == ret ){
		close(child_stat->fdout); child_stat->fdout = -1;
	}

	return(1);
}

static int handle_pipe(int fd)
{
	char buf[4096];
	int len;

	if( fd < 0 ){
		return(-1);
	}

	while( (len = read(fd,buf,sizeof(buf))) > 0 ){
		log_write(buf,(int)len);
	}

	if( 0 == len ){
		return(0);
	}
	else if( len < 0 && errno != EAGAIN && errno != EINTR ){
		log_write(strerror(errno),(int)strlen(strerror(errno)));
		return(-1);
	}

	return(1);
}

static int startup_child(child_stat_t *child_stat)
{
	pid_t pid;
	int out_pipe[2] = {-1,-1};
	int err_pipe[2] = {-1,-1};

	child_stat->pid = -1;
	fflush(NULL);
	if( pipe(err_pipe) < 0 ){
		fprintf(stderr,"ERROR: pipe: %s\n",strerror(errno));
		goto error;
	}

	if( pipe(out_pipe) < 0 ){
		fprintf(stderr,"ERROR: pipe: %s\n",strerror(errno));
		goto error;
	}

	pid = fork();
	if( pid < 0 ){
		fprintf(stderr,"ERROR: fork: %s\n",strerror(errno));
		goto error;
	}
	else if( pid > 0 ){
		close(err_pipe[1]);
		close(out_pipe[1]);
		fcntl(err_pipe[0],F_SETFL,O_NONBLOCK);
		fcntl(out_pipe[0],F_SETFL,O_NONBLOCK);
		child_stat->fderr = err_pipe[0];
		child_stat->fdout = out_pipe[0];
		lprintf("child start: %d\n",pid);
		return(1);
	}
	else{
		int max_desc = nax_openmax();
		int ix;

		for( ix = 0; ix <= max_desc; ix++ ){
			if( ix != out_pipe[1] && ix != err_pipe[1] ){
				close(ix);
			}
		}

		dup2(err_pipe[1],STDERR_FILENO);
		close(err_pipe[1]);
		dup2(out_pipe[1],STDOUT_FILENO);
		close(out_pipe[1]);
		execv(*(child_stat->child_cmd),child_stat->child_cmd);
		lprintf("ERROR: exec: %s\n",strerror(errno));
		exit(1);
	}

 error:
	if( err_pipe[0] > 0 ){ close(err_pipe[0]); }
	if( err_pipe[1] > 0 ){ close(err_pipe[1]); }
	if( out_pipe[0] > 0 ){ close(out_pipe[0]); }
	if( out_pipe[1] > 0 ){ close(out_pipe[1]); }
	return(0);
}

static void lprintf(const char *fmt,...)
{
	va_list ap;
	char buf[4096];
	int pre;
	int ret;

	pre = get_log_prefix(buf);
	va_start(ap,fmt);
	ret = vsnprintf(&buf[pre],sizeof(buf) - pre,fmt,ap);
	if( ret > (int)sizeof(buf) - pre ){
		ret = (int)sizeof(buf);
	}
	else{
		ret += pre;
	}
	va_end(ap);
	write(STDERR_FILENO,buf,(size_t)ret);
}

static ssize_t log_write(const char *ptr,int len)
{
	static int line_start_flag = 1;
	ssize_t wlen = 0;
	int start_ix;
	size_t line_len;
	int ix;

	if( NULL == ptr || len <= 0 ){
		return(0);
	}

	start_ix = 0;
	for( ix = 0; ix < len; ix++ ){
		if( line_start_flag ){
			char pre_buf[128];
			int pre;
			pre = get_log_prefix(pre_buf);
			write(STDERR_FILENO,&pre_buf,(size_t)pre);
			line_start_flag = 0;
		}

		if( '\n' == ptr[ix] ){
			line_len = ix - start_ix + 1;
			if( line_len > 0 ){
				wlen += write(STDERR_FILENO,&ptr[start_ix],line_len);
				start_ix = ix + 1;
			}
			line_start_flag = 1;
		}
	}

	line_len = ix - start_ix;
	if( line_len > 0 ){
		wlen += write(STDERR_FILENO,&ptr[start_ix],line_len);
	}

	return(wlen);
}

static int get_log_prefix(char *dest)
{
	time_t now;
	struct tm ltm;
	int pre;

	now = time(NULL);
	localtime_r(&now,&ltm);
	pre = sprintf(dest,"[%04d/%02d/%02d %02d:%02d:%02d] ",ltm.tm_year+1900
														,ltm.tm_mon+1
														,ltm.tm_mday
														,ltm.tm_hour
														,ltm.tm_min
														,ltm.tm_sec);
	return(pre);
}

static void usage(void)
{
	fprintf(stderr,"usage: exmon cmd args ...\n");
}

