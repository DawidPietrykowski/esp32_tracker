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
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/i2c.h>
#include <string.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <zephyr/pm/policy.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static const struct gpio_dt_spec wakeup_pin = GPIO_DT_SPEC_GET(DT_ALIAS(wakeup_pin), gpios);
#define WAKEUP_TIME_SEC     60

const struct gpio_dt_spec modem_pwr = GPIO_DT_SPEC_GET(DT_ALIAS(modem_pwr), gpios);
const struct device *modem_uart = DEVICE_DT_GET(DT_ALIAS(modem_uart));

#define BMI160_REG_INT_MAP_0  0x55
#define BMI160_REG_INT_OUT_CTRL  0x53

static const struct i2c_dt_spec dev_i2c = I2C_DT_SPEC_GET(DT_NODELABEL(my_bmi160));

#define RX_BUFFER_SIZE 256
static char rx_buf[RX_BUFFER_SIZE];
static int rx_pos = 0;

#define MODEM_RESPONSE_WAIT_MS  500
#define GPS_POLLING_WAIT_MS  5000
#define GPS_RESPONSE_WAIT_MS  500
#define GPS_POLLING_MAX_NODATE_MS  (1 * 1000)
#define GPS_POLLING_MAX_DATE_MS  (8 * 60 * 1000)

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


const char *nbiot_command_list[] = {
    // --- NB-IoT setup
    // Set preferred mode to AUTOMATIC
    "AT+CNMP=2",
    // Enable CAT-M and NB-IoT scanning
    "AT+CMNB=3",
    // Set APN
    "AT+CGDCONT=1,\"IP\",\"iot\"", 
};

const char *connect_command_list[] = {
    // Check connection
    // "AT+CPSI?",
    // "AT+CREG?",  // 2G Registration
    // "AT+CGREG?", // LTE Registration
    // "AT+COPS?",  // ISP
    // "AT+CPSI?",  // System info

    // Get IPv4 IP
		"AT+CNACT=0,1",
};

const char *http_command_list[] = {
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
			// printk("RECEIVED: %d\n", c);

			if (rx_pos < sizeof(rx_buf) - 1) {
				rx_buf[rx_pos++] = c;
				rx_buf[rx_pos] = '\0';
			}
		}
	}
}

void clear_buf()
{
	unsigned int key = irq_lock();
	rx_buf[0] = '\0';
	rx_pos=0;
  irq_unlock(key);
}

void send_at_cmd(const char *cmd)
{
	printk("\n\n>>> SENDING: %s\n", cmd);
	
	clear_buf();
	uart_poll_out(modem_uart, '\r');
	uart_poll_out(modem_uart, '\n');
	for (int i = 0; i < strlen(cmd); i++) {
		uart_poll_out(modem_uart, cmd[i]);
	}
	uart_poll_out(modem_uart, '\r');
	uart_poll_out(modem_uart, '\n');
}

int poll_at(uint32_t max_ms)
{
	char* search = "OK";
	uint32_t wait_time = 0;
	for(;;)
	{
		send_at_cmd("AT");
		k_msleep(200);
		if(strstr(rx_buf, search) != NULL)
			return 0;
		wait_time += 200;
		if(wait_time > max_ms) {
			return -1;
		}
	}
}

void poll_ready()
{
	char* search = "SMS Ready";
	for(;;)
	{
		k_msleep(100);
		if(strstr(rx_buf, search) != NULL)
			break;
	}
}

int poll_signal()
{
	char* search = "CSQ:";
	for(;;)
	{
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
			int signal = atoi(pos + 5);
			printk("\n\nfound signal %d\n", signal);
			return signal;
		}
	}
}

int poll_ip()
{
	char* search = "CNACT: 0,1,";
	for(;;)
	{
		send_at_cmd("AT+CNACT?");
		k_msleep(500);
		char* pos = strstr(rx_buf, search);
		char* next_ip_pos = strstr(rx_buf, "CNACT: 1,0");
		*(next_ip_pos - 1) = '\0';
		if (pos == NULL || next_ip_pos == NULL) {
			continue;
		}
		printk("\nreceived IP %s\n", pos + strlen(search));
		if(strstr(pos + strlen(search), "0.0.0.0") == NULL)
		{
			printk("\n\nreceived valid IP\n");
			return 0;
		}
	}
}

int poll_request(uint32_t max_ms)
{
	char* search = "SHREQ:";
	uint32_t wait_time = 0;
	for(;;)
	{
		k_msleep(100);
		printk("waiting for request finish\n");
		if(strstr(rx_buf, search) != NULL)
		{
			printk("\n\nfinished request\n");
			return 0;
		}
		wait_time += 100;
		if(wait_time > max_ms) {
			return -1;
		}
	}
}

