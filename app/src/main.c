#include <zephyr/kernel.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <string.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define STRIP_NODE		DT_ALIAS(led_strip)
#define STRIP_NUM_PIXELS	DT_PROP(STRIP_NODE, chain_length)

static const struct led_rgb colors[] = {
	{ .r = 255, .g = 0,   .b = 0   },
	{ .r = 0,   .g = 255, .b = 0   },
	{ .r = 0,   .g = 0,   .b = 255 },
	{ .r = 0,   .g = 0,   .b = 0   },
};

const struct gpio_dt_spec modem_pwr = GPIO_DT_SPEC_GET(DT_ALIAS(modem_pwr), gpios);
const struct device *modem_uart = DEVICE_DT_GET(DT_ALIAS(modem_uart));

static char rx_buf[256];
static int rx_pos = 0;

#define MODEM_RESPONSE_WAIT_MS  3000

// Read 512 bytes
const char *read_cmd = "AT+SHREAD=0,512\r\n";
// Disconnect
const char *close_cmd = "AT+SHDISC\r\n";

const char *at_command_list[] = {
    "AT\r\n", 

    // Reset Modem
    "AT+CFUN=0\r\n", 

    // Set Preferred Mode to AUTOMATIC
    "AT+CNMP=2\r\n", 

    // Enable CAT-M and NB-IoT scanning
    "AT+CMNB=3\r\n",

    // APN
    "AT+CGDCONT=1,\"IP\",\"iot\"\r\n", 

    // Reboot radio
    "AT+CFUN=1\r\n", 
    
    // Check signal
    "AT+CSQ\r\n",
    "AT+CSQ\r\n",
    "AT+CSQ\r\n",
    "AT+CSQ\r\n",
    "AT+CSQ\r\n",
    "AT+CSQ\r\n",
    "AT+CSQ\r\n",
    
    // Check service
    "AT+CPSI?\r\n",
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
	
	for (int i = 0; i < strlen(cmd); i++) {
		uart_poll_out(modem_uart, cmd[i]);
	}
}

int main(void)
{
	printk("--- SIM7070G TEST ---\n");

	if (!device_is_ready(modem_uart)) return 0;

	uart_irq_callback_user_data_set(modem_uart, uart_cb, NULL);
	uart_irq_rx_enable(modem_uart);

	int cmd_count = sizeof(at_command_list) / sizeof(at_command_list[0]);

	for (int i = 0; i < cmd_count; i++) {
		send_at_cmd(at_command_list[i]);
		k_msleep(MODEM_RESPONSE_WAIT_MS);
	}

	while (1) {
		k_msleep(1000);
	}
	return 0;

	const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);

	if (!device_is_ready(strip)) {
		LOG_ERR("LED strip device not ready");
		return 0;
	}

	LOG_INF("Found LED strip device %s", strip->name);

	size_t color_idx = 0;

	while (1) {
		struct led_rgb pixel; 

		pixel.r = colors[color_idx].r * 0.2f;
		pixel.g = colors[color_idx].g * 0.2f;
		pixel.b = colors[color_idx].b * 0.2f;

		// Update the strip
		int rc = led_strip_update_rgb(strip, &pixel, 1);
		
		if (rc) {
			LOG_ERR("couldn't update strip: %d", rc);
		}

		color_idx++;
		if (color_idx >= ARRAY_SIZE(colors)) {
			color_idx = 0;
		}

		k_msleep(1000);
	}
	return 0;
}
