#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/timeutil.h>
#include <zephyr/sys/printk.h>
#include <string.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

const struct gpio_dt_spec modem_pwr = GPIO_DT_SPEC_GET(DT_ALIAS(modem_pwr), gpios);
const struct device *modem_uart = DEVICE_DT_GET(DT_ALIAS(modem_uart));

#define RX_BUFFER_SIZE 256
static char rx_buf[RX_BUFFER_SIZE];
static int rx_pos = 0;

#define MODEM_RESPONSE_WAIT_MS  500
#define GPS_POLLING_WAIT_MS  5000
#define GPS_RESPONSE_WAIT_MS  500
#define GPS_POLLING_MAX_MS  15000

const char *at_command_list[] = {
		// Disable echo
    "ATE0",
    // Enable error messages
		"AT+CMEE=2",
    // Query SW version
    "AT+CGMR",
    "ATI",
    // Enable GPS
    "AT+CGNSPWR=1",
};

const char *connect_command_list[] = {
    // Check connection
    // "AT+CPSI?",
    // "AT+CREG?",  // 2G Registration
    // "AT+CGREG?", // LTE Registration
    // "AT+COPS?",  // ISP
    // "AT+CPSI?",  // System info

    // --- NB-IoT setup
    // Set preferred mode to AUTOMATIC
    "AT+CNMP=2", 
    // Enable CAT-M and NB-IoT scanning
    "AT+CMNB=3",
    // Set APN
    "AT+CGDCONT=1,\"IP\",\"iot\"", 
    // Get IPv4 IP
		"AT+CNACT=0,1",
    // Wait for IP
		"AT+CNACT?",
		"AT+CNACT?",
		"AT+CNACT?",
		"AT+CNACT?",

		// --- HTTP setup
		// Disconnect
		"AT+SHDISC",

		// Configure SSL
		"AT+SHSSL?",
		"AT+CSSLCFG=?",
		"AT+CSSLCFG=\"sslversion\",1,3",
		"AT+CSSLCFG=\"sni\",1,\"gps.dawidpietrykowski.com\"",
		"AT+CSSLCFG=\"ignorertctime\",1,1",
		"AT+SHSSL=1,\"\"",

		// Configure URL
		"AT+SHCONF=\"URL\",https://gps.dawidpietrykowski.com",
		"AT+SHCONF=\"HEADERLEN\",349",
		"AT+SHCONF=\"BODYLEN\",500",
};

void uart_cb(const struct device *dev, void *user_data)
{
	uint8_t c;

	if (!uart_irq_update(dev)) {
		return;
	}

	if (uart_irq_rx_ready(dev)) {
		while (uart_fifo_read(dev, &c, 1) == 1) {
			printk("%c", c);

			if (rx_pos < sizeof(rx_buf) - 1) {
				rx_buf[rx_pos++] = c;
				rx_buf[rx_pos] = '\0';
			}
		}
	}
}

void send_at_cmd(const char *cmd)
{
	printk("\n\n>>> SENDING: %s", cmd);
	
	uart_poll_out(modem_uart, '\r');
	uart_poll_out(modem_uart, '\n');
	for (int i = 0; i < strlen(cmd); i++) {
		uart_poll_out(modem_uart, cmd[i]);
	}
	uart_poll_out(modem_uart, '\r');
	uart_poll_out(modem_uart, '\n');
}

void clear_buf()
{
	rx_buf[0] = '\0';
	rx_pos=0;
}

void poll_at()
{
	char* search = "OK";
	for(;;)
	{
		clear_buf();
		send_at_cmd("AT");
		k_msleep(200);
		if(strstr(rx_buf, search) != NULL)
			break;
	}
}

void poll_ready()
{
	clear_buf();
	char* search = "SMS Ready";
	for(;;)
	{
		k_msleep(100);
		if(strstr(rx_buf, search) != NULL)
			break;
	}
}

void poll_signal()
{
	char* search = "CSQ:";
	for(;;)
	{
		clear_buf();
		send_at_cmd("AT+CSQ");
		k_msleep(500);
		char* pos = strstr(rx_buf, search);
		if (pos == NULL) {
			continue;
		}
		// CSQ: xx,xx
		// 0123456789
		*(pos+7) = '\0';
		printk("\n\nreceived signal %s\n", pos);
		if(strstr(pos, "99") == NULL)
		{
			printk("\n\nfound signal %s\n", pos);
			break;
		}
	}
}

