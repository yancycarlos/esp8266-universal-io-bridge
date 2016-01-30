#include "application.h"

#include "gpios.h"
#include "stats.h"
#include "util.h"
#include "user_main.h"
#include "config.h"
#include "uart.h"
#include "i2c.h"
#include "i2c_sensor.h"
#include "display.h"

#if IMAGE_OTA == 1
#include "ota.h"
#endif

#include <user_interface.h>
#include <c_types.h>
#include <sntp.h>

#include <stdlib.h>

typedef enum
{
	ws_inactive,
	ws_scanning,
	ws_finished,
} wlan_scan_state_t;

typedef struct
{
	const char		*command1;
	const char		*command2;
	app_action_t	(*function)(const string_t *, string_t *);
	const char		*description;
} application_function_table_t;

static const application_function_table_t application_function_table[];
static wlan_scan_state_t wlan_scan_state = ws_inactive;

irom app_action_t application_content(const string_t *src, string_t *dst)
{
	const application_function_table_t *tableptr;

	if(config.stat_trigger_gpio >= 0)
		gpios_trigger_output(config.stat_trigger_gpio);

	if(parse_string(0, src, dst) != parse_ok)
		return(app_action_empty);

	for(tableptr = application_function_table; tableptr->function; tableptr++)
		if(string_match(dst, tableptr->command1) ||
				string_match(dst, tableptr->command2))
			break;

	if(tableptr->function)
	{
		string_clear(dst);
		return(tableptr->function(src, dst));
	}

	string_cat(dst, ": command unknown\n");
	return(app_action_error);
}

irom static app_action_t application_function_config_dump(const string_t *src, string_t *dst)
{
	config_read(&tmpconfig);
	config_dump(dst, &tmpconfig);
	return(app_action_normal);
}

irom static app_action_t application_function_config_write(const string_t *src, string_t *dst)
{
	config_write(&config);
	string_cat(dst, "config write done\n");
	return(app_action_normal);
}

irom static app_action_t application_function_help(const string_t *src, string_t *dst)
{
	const application_function_table_t *tableptr;

	for(tableptr = application_function_table; tableptr->function; tableptr++)
		string_format(dst, "> %s/%s: %s\n",
				tableptr->command1, tableptr->command2,
				tableptr->description);

	return(app_action_normal);
}

irom static app_action_t application_function_quit(const string_t *src, string_t *dst)
{
	return(app_action_disconnect);
}

irom static app_action_t application_function_reset(const string_t *src, string_t *dst)
{
	return(app_action_reset);
}

irom static app_action_t application_function_stats(const string_t *src, string_t *dst)
{
	stats_generate(dst);
	return(app_action_normal);
}

irom static app_action_t application_function_bridge_tcp_port(const string_t *src, string_t *dst)
{
	int tcp_port;

	if(parse_int(1, src, &tcp_port, 0) == parse_ok)
	{
		if((tcp_port < 1) || (tcp_port > 65535))
		{
			string_format(dst, "bridge-tcp-port: invalid port %d\n", tcp_port);
			return(app_action_error);
		}

		config.bridge_tcp_port = (uint16_t)tcp_port;
	}

	string_format(dst, "bridge-tcp_port: %d\n", config.bridge_tcp_port);

	return(app_action_normal);
}

irom static app_action_t application_function_uart_baud_rate(const string_t *src, string_t *dst)
{
	int baud_rate;

	if(parse_int(1, src, &baud_rate, 0) == parse_ok)
	{
		if((baud_rate < 150) || (baud_rate > 1000000))
		{
			string_format(dst, "uart-baud: invalid baud rate: %d\n", baud_rate);
			return(app_action_error);
		}

		config.uart.baud_rate = baud_rate;
	}

	string_format(dst, "uart-baud: %d\n", config.uart.baud_rate);

	return(app_action_normal);
}

irom static app_action_t application_function_uart_data_bits(const string_t *src, string_t *dst)
{
	int data_bits;

	if(parse_int(1, src, &data_bits, 0) == parse_ok)
	{
		if((data_bits < 5) || (data_bits > 8))
		{
			string_format(dst, "uart-data: invalid data bits: %d\n", data_bits);
			return(app_action_error);
		}

		config.uart.data_bits = data_bits;
	}

	string_format(dst, "uart-data: %d\n", config.uart.data_bits);

	return(app_action_normal);
}

