#include "user_main.h"

#include "util.h"
#include "application.h"
#include "config.h"
#include "gpios.h"
#include "stats.h"
#include "i2c.h"
#include "display.h"
#include "http.h"

#include <stdlib.h>
#include <sntp.h>
#include <espconn.h>

typedef enum
{
	wlan_bootstrap_state_skip,
	wlan_bootstrap_state_start,
	wlan_bootstrap_state_done
} wlan_bootstrap_state_t;

_Static_assert(sizeof(wlan_bootstrap_state_t) == 4, "sizeof(wlan_bootstrap_state_t) != 4");

typedef enum
{
	ts_copy,
	ts_dodont,
	ts_data,
} telnet_strip_state_t;

typedef struct
{
	esp_tcp tcp_config;
	struct espconn parent_socket;
	struct espconn *child_socket;
	string_t receive_buffer;
	string_t *send_buffer;
	bool receive_ready;
	bool send_busy;
} espsrv_t;

_Static_assert(sizeof(telnet_strip_state_t) == 4, "sizeof(telnet_strip_state) != 4");

queue_t data_send_queue;
queue_t data_receive_queue;

os_event_t background_task_queue[background_task_queue_length];

static ETSTimer periodic_timer;

static wlan_bootstrap_state_t wlan_bootstrap_state;

static struct
{
	unsigned int disconnect:1;
	unsigned int reset:1;
	unsigned int init_i2c_sensors:1;
	unsigned int init_displays:1;
	unsigned int init_ntp_bogus:1;
	unsigned int http_disconnect:1;
	unsigned int new_cmd_connection:1;
} bg_action;

static espsrv_t cmd;
static espsrv_t data;
static espsrv_t http;

irom static void user_init2(void);

irom static void ntp_init(void)
{
	if(ip_addr_valid(config.ntp_server))
	{
		sntp_setserver(0, &config.ntp_server);
		sntp_set_timezone(config.ntp_timezone);
		sntp_init();
		bg_action.init_ntp_bogus = 0;
	}
	else
		bg_action.init_ntp_bogus = 1;
}

irom static void ntp_periodic(void)
{
	struct tm *tm;
	time_t ticks;

	if(bg_action.init_ntp_bogus) // server not configured
		return;

	ticks = sntp_get_current_timestamp();
	tm = sntp_localtime(&ticks);

	rt_hours = tm->tm_hour;
	rt_mins  = tm->tm_min;
}

irom static void tcp_accept(espsrv_t *espsrv, string_t *send_buffer,
		int port, int timeout, void (*connect_callback)(struct espconn *))
{
	espsrv->send_buffer = send_buffer;

	ets_memset(&espsrv->tcp_config, 0, sizeof(espsrv->tcp_config));
	ets_memset(&espsrv->parent_socket, 0, sizeof(espsrv->parent_socket));
	espsrv->child_socket = (struct espconn *)0;

	espsrv->tcp_config.local_port = port;
	espsrv->parent_socket.proto.tcp = &espsrv->tcp_config;
	espsrv->parent_socket.type = ESPCONN_TCP;
	espsrv->parent_socket.state = ESPCONN_NONE;

	espconn_regist_connectcb(&espsrv->parent_socket, (espconn_connect_callback)connect_callback);
	espconn_accept(&espsrv->parent_socket);
	espconn_regist_time(&espsrv->parent_socket, timeout, 0);
	espconn_tcp_set_max_con_allow(&espsrv->parent_socket, 1);
}

irom noinline static void config_wlan(const char *ssid, const char *passwd)
{
	struct station_config station_config;

	if(config_get_flag(config_flag_print_debug))
		dprintf("Configure wlan, set ssid=\"%s\", passwd=\"%s\"\r\n", ssid, passwd);

	if(config_get_flag(config_flag_wlan_sdk_connect))
		wifi_station_set_auto_connect(1);
	else
	{
		wifi_station_set_auto_connect(0);
		wifi_station_disconnect();
	}

	wifi_set_opmode(STATION_MODE);

	ets_memset(&station_config, 0, sizeof(station_config));
	strlcpy(station_config.ssid, ssid, sizeof(station_config.ssid));
	strlcpy(station_config.password, passwd, sizeof(station_config.password));
	station_config.bssid_set = 0;

	wifi_station_set_config(&station_config);
	wifi_station_connect();
}