void poll_request()
{
	clear_buf();
	char* search = "SHREQ:";
	for(;;)
	{
		k_msleep(100);
		printk("waiting for request finish\n");
		if(strstr(rx_buf, search) != NULL)
		{
			printk("\n\nfinished request\n");
			break;
		}
	}
}

void poll_ok()
{
	char* search = "OK";
	clear_buf();
	for(;;)
	{
		k_msleep(100);
		printk("\n\nwaiting for ok, buffer len: %d\n", strlen(rx_buf));
		if(strstr(rx_buf, search) != NULL)
			break;
	}
}

void poll_shbod()
{
	char* search = ">";
	clear_buf();
	for(;;)
	{
		k_msleep(100);
		printk("\n\nwaiting for ok, buffer len: %d\n", strlen(rx_buf));
		if(strstr(rx_buf, search) != NULL)
			break;
	}
}

typedef struct {
  double latitude;
  double longitude;
  double signal;
  double battery;
  uint64_t timestamp;
} frame;

int send_frame(frame frame_to_send) {
	char shbod[100];
	sprintf(shbod, "AT+SHBOD=%d,%d\r", (int)sizeof(frame), 1000);
	uart_poll_out(modem_uart, '\r');
	uart_poll_out(modem_uart, '\n');
	for (int i = 0; i < strlen(shbod); i++) {
		uart_poll_out(modem_uart, shbod[i]);
	}
	poll_shbod();
	for (int i = 0; i < sizeof(frame_to_send); i++) {
		uart_poll_out(modem_uart, ((char*)&frame_to_send)[i]);
	}
	uart_poll_out(modem_uart, '\r');
	uart_poll_out(modem_uart, '\n');

	poll_ok();
	send_at_cmd("AT+SHREQ=\"/pos\",3");
	poll_request();

	return 1;
}

static char *at_token(char **str) {
    char *token_start;
    
    if (str == NULL || *str == NULL || **str == '\0') {
        return NULL;
    }

    token_start = *str;
    
    // first occurrence of the delimiter
    char *token_end = strpbrk(token_start, ",");

    if (token_end) {
        *token_end = '\0';
        *str = token_end + 1;
    } else {
        // no delimiters, point to end
        *str = token_start + strlen(token_start);
    }

    return token_start;
}

int64_t gps_date_to_epoch_ms(const char *ts_str)
{
    struct tm tm_data = { 0 };
    char temp_buf[5];
    int ms = 0;

    if (strlen(ts_str) < 18) {
        printk("Error: Timestamp string too short\n");
        return -1;
    }

    // year
    memcpy(temp_buf, &ts_str[0], 4);
    temp_buf[4] = '\0';
    tm_data.tm_year = atoi(temp_buf) - 1900; 

    // month
    memcpy(temp_buf, &ts_str[4], 2);
    temp_buf[2] = '\0';
    tm_data.tm_mon = atoi(temp_buf) - 1; 

    // day
    memcpy(temp_buf, &ts_str[6], 2);
    temp_buf[2] = '\0';
    tm_data.tm_mday = atoi(temp_buf);

    // hour
    memcpy(temp_buf, &ts_str[8], 2);
    temp_buf[2] = '\0';
    tm_data.tm_hour = atoi(temp_buf);

    // minute
    memcpy(temp_buf, &ts_str[10], 2);
    temp_buf[2] = '\0';
    tm_data.tm_min = atoi(temp_buf);

    // second
    memcpy(temp_buf, &ts_str[12], 2);
    temp_buf[2] = '\0';
    tm_data.tm_sec = atoi(temp_buf);

    // millisecond
    memcpy(temp_buf, &ts_str[15], 3);
    temp_buf[3] = '\0';
    ms = atoi(temp_buf);

    time_t seconds_epoch = timeutil_timegm(&tm_data);

    if (seconds_epoch == -1) {
        printk("Error: Failed to convert time to epoch seconds\n");
        return -1;
    }

    int64_t total_ms = ((int64_t)seconds_epoch * 1000) + ms;

    return total_ms;
}

