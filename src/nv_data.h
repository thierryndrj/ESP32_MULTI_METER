#ifndef NVDATA_H_
#define NVDATA_H_

//#include "ina226.h"

#define SAMPLE_SECS_MIN       1
#define SAMPLE_SECS_MAX       30

#define SCALE_HI	0 // Full scale = 1.64A
#define SCALE_LO	1 // Full scale = 78mA

#define DEFAULT_SCALE		    SCALE_HI
#define DEFAULT_SAMPLE_SECS		SAMPLE_SECS_MIN  

#define DEFAULT_CFG_INDEX    	2

typedef struct {
	String ssid;
	String password;
	uint16_t scale;
	uint16_t sampleSecs;
	} OPTIONS_t;

typedef struct {
	uint16_t reg;
	uint16_t sampleRate;
	} CONFIG_t;

#define NUM_CFG 10

typedef struct {
	CONFIG_t cfg[NUM_CFG];
	int cfgIndex;
} CONFIG_TABLE_t;

extern OPTIONS_t Options;
extern CONFIG_TABLE_t ConfigTbl;

void nv_options_store(OPTIONS_t &options);
void nv_options_load(OPTIONS_t &options);
void nv_options_reset(OPTIONS_t &options);
void nv_options_print(OPTIONS_t &options);

void nv_config_store(CONFIG_TABLE_t &configTbl);
void nv_config_load(CONFIG_TABLE_t &configTbl);
void nv_config_reset(CONFIG_TABLE_t &configTbl);
void nv_config_print(CONFIG_TABLE_t &configTbl);

#endif
