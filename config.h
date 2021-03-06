#ifndef config_h
#define config_h

#include "uart.h"
#include "util.h"

#include <stdint.h>

typedef enum
{
	config_wlan_mode_client,
	config_wlan_mode_ap
} config_wlan_mode_t;

typedef union
{
	struct
	{
		unsigned int strip_telnet:1;
		unsigned int log_to_uart:1;
		unsigned int tsl_high_sens:1;
		unsigned int bh_high_sens:1;
		unsigned int cpu_high_speed:1;
		unsigned int wlan_power_save:1;
		unsigned int enable_cfa634:1;
		unsigned int i2c_high_speed:1;
		unsigned int log_to_buffer:1;
	} flag;

	uint32_t intval;
} config_flags_t;

typedef struct
{
	unsigned int using_logbuffer:1;
} config_options_t;

void			config_flags_to_string(string_t *);
bool_t			config_flags_change(const string_t *, bool_t add);

bool_t			config_get_string(const string_t *id, int index1, int index2, string_t *value);
bool_t			config_get_int(const string_t *id, int index1, int index2, int *value);
bool_t			config_set_string(const string_t *id, int index1, int index2, const string_t *value, int value_offset, int value_length);
bool_t			config_set_int(const string_t *id, int index1, int index2, int value);
unsigned int	config_delete(const string_t *id, int index1, int index2, bool_t wildcard);

bool_t			config_read(void);
unsigned int	config_write(void);
void			config_dump(string_t *);

extern config_flags_t flags_cache;
extern config_options_t config_options;

always_inline static config_flags_t config_flags_get(void)
{
	return(flags_cache);
}

always_inline static attr_pure bool_t config_uses_logbuffer(void)
{
	return(config_options.using_logbuffer != 0);
}

#endif
