#include <sys/types.h>
#include <errno.h>
#include <stdlib.h>
#include "exmon_conf.h"

static const char* _setCharpConfig(const char *confname,const char *dfl);
static int _setIntConfig(const char *confname,int dfl);

int load_exmon_conf(exmon_conf_t *conf)
{
	conf->abend_limit = _setIntConfig("ABEND_LIMIT",10);
	conf->abend_expire = _setIntConfig("ABEND_EXPIRE",300);
	conf->log_dir = _setCharpConfig("LOG_DIR",NULL);
	conf->log_fname = _setCharpConfig("LOG_FNAME",NULL);

	if( NULL == conf->log_dir || NULL == conf->log_fname ){
		return(0);
	}

	return(1);
}

static const char* _setCharpConfig(const char *confname,const char *dfl)
{
	const char *val;

	val = getenv(confname);
	if( NULL == val ){
		val = dfl;
	}

	return(val);
}

static int _setIntConfig(const char *confname,int dfl)
{
	const char *valstr;
	int val;

	valstr = getenv(confname);
	if( NULL == valstr ){
		return(dfl);
	}

	val = strtol(valstr,NULL,10);
	if( val < 0 ){
		return(dfl);
	}

	return(val);
}