irom static app_action_t application_function_uart_stop_bits(const string_t *src, string_t *dst)
{
	int stop_bits;

	if(parse_int(1, src, &stop_bits, 0) == parse_ok)
	{
		if((stop_bits < 1) || (stop_bits > 2))
		{
			string_format(dst, "uart-stop: stop bits out of range: %d\n", stop_bits);
			return(app_action_error);
		}

		config.uart.stop_bits = stop_bits;
	}

	string_format(dst, "uart-stop: %d\n", config.uart.stop_bits);

	return(app_action_normal);
}

irom static app_action_t application_function_uart_parity(const string_t *src, string_t *dst)
{
	uart_parity_t parity;

	if(parse_string(1, src, dst) == parse_ok)
	{
		parity = uart_string_to_parity(dst);

		if((parity < parity_none) || (parity >= parity_error))
		{
			string_cat(dst, ": invalid parity\n");
			return(app_action_error);
		}

		config.uart.parity = parity;
	}

	string_copy(dst, "uart-parity: ");
	uart_parity_to_string(dst, config.uart.parity);
	string_cat(dst, "\n");

	return(app_action_normal);
}

static int i2c_address = 0;

irom static app_action_t application_function_i2c_address(const string_t *src, string_t *dst)
{
	int intin;

	if(parse_int(1, src, &intin, 16) == parse_ok)
	{
		if((intin < 2) || (intin > 127))
		{
			string_format(dst, "i2c-address: invalid address 0x%02x\n", intin);
			return(app_action_error);
		}

		i2c_address = intin;
	}

	string_format(dst, "i2c-address: address: 0x%02x\n", i2c_address);

	return(app_action_normal);
}

irom static app_action_t application_function_i2c_delay(const string_t *src, string_t *dst)
{
	int intin;

	if(parse_int(1, src, &intin, 0) == parse_ok)
	{
		if((intin < 0) || (intin > 100))
		{
			string_format(dst, "i2c-delay: invalid delay %d\n", intin);
			return(app_action_error);
		}

		config.i2c_delay = intin;
	}

	string_format(dst, "i2c-delay: delay: %d\n", config.i2c_delay);

	return(app_action_normal);
}

irom static app_action_t application_function_i2c_read(const string_t *src, string_t *dst)
{
	int size, current;
	i2c_error_t error;
	uint8_t bytes[32];

	if(parse_int(1, src, &size, 0) != parse_ok)
	{
		string_cat(dst, "i2c-read: missing byte count\n");
		return(app_action_error);
	}

	if(size > (int)sizeof(bytes))
	{
		string_format(dst, "i2c-read: read max %d bytes\n", sizeof(bytes));
		return(app_action_error);
	}

	if((error = i2c_receive(i2c_address, size, bytes)) != i2c_error_ok)
	{
		string_cat(dst, "i2c_read");
		i2c_error_format_string(dst, error);
		string_cat(dst, "\n");
		i2c_reset();
		return(app_action_error);
	}

	string_format(dst, "i2c_read: read %d bytes from %02x:", size, i2c_address);

	for(current = 0; current < size; current++)
		string_format(dst, " %02x", bytes[current]);

	string_cat(dst, "\n");

	return(app_action_normal);
}

irom static app_action_t application_function_i2c_write(const string_t *src, string_t *dst)
{
	i2c_error_t error;
	static uint8_t bytes[32];
	int current, out;

	for(current = 0; current < (int)sizeof(bytes); current++)
	{
		if(parse_int(current + 1, src, &out, 16) != parse_ok)
			break;

		bytes[current] = (uint8_t)(out & 0xff);
	}

	if((error = i2c_send(i2c_address, current, bytes)) != i2c_error_ok)
	{
		string_cat(dst, "i2c_write");
		i2c_error_format_string(dst, error);
		string_cat(dst, "\n");
		i2c_reset();
		return(app_action_error);
	}

	string_format(dst, "i2c_write: written %d bytes to %02x\n", current, i2c_address);

	return(app_action_normal);
}

irom static app_action_t application_function_i2c_reset(const string_t *src, string_t *dst)
{
	i2c_error_t error;

	if((error = i2c_reset()) != i2c_error_ok)
	{
		string_cat(dst, "i2c-reset: ");
		i2c_error_format_string(dst, error);
		string_cat(dst, "\n");
		return(app_action_error);
	}

	string_cat(dst, "i2c_reset: ok\n");

	return(app_action_normal);
}