irom noinline static void wlan_bootstrap(void)
{
	string_new(static, ssid, 32);
	string_new(static, passwd, 32);
	char byte;

	while(string_space(&ssid) && !queue_empty(&data_receive_queue))
	{
		if((byte = queue_pop(&data_receive_queue)) == ' ')
			break;

		string_append(&ssid, byte);
	}

	while(string_space(&passwd) && !queue_empty(&data_receive_queue))
	{
		if((byte = queue_pop(&data_receive_queue)) == '\n')
			break;

		string_append(&passwd, byte);
	}

	config_wlan(string_to_ptr(&ssid), string_to_ptr(&passwd));

	strlcpy(config.ssid, string_to_ptr(&ssid), sizeof(config.ssid));
	strlcpy(config.passwd, string_to_ptr(&passwd), sizeof(config.passwd));
	config_write(&config);

	wlan_bootstrap_state = wlan_bootstrap_state_done;
}

irom static void background_task(os_event_t *events)
{
	stat_background_task++;

	if(wlan_bootstrap_state == wlan_bootstrap_state_start)
	{
		if(queue_lf(&data_receive_queue))
		{
			wlan_bootstrap();
			wlan_bootstrap_state = wlan_bootstrap_state_done;
		}

		if(stat_timer_slow > 100) // ~10 secs
		{
			if(config_get_flag(config_flag_print_debug))
				dprintf("%s\r\n", "Returning to normal uart bridge mode\r\n");

			wlan_bootstrap_state = wlan_bootstrap_state_done;
		}
	}

	if(wlan_bootstrap_state != wlan_bootstrap_state_start)
	{
		// send data in the uart receive fifo to tcp

		if(!queue_empty(&data_receive_queue) && !data.send_busy && string_space(data.send_buffer))
		{
			// data available and can be sent now

			while(!queue_empty(&data_receive_queue) && string_space(data.send_buffer))
				string_append(data.send_buffer, queue_pop(&data_receive_queue));

			if(string_length(data.send_buffer) > 0)
				data.send_busy = espconn_send(data.child_socket, string_to_ptr(data.send_buffer), string_length(data.send_buffer)) == 0;
		}

		// if there is still data in uart receive fifo that can't be
		// sent to tcp yet, tcp_sent_callback will call us when it can
	}

	if(bg_action.disconnect)
	{
		espconn_disconnect(cmd.child_socket);
		bg_action.disconnect = 0;
	}

	if(bg_action.http_disconnect)
	{
		espconn_disconnect(http.child_socket);
		bg_action.http_disconnect = 0;
	}

	if(bg_action.init_i2c_sensors)
	{
		uint32_t now = system_get_time();
		i2c_sensor_init();
		bg_action.init_i2c_sensors = 0;
		stat_i2c_init_time_us = system_get_time() - now;
	}

	if(bg_action.init_displays)
	{
		uint32_t now = system_get_time();
		display_init(config.display_default_msg);
		bg_action.init_displays = 0;
		stat_display_init_time_us = system_get_time() - now;
	}

	string_clear(cmd.send_buffer);

	if(bg_action.new_cmd_connection && !cmd.send_busy)
	{
		bg_action.new_cmd_connection = 0;
		string_copy(cmd.send_buffer, "OK\n");
	}

	if(cmd.receive_ready)
	{
		switch(application_content(&cmd.receive_buffer, cmd.send_buffer))
		{
			case(app_action_normal):
			case(app_action_error):
			{
				/* no special action for now */
				break;
			}
			case(app_action_empty):
			{
				string_copy(cmd.send_buffer, "> empty command\n");
				break;
			}
			case(app_action_disconnect):
			{
				string_copy(cmd.send_buffer, "> disconnect\n");
				bg_action.disconnect = 1;
				break;
			}
			case(app_action_reset):
			{
				string_copy(cmd.send_buffer, "> reset\n");
				bg_action.disconnect = 1;
				bg_action.http_disconnect = 1;
				bg_action.reset = 1;
				break;
			}
		}

		cmd.receive_ready = false;
	}

	if(string_length(cmd.send_buffer) > 0)
		cmd.send_busy =
				espconn_send(cmd.child_socket, string_to_ptr(cmd.send_buffer), string_length(cmd.send_buffer)) == 0;
}

irom static void tcp_data_sent_callback(void *arg)
{
	data.send_busy = false;

	// retry to send data still in the fifo

	system_os_post(background_task_id, 0, 0);
}

irom static void tcp_data_receive_callback(void *arg, char *buffer, unsigned short length)
{
	int current, byte;
	bool_t strip_telnet;
	telnet_strip_state_t telnet_strip_state;

	strip_telnet = config_get_flag(config_flag_strip_telnet);
	telnet_strip_state = ts_copy;

	for(current = 0; (current < length) && !queue_full(&data_send_queue); current++)
	{
		byte = buffer[current];

		switch(telnet_strip_state)
		{
			case(ts_copy):
			{
				if(strip_telnet && (byte == 0xff))
					telnet_strip_state = ts_dodont;
				else
					queue_push(&data_send_queue, (char)byte);

				break;
			}

			case(ts_dodont):
			{
				telnet_strip_state = ts_data;
				break;
			}

			case(ts_data):
			{
				telnet_strip_state = ts_copy;
				break;
			}
		}
	}

	uart_start_transmit(!queue_empty(&data_send_queue));
}

