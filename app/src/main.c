#include "zephyr/sys/printk.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <string.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

const struct gpio_dt_spec modem_pwr = GPIO_DT_SPEC_GET(DT_ALIAS(modem_pwr), gpios);
const struct device *modem_uart = DEVICE_DT_GET(DT_ALIAS(modem_uart));

#define RX_BUFFER_SIZE 256
static char rx_buf[RX_BUFFER_SIZE];
static int rx_pos = 0;

#define MODEM_RESPONSE_WAIT_MS  500

const char *at_command_list[] = {
		// Disable echo
    "ATE0",
    // Enable error messages
		"AT+CMEE=2",
    // Set preferred mode to AUTOMATIC
    "AT+CNMP=2", 
    // Enable CAT-M and NB-IoT scanning
    "AT+CMNB=3",
    // Set APN
    "AT+CGDCONT=1,\"IP\",\"iot\"", 
    // Query SW version
    "AT+CGMR",
    "ATI",
};

const char *connect_command_list[] = {
    // Check signal
    "AT+CSQ",
    // Check connection
    "AT+CPSI?",
    "AT+CREG?",  // 2G Registration
    "AT+CGREG?", // LTE Registration
    "AT+COPS?",  // ISP
    "AT+CPSI?",   // System info

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
	clear_buf();
	for(;;)
	{
		send_at_cmd("AT");
		k_msleep(200);
		printk("\n\nbuffer len: %d\n", strlen(rx_buf));
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
		printk("\n\nbuffer len: %d\n", strlen(rx_buf));
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
		printk("\n\nbuffer len: %d\n", strlen(rx_buf));
		char* pos = strstr(rx_buf, search);
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


int main(void)
{
	printk("--- SIM7070G TEST ---\n");

	if (!device_is_ready(modem_uart)) return 0;

	uart_irq_callback_user_data_set(modem_uart, uart_cb, NULL);
	uart_irq_rx_enable(modem_uart);

	poll_at();
	k_msleep(100);

	// Disable radio
	send_at_cmd("AT+CFUN=0"); 
	k_msleep(1000);

	// Configure
	int cmd_count = sizeof(at_command_list) / sizeof(at_command_list[0]);
	for (int i = 0; i < cmd_count; i++) {
		send_at_cmd(at_command_list[i]);
		k_msleep(MODEM_RESPONSE_WAIT_MS);
	}

	// Enable radio
	send_at_cmd("AT+CFUN=1"); 
	poll_ready();
	k_msleep(200);
	poll_signal();

	// Connect
	cmd_count = sizeof(connect_command_list) / sizeof(connect_command_list[0]);
	for (int i = 0; i < cmd_count; i++) {
		send_at_cmd(connect_command_list[i]);
		k_msleep(1000);
	}

	// Send request
	send_at_cmd("AT+SHCONN");
	poll_ok();
	send_at_cmd("AT+SHREQ=\"/\",1");
	poll_request();
	send_at_cmd("AT+SHREAD=0,7");
	k_msleep(100);
	send_at_cmd("AT+SHDISC");

	return 0;
}