irom static app_action_t application_function_i2c_sensor_read(const string_t *src, string_t *dst)
{
	int intin;
	i2c_sensor_t sensor;

	if((parse_int(1, src, &intin, 0)) != parse_ok)
	{
		string_cat(dst, "> invalid i2c sensor\n");
		return(app_action_error);
	}

	sensor = (i2c_sensor_t)intin;

	if(!i2c_sensor_read(dst, sensor, true))
	{
		string_clear(dst);
		string_format(dst, "> invalid i2c sensor: %d\n", (int)sensor);
		return(app_action_error);
	}

	string_cat(dst, "\n");

	return(app_action_normal);
}

irom static app_action_t application_function_i2c_sensor_calibrate(const string_t *src, string_t *dst)
{
	int intin;
	i2c_sensor_t sensor;
	double factor;
	double offset;

	if(parse_int(1, src, &intin, 0) != parse_ok)
	{
		string_cat(dst, "> invalid i2c sensor\n");
		return(app_action_error);
	}

	sensor = (i2c_sensor_t)intin;

	if(parse_float(2, src, &factor) != parse_ok)
	{
		string_cat(dst, "> invalid factor\n");
		return(app_action_error);
	}

	if(parse_float(3, src, &offset) != parse_ok)
	{
		string_cat(dst, "> invalid offset\n")
		return(app_action_error);
	}

	if(!i2c_sensor_setcal(sensor, factor, offset))
	{
		string_format(dst, "> invalid i2c sensor: %d\n", (int)sensor);
		return(app_action_error);
	}

	string_format(dst, "> i2c sensor %d calibration set to factor ", (int)sensor);
	string_double(dst, config.i2c_sensors.sensor[sensor].calibration.factor, 4, 1e10);
	string_cat(dst, ", offset: ");
	string_double(dst, config.i2c_sensors.sensor[sensor].calibration.offset, 4, 1e10);
	string_cat(dst, "\n");

	return(app_action_normal);
}

irom static app_action_t application_function_i2c_sensor_dump(const string_t *src, string_t *dst)
{
	i2c_sensor_t sensor;
	int option;
	bool_t all, verbose;
	int original_length = string_length(dst);

	all = false;
	verbose = false;

	if(parse_int(1, src, &option, 0) == parse_ok)
	{
		switch(option)
		{
			case(2):
				verbose = true;
			case(1):
				all = true;
			default:
				(void)0;
		}
	}

	for(sensor = 0; sensor < i2c_sensor_size; sensor++)
	{
		if(all || i2c_sensor_detected(sensor))
		{
			i2c_sensor_read(dst, sensor, verbose);
			string_cat(dst, "\n");
		}
	}

	if(string_length(dst) == original_length)
		string_cat(dst, "> no sensors detected\n");

	return(app_action_normal);
}

irom static app_action_t set_unset_flag(const string_t *src, string_t *dst, bool_t value)
{
	if(parse_string(1, src, dst) == parse_ok)
	{
		if(!config_set_flag_by_name(dst, value))
		{
			string_copy(dst, "> unknown flag\n");
			return(app_action_error);
		}
	}

	config_flags_to_string(dst, "flags: ", "\n", config.flags);

	return(app_action_normal);
}

irom static app_action_t application_function_set(const string_t *src, string_t *dst)
{
	return(set_unset_flag(src, dst, true));
}

irom static app_action_t application_function_unset(const string_t *src, string_t *dst)
{
	return(set_unset_flag(src, dst, false));
}

irom static app_action_t application_function_rtc_set(const string_t *src, string_t *dst)
{
	int hours, minutes;

	if((parse_int(1, src, &hours, 0) == parse_ok) &&
		(parse_int(2, src, &minutes, 0) == parse_ok))
	{
		rt_hours = hours;
		rt_mins = minutes;
		rt_secs = 0;
	}

	string_format(dst, "rtc: %02u:%02u\n", rt_hours, rt_mins);

	return(app_action_normal);
}