int poll_ok(uint32_t max_ms)
{
	char* search = "OK";
	uint32_t wait_time = 0;
	for(;;)
	{
		k_msleep(100);
		if(strstr(rx_buf, search) != NULL)
			return 0;
		wait_time += 100;
		if(wait_time > max_ms) {
			return -1;
		}
	}
}

void poll_shbod()
{
	char* search = ">";
	for(;;)
	{
		printk("\n\nwaiting for shbod ok, buffer: \n");
		for (int i = 0; i <= rx_pos; i++) {
			printk("buf[%d] = %d\n", i, rx_buf[i]);
		}
		if(strstr(rx_buf, search) != NULL)
			break;
		k_msleep(500);
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
	clear_buf();
	k_msleep(100);
	sprintf(shbod, "AT+SHBOD=%d,%d\r", (int)sizeof(frame), 1000);
	printk(">>> SENDING: %s\n", shbod);

	uart_poll_out(modem_uart, '\r');
	uart_poll_out(modem_uart, '\n');
	for (int i = 0; i < strlen(shbod); i++) {
		uart_poll_out(modem_uart, shbod[i]);
	}
	// TODO: fix rx_buf not receiving '>' character
	// poll_shbod();
	k_msleep(100);
	clear_buf();
	for (int i = 0; i < sizeof(frame_to_send); i++) {
		uart_poll_out(modem_uart, ((char*)&frame_to_send)[i]);
	}
	uart_poll_out(modem_uart, '\r');
	uart_poll_out(modem_uart, '\n');
	if (poll_ok(3000) != 0) {
		return -1;
	}

	send_at_cmd("AT+SHREQ=\"/pos\",3");
	if (poll_request(3000) != 0) {
		return -1;
	}

	return 0;
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

int get_gps_data(frame *data)
{
	char* search = "+CGNSINF: ";
	uint32_t polling_time = 0;
	bool date_received = false;
  data->timestamp = 0;

	for(;;) {
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
											date_received = true;
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
      	return 0;
      }
		}
		if ((!date_received && (polling_time >= GPS_POLLING_MAX_NODATE_MS))
		    || (date_received && (polling_time >= GPS_POLLING_MAX_DATE_MS))) {
		  printk("Failed to get GPS: timeout\n");
		  data->latitude = NAN;
		  data->longitude = NAN;
		  return -1;
		}
		k_msleep(GPS_POLLING_WAIT_MS);
    polling_time += GPS_POLLING_WAIT_MS + GPS_RESPONSE_WAIT_MS;
	}
}

int configure_bmi160_interrupts()
{
  printk("Initializing BMI160\n");
  const struct device *const bmi160 = DEVICE_DT_GET_ANY(bosch_bmi160);

  if (!device_is_ready(bmi160)) {
      printk("BMI160 is not ready\n");
      return -1;
  }

  if (!i2c_is_ready_dt(&dev_i2c)) {
      printk("I2C bus not ready\n");
      return -1;
  }

  // Enable single-tap interrupt
  if (i2c_reg_write_byte_dt(&dev_i2c, BMI160_REG_INT_MAP_0, 0x20) != 0) {
      printk("Failed to write INT_MAP_0\n");
      return -1;
  }
  // Configure INT1 output
  if (i2c_reg_write_byte_dt(&dev_i2c, 0x53, 0xA) != 0) {
      printk("Failed to write INT_OUT_CTRL\n");
      return -1;
  }
  // Enable interrupt
  if (i2c_reg_write_byte_dt(&dev_i2c, 0x50, 0x30) != 0) {
      printk("Failed to write INT_OUT_CTRL\n");
      return -1;
  }

  printk("Configured BMI160\n");
  return 0;
}

int power_on_sim7070g()
{
    int ret;

    if (!gpio_is_ready_dt(&modem_pwr)) {
        printk("Error: Modem GPIO not ready\n");
        return -1;
    }

    ret = gpio_pin_configure_dt(&modem_pwr, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        printk("Error: Failed to set modem GPIO\n");
        return ret;
    }

    gpio_pin_set_dt(&modem_pwr, 1);
    
    k_sleep(K_SECONDS(1)); 
    
    gpio_pin_set_dt(&modem_pwr, 0);

    k_sleep(K_SECONDS(1));

    return 0;
}

