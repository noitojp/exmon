#ifndef __EXMON_CONF_H_
#define __EXMON_CONF_H_

typedef struct{
	int abend_limit;
	time_t abend_expire;
	const char *log_dir;
	const char *log_fname;
} exmon_conf_t;

#ifdef __cplusplus
extern "C" {
#endif

extern int load_exmon_conf(exmon_conf_t *conf);

#ifdef __cplusplus
}
#endif

#endif