irom static void wlan_scan_done_callback(void *arg, STATUS status)
{
	struct bss_info *bss;

	static const char *status_msg[] =
	{
		"OK",
		"FAIL",
		"PENDING",
		"BUSY",
		"CANCEL"
	};

	static const char *auth_mode_msg[] =
	{
		"OTHER",
		"WEP",
		"WPA PSK",
		"WPA2 PSK",
		"WPA PSK + WPA2 PSK"
	};

	string_clear(&buffer_4k);
	string_format(&buffer_4k, "wlan scan result: %s\n", status <= CANCEL ? status_msg[status] : "<invalid>");
	string_format(&buffer_4k, "> %-16s  %-4s  %-4s  %-18s  %-6s  %s\n", "SSID", "CHAN", "RSSI", "AUTH", "OFFSET", "BSSID");

	for(bss = arg; bss; bss = bss->next.stqe_next)
		string_format(&buffer_4k, "> %-16s  %4u  %4d  %-18s  %6d  %02x:%02x:%02x:%02x:%02x:%02x\n",
				bss->ssid,
				bss->channel,
				bss->rssi,
				bss->authmode < AUTH_MAX ? auth_mode_msg[bss->authmode] : "<invalid auth>",
				bss->freq_offset,
				bss->bssid[0], bss->bssid[1], bss->bssid[2], bss->bssid[3], bss->bssid[4], bss->bssid[5]);

	wlan_scan_state = ws_finished;
}

irom static app_action_t application_function_wlan_list(const string_t *src, string_t *dst)
{
	if(wlan_scan_state != ws_finished)
	{
		string_cat(dst, "wlan scan: no results (yet)\n");
		return(app_action_normal);
	}

	string_copy_string(dst, &buffer_4k);
	wlan_scan_state = ws_inactive;
	return(app_action_normal);
}

irom static app_action_t application_function_wlan_scan(const string_t *src, string_t *dst)
{
	if(wlan_scan_state != ws_inactive)
	{
		string_cat(dst, "wlan-scan: already scanning\n");
		return(app_action_error);
	}

	if(ota_active())
	{
		string_cat(dst, "wlan-scan: ota active\n");
		return(app_action_error);
	}

	wlan_scan_state = ws_scanning;
	wifi_station_scan(0, wlan_scan_done_callback);
	string_cat(dst, "wlan scan started, use wlan-list to retrieve the results\n");

	return(app_action_normal);
}

irom attr_pure bool wlan_scan_active(void)
{
	return(wlan_scan_state != ws_inactive);
}

irom static app_action_t application_function_ntp_dump(const string_t *src, string_t *dst)
{
	ip_addr_t addr;
	int timezone;

	timezone = sntp_get_timezone();
	addr = sntp_getserver(0);

	string_cat(dst, "> server: ");
	string_ip(dst, addr);

	string_format(dst, "\n> time zone: GMT%c%d\n> ntp time: %s",
			timezone < 0 ? '-' : '+',
			timezone < 0 ? 0 - timezone : timezone,
			sntp_get_real_time(sntp_get_current_timestamp()));

	return(app_action_normal);
}

irom static app_action_t application_function_ntp_set(const string_t *src, string_t *dst)
{
	int timezone;
	string_new(static, ip, 32);

	if((parse_string(1, src, &ip) == parse_ok) && (parse_int(2, src, &timezone, 0) == parse_ok))
	{
		config.ntp_server = ip_addr(string_to_ptr(&ip));
		config.ntp_timezone = timezone;
	}

	return(application_function_ntp_dump(src, dst));
}

irom static app_action_t application_function_gpio_status_set(const string_t *src, string_t *dst)
{
	int gpio;

	if(parse_int(1, src, &gpio, 0) == parse_ok)
	{
		if((gpio < -1) || (gpio > 16))
		{
			string_format(dst, "status trigger gpio %d invalid\n", gpio);
			return(app_action_error);
		}

		config.stat_trigger_gpio = gpio;
	}

	string_format(dst, "status trigger at gpio %d (-1 is disabled)\n", config.stat_trigger_gpio);

	return(app_action_normal);
}

irom static app_action_t application_function_gpio_wlan_set(const string_t *src, string_t *dst)
{
	int gpio;

	if(parse_int(1, src, &gpio, 0) == parse_ok)
	{
		if((gpio < -1) || (gpio > 16))
		{
			string_format(dst, "wlan status gpio %d invalid\n", gpio);
			return(app_action_error);
		}

		config.wlan_trigger_gpio = gpio;
	}

	string_format(dst, "wlan status at gpio %d (-1 is disabled)\n", config.wlan_trigger_gpio);

	return(app_action_normal);
}