int collect_send_gps_data()
{
	printk("Initializing SIM7070G UART\n");
	// Initialize UART
	if (!device_is_ready(modem_uart)) return -1;

	k_sleep(K_MSEC(100));

	uart_irq_callback_user_data_set(modem_uart, uart_cb, NULL);
	uart_irq_rx_enable(modem_uart);
  printk("Initialized SIM7070G UART\n");

	// Trigger PWR
	if (power_on_sim7070g()) return -1;
	if (poll_at(10000) != 0) {
	  printk("Error: AT AT poll timeoutpoll timeout\n");
		send_at_cmd("AT+CPOWD=1");
		return -1;
	}
  printk("Triggered PWR on SIM7070G\n");

	frame frame_to_send;
	frame_to_send.battery = 0.0; // TODO: ADC for battery level
	frame_to_send.latitude = 0.0;
	frame_to_send.longitude = 0.0;

	// Configure
	int cmd_count = sizeof(at_command_list) / sizeof(at_command_list[0]);
	for (int i = 0; i < cmd_count; i++) {
		send_at_cmd(at_command_list[i]);
		k_msleep(MODEM_RESPONSE_WAIT_MS);
	}

	if (get_gps_data(&frame_to_send) != 0) {
	  printk("Failed to get GPS data: sending NAN\n");
	}
	send_at_cmd("AT+CGNSPWR=0"); 
	poll_ok(5000);

	// Reset radio
	send_at_cmd("AT+CFUN=0"); 
	poll_ok(5000);
	// Connect
	cmd_count = sizeof(nbiot_command_list) / sizeof(nbiot_command_list[0]);
	for (int i = 0; i < cmd_count; i++) {
		send_at_cmd(nbiot_command_list[i]);
		k_msleep(500);
	}
	send_at_cmd("AT+CFUN=1"); 
	poll_ready();

	// Check signal
	int signal = poll_signal();
	frame_to_send.signal = (double)signal / 99.0;

	// Connect
	cmd_count = sizeof(connect_command_list) / sizeof(connect_command_list[0]);
	for (int i = 0; i < cmd_count; i++) {
		send_at_cmd(connect_command_list[i]);
		k_msleep(1000);
	}

	// Wait for IP
	poll_ip();
	k_msleep(1000);

	// Configure HTTP
	cmd_count = sizeof(http_command_list) / sizeof(http_command_list[0]);
	for (int i = 0; i < cmd_count; i++) {
		send_at_cmd(http_command_list[i]);
		k_msleep(300);
	}

	// Send request
	send_at_cmd("AT+SHCONN");
	poll_ok(5000);
	if (send_frame(frame_to_send) != 0) {
	  printk("Error: Request failed\n");
		// Disable module
		send_at_cmd("AT+CPOWD=1");
		return -1;
	}
	send_at_cmd("AT+SHDISC");
	poll_ok(3000);

	// Disable module
	send_at_cmd("AT+CPOWD=1");

	return 0;
}

int configure_wakeup()
{
	esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
	switch (cause) {
		case ESP_SLEEP_WAKEUP_TIMER:
			printk("Wakeup cause: TIMER\n");
			break;
		case ESP_SLEEP_WAKEUP_EXT1:
			printk("Wakeup cause: BMI160 movement\n");
			break;
		case ESP_SLEEP_WAKEUP_UNDEFINED:
		default:
			printk("Wakeup cause: COLD BOOT\n");
			break;
	}

	return 0;
}

int enable_timer_bmi160_wakeup()
{
	printk("Enabling timer wakeup\n");
	uint64_t sleep_time_us = WAKEUP_TIME_SEC * 1000000ULL;
	esp_sleep_enable_timer_wakeup(sleep_time_us);
	printk("Timer set for %d seconds.\n", WAKEUP_TIME_SEC);

	// Enable wakeup on bmi160 pin
	printk("Enabling EXT1 wakeup\n");
	if (!gpio_is_ready_dt(&wakeup_pin)) {
		printk("Error: button device %s is not ready\n", wakeup_pin.port->name);
		return -1;
	}
	int ret = gpio_pin_configure_dt(&wakeup_pin, GPIO_INPUT);
	if (ret != 0) {
		printk("Error %d: failed to configure %s pin %d\n", ret, wakeup_pin.port->name, wakeup_pin.pin);
		return -1;
	}

	// Enable EXT1 wakeup from deep sleep
	gpio_pin_interrupt_configure_dt(&wakeup_pin, GPIO_INT_DISABLE);
	esp_sleep_enable_ext1_wakeup(BIT(wakeup_pin.pin), ESP_EXT1_WAKEUP_ANY_HIGH);
	gpio_pin_interrupt_configure_dt(&wakeup_pin, GPIO_INT_ENABLE);

	return 0;
}

int main(void)
{
	pm_policy_state_lock_get(PM_STATE_STANDBY, PM_ALL_SUBSTATES);
	pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_RAM, PM_ALL_SUBSTATES);

	configure_wakeup();

	printk("Sending GPS data\n");
	collect_send_gps_data();

	// Configure BMI160 to send interrupts on movement
  if(configure_bmi160_interrupts() != 0) return 0;
	// Enable timer wakeup
  if(enable_timer_bmi160_wakeup() != 0) return 0;
	// Enter Deep Sleep
	printk("Entering Deep Sleep\n");
	k_sleep(K_MSEC(100)); 
	esp_deep_sleep_start();

	return 0;
}