void get_gps_data(frame *data)
{
	char* search = "+CGNSINF: ";
	uint32_t polling_time = 0;

	for(;;) {
		clear_buf();
		send_at_cmd("AT+CGNSINF");
		k_msleep(GPS_RESPONSE_WAIT_MS);
		char* pos = strstr(rx_buf, search);
		if(pos != NULL)
		{
			pos += strlen(search);

      int fix_status = 0;
		  char *token;
		  int index = 0;
	    char utc_datetime[32]; // yyyyMMddhhmmss.sss
		  int run_status;

	    while ((token = at_token(&pos)) != NULL) {
	        switch (index) {
	            case 0: // GNSS Run Status
	                run_status = atoi(token);
	                printk("run: %d\n", run_status);
	                break;
	            case 1: // Fix Status
	                if (*token != '\0') fix_status = atoi(token);
	                else fix_status = 0;
	                printk("fix: %d\n", fix_status);
	                break;
	            case 2: // UTC Date & Time
	                if (*token != '\0') {
	                    strncpy(utc_datetime, token, sizeof(utc_datetime) - 1);
											data->timestamp = (uint64_t)gps_date_to_epoch_ms(utc_datetime);
			                printk("date: %s, ms: %lld\n", utc_datetime, data->timestamp);
	                }
	                break;
	            case 3: // Latitude
	                if (*token != '\0') {
	                	data->latitude = strtod(token, NULL);
		                printk("latitude: %f\n", data->latitude);
	                }
	                break;
	            case 4: // Longitude
	                if (*token != '\0') {
	                	data->longitude = strtod(token, NULL);
		                printk("longitude: %f\n", data->longitude);
	                }
	                break;
	            case 5: // Altitude
	                if (*token != '\0') {
	                	double altitude = strtod(token, NULL);
		                printk("altitude: %f\n", altitude);
	                }
	                break;
	            default:
	                break;
	        }
	        index++;
	    }
      if (fix_status) {
			  printk("Found GPS position\n");
      	return;
      }
		}
		if (polling_time >= GPS_POLLING_MAX_MS) {
		  printk("Failed to get GPS\n");
		  data->latitude = NAN;
		  data->longitude = NAN;
		  data->timestamp = NAN;
		  return;
		}
		k_msleep(GPS_POLLING_WAIT_MS);
    polling_time += GPS_POLLING_WAIT_MS + GPS_RESPONSE_WAIT_MS;
	}
}

int main(void)
{
	printk("--- SIM7070G TEST ---\n");

	if (!device_is_ready(modem_uart)) return 0;

	uart_irq_callback_user_data_set(modem_uart, uart_cb, NULL);
	uart_irq_rx_enable(modem_uart);

	poll_at();
	k_msleep(100);

	frame frame_to_send;
	frame_to_send.battery = 0.5;
	frame_to_send.signal = 0.5;
	frame_to_send.latitude = 0.0;
	frame_to_send.longitude = 0.0;

	// Configure
	int cmd_count = sizeof(at_command_list) / sizeof(at_command_list[0]);
	for (int i = 0; i < cmd_count; i++) {
		send_at_cmd(at_command_list[i]);
		k_msleep(MODEM_RESPONSE_WAIT_MS);
	}

	get_gps_data(&frame_to_send);
	clear_buf();
	send_at_cmd("AT+CGNSPWR=0"); 
	poll_ok();

	// Disable radio
	send_at_cmd("AT+CFUN=0"); 
	poll_ok();

	// Enable radio
	send_at_cmd("AT+CFUN=1"); 
	poll_ready();
	poll_signal();

	// Connect
	cmd_count = sizeof(connect_command_list) / sizeof(connect_command_list[0]);
	for (int i = 0; i < cmd_count; i++) {
		send_at_cmd(connect_command_list[i]);
		k_msleep(500);
	}

	// Send request
	send_at_cmd("AT+SHCONN");
	poll_ok();
	send_frame(frame_to_send);
	send_at_cmd("AT+SHDISC");

	return 0;
}