static const application_function_table_t application_function_table[] =
{
	{
		"ar", "analog-read",
		application_function_analog_read,
		"read analog input"
	},
	{
		"btp", "bridge-tcp-port",
		application_function_bridge_tcp_port,
		"set uart tcp bridge tcp port (default 25)"
	},
	{
		"cd", "config-dump",
		application_function_config_dump,
		"dump config contents"
	},
	{
		"cw", "config-write",
		application_function_config_write,
		"write config to non-volatile storage"
	},
	{
		"db", "display-brightness",
		application_function_display_brightness,
		"set or show display brightness"
	},
	{
		"dd", "display-dump",
		application_function_display_dump,
		"shows all displays"
	},
	{
		"ddm", "display-default-message",
		application_function_display_default_message,
		"set default message",
	},
	{
		"ds", "display-set",
		application_function_display_set,
		"put content on display <display id> <slot> <timeout> <text>"
	},
	{
		"gd", "gpio-dump",
		application_function_gpio_dump,
		"dump all gpio config"
	},
	{
		"gg", "gpio-get",
		application_function_gpio_get,
		"get gpio"
	},
	{
		"gm", "gpio-mode",
		application_function_gpio_mode,
		"get/set gpio mode (gpio, mode, parameters)",
	},
	{
		"gs", "gpio-set",
		application_function_gpio_set,
		"set gpio"
	},
	{
		"gss", "gpio-status-set",
		application_function_gpio_status_set,
		"set gpio to trigger on status update"
	},
	{
		"gws", "gpio-wlan-set",
		application_function_gpio_wlan_set,
		"set gpio to trigger on wlan activity"
	},
	{
		"ia", "i2c-address",
		application_function_i2c_address,
		"set i2c slave address",
	},
	{
		"id", "i2c-delay",
		application_function_i2c_delay,
		"set i2c bit transaction delay (microseconds, default 5 ~ standard 100 kHz bus)",
	},
	{
		"ir", "i2c-read",
		application_function_i2c_read,
		"read data from i2c slave",
	},
	{
		"irst", "i2c-reset",
		application_function_i2c_reset,
		"i2c interface reset",
	},
	{
		"iw", "i2c-write",
		application_function_i2c_write,
		"write data to i2c slave",
	},
	{
		"isr", "i2c-sensor-read",
		application_function_i2c_sensor_read,
		"read from i2c sensor",
	},
	{
		"isc", "i2c-sensor-calibrate",
		application_function_i2c_sensor_calibrate,
		"calibrate i2c sensor, use sensor factor offset",
	},
	{
		"isd", "i2c-sensor-dump",
		application_function_i2c_sensor_dump,
		"dump all i2c sensors",
	},
	{
		"nd", "ntp-dump",
		application_function_ntp_dump,
		"dump ntp information",
	},
	{
		"ns", "ntp-set",
		application_function_ntp_set,
		"set ntp <ip addr> <timezone GMT+x>",
	},
	{
		"?", "help",
		application_function_help,
		"help [command]",
	},
#if IMAGE_OTA == 1
	{
		"ow", "ota-write",
		application_function_ota_write,
		"ota-write file_length",
	},
	{
		"ov", "ota-verify",
		application_function_ota_verify,
		"ota-verify file_length",
	},
	{
		"os", "ota-send",
		application_function_ota_send,
		"ota-send chunk_length data",
	},
	{
		"of", "ota-finish",
		application_function_ota_finish,
		"ota-finish md5sum",
	},
	{
		"oc", "ota-commit",
		application_function_ota_commit,
		"ota-commit",
	},
#endif
	{
		"q", "quit",
		application_function_quit,
		"quit",
	},
	{
		"r", "reset",
		application_function_reset,
		"reset",
	},
	{
		"rs", "rtc-set",
		application_function_rtc_set,
		"set rtc [h m]",
	},
	{
		"s", "set",
		application_function_set,
		"set an option",
	},
	{
		"u", "unset",
		application_function_unset,
		"unset an option",
	},
	{
		"S", "stats",
		application_function_stats,
		"statistics",
	},
	{
		"ub", "uart-baud",
		application_function_uart_baud_rate,
		"set uart baud rate [1-1000000]",
	},
	{
		"ud", "uart-data",
		application_function_uart_data_bits,
		"set uart data bits [5/6/7/8]",
	},
	{
		"us", "uart-stop",
		application_function_uart_stop_bits,
		"set uart stop bits [1/2]",
	},
	{
		"up", "uart-parity",
		application_function_uart_parity,
		"set uart parity [none/even/odd]",
	},
	{
		"wl", "wlan-list",
		application_function_wlan_list,
		"retrieve results from wlan-scan"
	},
	{
		"ws", "wlan-scan",
		application_function_wlan_scan,
		"scan wlan, use wlan-list to retrieve the results"
	},
	{
		"", "",
		(void *)0,
		"",
	},
};