irom static void tcp_data_disconnect_callback(void *arg)
{
	data.child_socket = 0;
}

irom static void tcp_data_connect_callback(struct espconn *new_connection)
{
	if(data.child_socket)
		espconn_disconnect(new_connection); // not allowed but won't occur anyway
	else
	{
		data.child_socket = new_connection;
		data.send_busy = false;

		espconn_regist_recvcb(data.child_socket, tcp_data_receive_callback);
		espconn_regist_sentcb(data.child_socket, tcp_data_sent_callback);
		espconn_regist_disconcb(data.child_socket, tcp_data_disconnect_callback);

		espconn_set_opt(data.child_socket, ESPCONN_REUSEADDR | ESPCONN_NODELAY);

		queue_flush(&data_send_queue);
		queue_flush(&data_receive_queue);
	}
}

irom static void tcp_cmd_sent_callback(void *arg)
{
	cmd.send_busy = false;
}

irom static void tcp_cmd_receive_callback(void *arg, char *buffer, unsigned short length)
{
	if(!cmd.receive_ready && (length > 1) && (buffer[length - 2] == '\r') && (buffer[length - 1] == '\n'))
	{
		string_set(&cmd.receive_buffer, buffer, length, length);
		string_setlength(&cmd.receive_buffer, length - 2);
		cmd.receive_ready = true;
	}

	system_os_post(background_task_id, 0, 0);
}

irom static void tcp_cmd_reconnect_callback(void *arg, int8_t err)
{
	cmd.send_busy = false;
}

irom static void tcp_cmd_disconnect_callback(void *arg)
{
	cmd.send_busy = false;
	cmd.receive_ready = false;
	cmd.child_socket = 0;

	if(bg_action.reset)
	{
		msleep(10);
		reset();
	}
}

irom static void tcp_cmd_connect_callback(struct espconn *new_connection)
{
	if(cmd.child_socket)
		espconn_disconnect(new_connection); // not allowed but won't occur anyway
	else
	{
		cmd.child_socket = new_connection;

		espconn_regist_recvcb(cmd.child_socket, tcp_cmd_receive_callback);
		espconn_regist_sentcb(cmd.child_socket, tcp_cmd_sent_callback);
		espconn_regist_reconcb(cmd.child_socket, tcp_cmd_reconnect_callback);
		espconn_regist_disconcb(cmd.child_socket, tcp_cmd_disconnect_callback);

		espconn_set_opt(cmd.child_socket, ESPCONN_REUSEADDR | ESPCONN_NODELAY);
		bg_action.new_cmd_connection = 1;
	}
}

irom static void tcp_http_sent_callback(void *arg)
{
	http.send_busy = false;
}

irom static void tcp_http_receive_callback(void *arg, char *buffer, unsigned short length)
{
	string_t src = string_from_ptr(length, buffer);
	http_action_t http_action;

	if(!http.send_busy)
	{
		http_action = http_process_request(&src, http.send_buffer);

		(void)http_action; //FIXME

		http.send_busy =
				espconn_send(http.child_socket, string_to_ptr(http.send_buffer), string_length(http.send_buffer)) == 0;
	}

	bg_action.http_disconnect = 1;
}

irom static void tcp_http_disconnect_callback(void *arg)
{
	http.child_socket = 0;
}

irom static void tcp_http_connect_callback(struct espconn *new_connection)
{
	if(http.child_socket)
		espconn_disconnect(new_connection); // not allowed but won't occur anyway
	else
	{
		http.child_socket = new_connection;
		http.send_busy = false;

		espconn_regist_recvcb(http.child_socket, tcp_http_receive_callback);
		espconn_regist_sentcb(http.child_socket, tcp_http_sent_callback);
		espconn_regist_disconcb(http.child_socket, tcp_http_disconnect_callback);

		espconn_set_opt(http.child_socket, ESPCONN_REUSEADDR | ESPCONN_NODELAY);
	}
}

irom noinline static void periodic_timer_slowpath(void)
{
	stat_timer_slow++;

	if(++ut_tens > 9)
	{
		ut_tens = 0;

		if(++ut_secs > 59)
		{
			ut_secs = 0;

			if(++ut_mins > 59)
			{
				ut_mins = 0;

				if(++ut_hours > 23)
				{
					ut_hours = 0;
					ut_days++;
				}
			}
		}
	}

	if(++rt_tens > 9)
	{
		rt_tens = 0;

		if(++rt_secs > 59)
		{
			rt_secs = 0;

			if(++rt_mins > 59)
			{
				rt_mins = 0;

				if(++rt_hours > 23)
				{
					rt_hours = 0;
					rt_days++;
				}
			}
		}
	}

	system_os_post(background_task_id, 0, 0);
}

iram static void periodic_timer_callback(void *arg)
{
	static int timer_slow_skipped = 0;
	static int timer_second_skipped = 0;
	static int timer_minute_skipped = 0;

	(void)arg;

	stat_timer_fast++;
	timer_slow_skipped++;
	timer_second_skipped++;
	timer_minute_skipped++;

	// timer runs on 100 Hz == 10 ms

	gpios_periodic();

	// run background task every 10 Hz = 100 ms

	if(timer_slow_skipped > 9)
	{
		timer_slow_skipped = 0;
		periodic_timer_slowpath();
	}

	// run display background task every second = 1000 ms

	if(timer_second_skipped > 99)
	{
		stat_timer_second++;
		timer_second_skipped = 0;
		display_periodic();
	}

	// check ntp every minute = 60000 ms

	if(timer_minute_skipped > 5999)
	{
		stat_timer_minute++;
		timer_minute_skipped = 0;
		ntp_periodic();
	}
}

irom void user_init(void);

irom void user_init(void)
{
	static char data_send_queue_buffer[1024];
	static char data_receive_queue_buffer[1024];

	queue_new(&data_send_queue, sizeof(data_send_queue_buffer), data_send_queue_buffer);
	queue_new(&data_receive_queue, sizeof(data_receive_queue_buffer), data_receive_queue_buffer);

	bg_action.reset = 0;
	bg_action.disconnect = 0;
	bg_action.init_i2c_sensors = 1;
	bg_action.init_displays = 1;
	bg_action.init_ntp_bogus = 0;
	bg_action.http_disconnect = 0;
	bg_action.new_cmd_connection = 0;

	config_read(&config);
	uart_init(&config.uart);
	system_set_os_print(config_get_flag(config_flag_print_debug));

	if(config.wlan_trigger_gpio >= 0)
		gpios_set_wlan_trigger(config.wlan_trigger_gpio);

	if(config_get_flag(config_flag_phy_force))
	{
		//wifi_set_phy_mode(PHY_MODE_11G);
		//wifi_set_user_fixed_rate(FIXED_RATE_MASK_STA, PHY_RATE_54);
		//wifi_set_user_sup_rate(RATE_11G6M, RATE_11G36M);
		//wifi_set_user_rate_limit(RC_LIMIT_11G, 0x00, RATE_11G_G6M, RATE_11G_G6M);
	}

	if(config_get_flag(config_flag_wlan_power_save))
		wifi_set_sleep_type(MODEM_SLEEP_T);
	else
		wifi_set_sleep_type(NONE_SLEEP_T);

	system_init_done_cb(user_init2);
}

irom static void user_init2(void)
{
	string_new(static, data_send_buffer, 1024);
	string_new(static, http_send_buffer, 2048);
	string_new(static, cmd_send_buffer, 4096);

	ntp_init();
	gpios_init();

	config_wlan(config.ssid, config.passwd);

	tcp_accept(&data,	&data_send_buffer,	config.bridge_tcp_port, 0,	tcp_data_connect_callback);
	tcp_accept(&cmd,	&cmd_send_buffer,	24,						30,	tcp_cmd_connect_callback);
	tcp_accept(&http,	&http_send_buffer,	80,						30,	tcp_http_connect_callback);

	system_os_task(background_task, background_task_id, background_task_queue, background_task_queue_length);

	if(config_get_flag(config_flag_disable_wlan_bootstrap))
		wlan_bootstrap_state = wlan_bootstrap_state_skip;
	else
	{
		if(config_get_flag(config_flag_print_debug))
		{
			dprintf("\r\n%s\r\n", "You now can enter wlan ssid and passwd within 10 seconds.");
			dprintf("%s\r\n", "Use exactly one space between them and a linefeed at the end.");
		}
		wlan_bootstrap_state = wlan_bootstrap_state_start;
	}

	if(config_get_flag(config_flag_cpu_high_speed))
		system_update_cpu_freq(160);
	else
		system_update_cpu_freq(80);

	os_timer_setfn(&periodic_timer, periodic_timer_callback, (void *)0);
	os_timer_arm(&periodic_timer, 10, 1); // fast system timer = 100 Hz = 10 ms
}
