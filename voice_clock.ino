#include <hardware/watchdog.h>
#include <hardware/pwm.h>
#include <WiFi.h>
#include <Wire.h>
#include <DFRobot_RGBLCD1602.h>
#include <RP2040_SD.h>

//#define DEBUG
#if defined(DEBUG)
#	define DEBUG_WRITE(c) Serial.write(c)
#	define DEBUG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#	define DEBUG_WRITE(c) (void)(0)
#	define DEBUG_PRINTF(...) (void)(0)
#endif

#define WAVE_QUEUE_SIZE_NUM_SHIFT (14)
#define WAVE_QUEUE_SIZE (1U << WAVE_QUEUE_SIZE_NUM_SHIFT)
#define WAVE_QUEUE_SIZE_MASK (WAVE_QUEUE_SIZE - 1)
#define WAVE_QUEUE_SIZE_RMASK (~WAVE_QUEUE_SIZE_MASK)
#define WAVE_FILE_NAME_QUEUE_SIZE_NUM_SHIFT (4)
#define WAVE_FILE_NAME_QUEUE_SIZE (1U << WAVE_FILE_NAME_QUEUE_SIZE_NUM_SHIFT)
#define WAVE_FILE_NAME_QUEUE_SIZE_MASK (WAVE_FILE_NAME_QUEUE_SIZE - 1)
#define WAVE_FILE_NAME_QUEUE_SIZE_RMASK (~WAVE_FILE_NAME_QUEUE_SIZE_MASK)
#define MAX_NUM_SIGNAL (24)
#define MAX_NUM_CHIME (24)
#define MAX_SIZE_SSID_STRING (32)
#define MAX_SIZE_PASS_STRING (32)
#define MAX_SIZE_NET_ADDRESS_STRING (32)
#define MAX_SIZE_DISP_STRING (32)
#define MAX_SIZE_VOICE_STRING (16)
#define MAX_SIZE_MELODY_STRING (64)
#define NTP_TIMEOUT (4)
#define MIN_DIRECT_ADJ_POS_DIFF_TIME (60)
#define MIN_DIRECT_ADJ_NEG_DIFF_TIME (60)
#define IS_INIT_NTP_RECOVERED_RTC (1)
#define IS_NTP_RECOVERED_RTC (1)
#define IS_RTC_OMITTED (0)
#define INI_PATH "/"
#define WAVE_PATH "/wav/"

/* Byte Order Conversion (le to host) API
 * - uint16_t ltohs(uint16_t n)
 * - uint32_t ltohl(uint32_t n)
 */

#define ltohs(n) (n)
#define ltohl(n) (n)

/* System API
 * - void system_reset()
 * - void system_yield()
 */

void system_reset()
{
#	if defined(DEBUG)
		DEBUG_PRINTF("reboot\n");
		Serial.end();
#	endif
	(void)save_and_disable_interrupts();
	watchdog_reboot(0, 0, 0);
	for ( ; ; ) tight_loop_contents();
}
#define system_yield() watchdog_update()

/* Clock API
 * - int clk_begin()
 * - int clk_end()
 * - time_t clk_get()
 * - void clk_set(time_t t)
 * - struct tm *clk_gettm(time_t t)
 */

int clk_begin() {return 0;}
int clk_end() {return 0;}
#define clk_get() (time(NULL))
#define clk_set(t) do{struct timeval tv = {(t), 0}; settimeofday(&tv, NULL);}while (0)
struct tm *clk_gettm(time_t t) {return gmtime(&t);}

/* Real Time Clock API
 * - int rtc_begin()
 * - int rtc_end()
 * - time_t rtc_get()
 * - int rtc_set(time_t t)
 */

#if IS_RTC_OMITTED
	int rtc_begin() {return 0;}
	int rtc_end() {return 0;}
#	define rtc_get() (0)
#	define rtc_set(t) ((t), 0)
#else
#	define _RTC_SDA_GPIO (4)
#	define _RTC_SCL_GPIO (5)
#	define _RTC_ADDR (0x68)
#	define _RTC_CONTROL_1_REG (0x00)
#	define _RTC_CONTROL_2_REG (0x01)
#	define _RTC_CONTROL_3_REG (0x02)
#	define _RTC_SECONDS_REG (0x03)
#	define _RTC_MINUTES_REG (0x04)
#	define _RTC_HOURS_REG (0x05)
#	define _RTC_DAYS_REG (0x06)
#	define _RTC_WEEKDAYS_REG (0x07)
#	define _RTC_MONTHS_REG (0x08)
#	define _RTC_YEARS_REG (0x09)
	static int _rtc_reg_read(uint8_t address, uint8_t *data, uint8_t size)
	{
		int ret = 0;
		int i;
		Wire.beginTransmission(_RTC_ADDR);
		Wire.write(address);
		if (0 != Wire.endTransmission())
		{
			ret = 1;
			goto EXIT;
		}
		if (size != Wire.requestFrom(_RTC_ADDR, size))
		{
			ret = 1;
			goto EXIT;
		}
		for (i = 0 ; i < (int)size ; i++)
		{
			data[i] = Wire.read();
		}
	EXIT:
		return ret;
	}
	static int _rtc_reg_write(uint8_t address, uint8_t const *data, uint8_t size)
	{
		int ret = 0;
		int i;
		Wire.beginTransmission(_RTC_ADDR);
		Wire.write(address);
		for (i = 0 ; i < (int)size ; i++)
		{
			Wire.write(data[i]);
		}
		if (0 != Wire.endTransmission())
		{
			ret = 1;
			goto EXIT;
		}
	EXIT:
		return ret;
	}
	int rtc_begin()
	{
#		pragma pack(push, 1)
		struct {
			uint8_t i;
			uint8_t ii;
			uint8_t iii;
		} control[1] = {{
			0b00000000,
			0b00000000,
			0b00000000
		}};
#		pragma pack(pop)
		Wire.begin();
		Wire.setSDA(_RTC_SDA_GPIO);
		Wire.setSCL(_RTC_SCL_GPIO);
		return _rtc_reg_write(_RTC_CONTROL_1_REG, (uint8_t *)control, sizeof(*control));
	}
	int rtc_end() {return 0;}
	time_t rtc_get()
	{
		time_t ret;
#		pragma pack(push, 1)
		struct {
			uint8_t seconds;
			uint8_t minutes;
			uint8_t hours;
			uint8_t days;
			uint8_t weekdays;
			uint8_t months;
			uint8_t years;
		} time_date[1];
#		pragma pack(pop)
		struct tm tm[1] = {{0}};
#		define bcd_to_int(b) (((b) >> 4) * 10 + ((b) & 0xf))
		if (0 != _rtc_reg_read(_RTC_SECONDS_REG, (uint8_t *)time_date, sizeof(*time_date)))
		{
			DEBUG_PRINTF("rtc: get (error)\n");
			ret = -1;
			goto EXIT;
		}
		if (0 != (time_date->seconds & 0x80))
		{
			DEBUG_PRINTF("rtc: get (error)\n");
			ret = -1;
			goto EXIT;
		}
		tm->tm_year = bcd_to_int(time_date->years) + 100; // -1900
		tm->tm_mon = bcd_to_int(time_date->months) - 1;   // 0 - 11 (Jan - Dec)
		tm->tm_mday = bcd_to_int(time_date->days);        // 1 - 31
		tm->tm_wday = time_date->weekdays;                // 0 - 6  (Sun - Sat)
		tm->tm_hour = bcd_to_int(time_date->hours);       // 0 - 23
		tm->tm_min = bcd_to_int(time_date->minutes);      // 0 - 59
		tm->tm_sec = bcd_to_int(time_date->seconds);      // 0 - 59
		DEBUG_PRINTF(
			"rtc: get (year=%d, mon=%d, mday=%d, wday=%d, hour=%d, min=%d, sec=%d)\n",
			tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_wday, tm->tm_hour, tm->tm_min, tm->tm_sec
		);
		ret = mktime(tm);
	EXIT:
		return ret;
	}
	int rtc_set(time_t t)
	{
		int ret = 0;
#		pragma pack(push, 1)
		struct {
			uint8_t seconds;
			uint8_t minutes;
			uint8_t hours;
			uint8_t days;
			uint8_t weekdays;
			uint8_t months;
			uint8_t years;
		} time_date[1];
#		pragma pack(pop)
		struct tm *tm;
#		define int_to_bcd(i) ((((i) / 10 - (i) / 100 * 10) << 4) | ((i) - (i) / 10 * 10))
		if (-1 == t)
		{
			DEBUG_PRINTF("rtc: set (error)\n");
			ret = 1;
			goto EXIT;
		}
		tm = gmtime(&t);
		if (NULL == tm)
		{
			DEBUG_PRINTF("rtc: set (error)\n");
			ret = 1;
			goto EXIT;
		}
		time_date->years = int_to_bcd(tm->tm_year - 100); // -2000
		time_date->months = int_to_bcd(tm->tm_mon + 1);   // 1 - 12 (Jan - Dec)
		time_date->days = int_to_bcd(tm->tm_mday);        // 1 - 31
		time_date->weekdays = int_to_bcd(tm->tm_wday);    // 0 - 6  (Sun - Sat)
		time_date->hours = int_to_bcd(tm->tm_hour);       // 0 - 23
		time_date->minutes = int_to_bcd(tm->tm_min);      // 0 - 59
		time_date->seconds = int_to_bcd(tm->tm_sec);      // 0 - 59
		if (0 != _rtc_reg_write(_RTC_SECONDS_REG, (uint8_t *)time_date, sizeof(*time_date)))
		{
			DEBUG_PRINTF("rtc: set (error)\n");
			ret = 1;
			goto EXIT;
		}
#		if defined(DEBUG)
			DEBUG_PRINTF(
				"rtc: set (year=%d, mon=%d, mday=%d, wday=%d, hour=%d, min=%d, sec=%d)\n",
				tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_wday, tm->tm_hour, tm->tm_min, tm->tm_sec
			);
			DEBUG_PRINTF("rtc: set -> ");
			if (-1 == (t = rtc_get()))
			{
				goto EXIT;
			}
			if (NULL == (tm = gmtime(&t)))
			{
				goto EXIT;
			}
			DEBUG_PRINTF(
				"rtc: set (get_year=%d, get_mon=%d, get_mday=%d, get_wday=%d, get_hour=%d, get_min=%d, get_sec=%d))\n",
				tm->tm_year, tm->tm_mon, tm->tm_mday, tm->tm_wday, tm->tm_hour, tm->tm_min, tm->tm_sec
			);
#		endif
	EXIT:
		return ret;
	}
#endif

/* Network API
 * - int net_begin()
 * - int net_end()
 */

int net_begin() {return 0;}
int net_end() {return 0;}

/* UDP API
 * - int udp_begin_nb()
 * - int udp_begin()
 * - int udp_end()
 * - ssize_t udp_send_to(char *address, int port, unsigned char *packet, size_t size)
 * - ssize_t udp_recv_from(char *address, int *port, unsigned char *packet, size_t size)
 */

static int _net_begin_result = 1;
WiFiUDP udp;
int udp_begin_nb()
{
	extern char arg_ssid[];
	extern char arg_pass[];
	int ret = 1;
	if (0 < _net_begin_result)
	{
		WiFi.beginNoBlock(arg_ssid, arg_pass);
		ret = _net_begin_result = -1;
	}
	if (-1 == _net_begin_result)
	{
		switch (WiFi.status())
		{
		case WL_CONNECTED:
			_net_begin_result = 0;
			break;
		case WL_CONNECT_FAILED:
			_net_begin_result = 1;
			break;
		default:
			break;
		}
		ret = _net_begin_result;
	}
	if (0 == _net_begin_result)
	{
		ret = udp.begin(0) ? 0 : 1;
		if (0 != ret)
		{
			WiFi.disconnect();
			_net_begin_result = 1;
		}
	}
	return ret;
}
int udp_begin()
{
	int ret;
	while (-1 == (ret = udp_begin_nb())) system_yield();
	return ret;
}
int udp_end()
{
	udp.stop();
	WiFi.disconnect();
	_net_begin_result = 1;
	return 0;
}
ssize_t udp_send_to(char *address, int port, unsigned char *packet, size_t size)
{
	ssize_t ret;
	IPAddress ip;
	if (0 == WiFi.hostByName(address, ip))
	{
		ret = -1;
		goto EXIT;
	}
	DEBUG_PRINTF("udp: send (adrs=%s, ip_adrs=%s)\n", address, ip.toString().c_str());
	strncpy(address, ip.toString().c_str(), MAX_SIZE_NET_ADDRESS_STRING);
	address[MAX_SIZE_NET_ADDRESS_STRING - 1] = '\0';
	if (0 == udp.beginPacket(address, port))
	{
		ret = -1;
		goto EXIT;
	}
	ret = udp.write(packet, size);
	udp.endPacket();
EXIT:
#	if defined(DEBUG)
		if (0 <= ret) DEBUG_PRINTF("udp: send (adrs=%s, port=%d, size=%lu)\n", address, port, ret);
#	endif
	return ret;
}
ssize_t udp_recv_from(char *address, int *port, unsigned char *packet, size_t size)
{
	ssize_t ret;
	if (0 == udp.parsePacket())
	{
		ret = -1;
		goto EXIT;
	}
	ret = udp.read(packet, size);
	if (0 <= ret)
	{
		strncpy(address, udp.remoteIP().toString().c_str(), MAX_SIZE_NET_ADDRESS_STRING);
		address[MAX_SIZE_NET_ADDRESS_STRING - 1] = '\0';
		*port = udp.remotePort();
	}
EXIT:
#if defined(DEBUG)
		if (0 <= ret) DEBUG_PRINTF("udp: recv (adrs=%s, port=%d, size=%lu)\n", address, *port, ret);
#endif
	return ret;
}

/* Character Display (16 x 2) API
 * - int cdp16x2_begin()
 * - int cdp16x2_end()
 * - void cdp16x2_home()
 * - int cdp16x2_printf(char const *format, ...)
 * - int cdp16x2_putc(int c)
 */

#define _CDP16X2_SDA_GPIO (6)
#define _CDP16X2_SCL_GPIO (7)
#define _CDP16X2_ADDR (0x3e)
#define _CDP16X2_RGBADDR (0x60)
#define _CDP16X2_COLS (16)
#define _CDP16X2_ROWS (2)
#define _CDP16X2_MAX_CURSOR (_CDP16X2_COLS * _CDP16X2_ROWS)
#define _CDP16X2_RGB (0)
DFRobot_RGBLCD1602 lcd(_CDP16X2_RGBADDR , _CDP16X2_COLS, _CDP16X2_ROWS, &Wire1, _CDP16X2_ADDR);
static uint8_t _cdp16x2_cursor;
#if 0 == _CDP16X2_RGB
	static unsigned _cdp16x2_rgb(unsigned color)
	{
		unsigned ret;
		if      (color <  60) ret = (255u << 16) | ((color * 60 * 255 / 3600) << 8) | (0 << 0);
		else if (color < 120) ret = (((120 - color) * 60 * 255 / 3600) << 16) | (255u << 8) | (0 << 0);
		else if (color < 180) ret = (0 << 16) | (255u << 8) | (((color - 120) * 60 * 255 / 3600) << 0);
		else if (color < 240) ret = (0 << 16) | (((240 - color) * 60 * 255 / 3600) << 8) | (255u << 0);
		else if (color < 300) ret = (((color - 240) * 60 * 255 / 3600) << 16) | (0 << 8) | (255u << 0);
		else if (color < 360) ret = (255u << 16) | (0 << 8) | (((360 - color) * 60 * 255 / 3600) << 0);
		else                  ret = (255u << 16) | (255u << 8) | (255u << 0);
		return ret;
	}
#endif
#define _cdp16x2_r(c) (((c) >> 16) & 0xff)
#define _cdp16x2_g(c) (((c) >>  8) & 0xff)
#define _cdp16x2_b(c) (((c) >>  0) & 0xff)
int cdp16x2_begin() {
	Wire1.setSDA(_CDP16X2_SDA_GPIO);
	Wire1.setSCL(_CDP16X2_SCL_GPIO);
	lcd.init();
	lcd.clear();
#	if 0 == _CDP16X2_RGB
		lcd.setRGB(0, 0, 0);
#	else
		lcd.setRGB(_cdp16x2_r(_CDP16X2_RGB), _cdp16x2_g(_CDP16X2_RGB), _cdp16x2_b(_CDP16X2_RGB));
#	endif
	_cdp16x2_cursor = 0;
	return 0;
}
int cdp16x2_end() {return 0;}
void cdp16x2_home()
{
#	if 0 == _CDP16X2_RGB
		extern volatile unsigned tick_counter;
		extern unsigned arg_wave_rate;
		unsigned const rgb = _cdp16x2_rgb((tick_counter / arg_wave_rate) % 360);
		lcd.setRGB(_cdp16x2_r(rgb), _cdp16x2_g(rgb), _cdp16x2_b(rgb));
#	endif
	lcd.setCursor(0, 0);
	_cdp16x2_cursor = 0;
#	if 0 == _CDP16X2_RGB
		DEBUG_PRINTF("(%06x) <\n", rgb);
#	else
		DEBUG_PRINTF(" <\n");
#	endif
}
int cdp16x2_printf(char const *format, ...)
{
	int ret;
	va_list ap;
	char str[64];
	va_start(ap, format);
	vsnprintf(str, sizeof(str), format, ap);
	for (ret = 0 ; ('\0' != str[ret]) && (-1 != cdp16x2_putc(str[ret])) ; ret++);
	return ret;
}
int cdp16x2_putc(int c)
{
	int ret;
	if (_cdp16x2_cursor < _CDP16X2_MAX_CURSOR)
	{
		lcd.write(c);
		DEBUG_WRITE(c);
		_cdp16x2_cursor++;
		if (0 == (_cdp16x2_cursor % _CDP16X2_COLS))
		{
			if (_cdp16x2_cursor < _CDP16X2_MAX_CURSOR)
			{
				lcd.setCursor(0, _cdp16x2_cursor / _CDP16X2_COLS);
			}
			else
			{
				_cdp16x2_cursor = 0;
				lcd.setCursor(0, 0);
			}
			DEBUG_PRINTF(" <\n");
		}
		ret = c;
	}
	else
	{
		ret = -1;
	}
	return ret;
}

/* Analog Out API
 * - int aout_begin()
 * - int aout_end()
 * - void aout_write(struct aout_value const &av)
 * - void aout_conv_value(uint16_t v, struct aout_value &av)
 * - struct aout_value *aout_value_min
 * - struct aout_value *aout_value_max
 * - struct aout_value *aout_value_mid
 */

#define _AOUT_GPIO (8)
#define _AOUT_DIV (1)
#define _AOUT_WRAP_SHIFT (10)
#define _AOUT_WRAP ((1 << _AOUT_WRAP_SHIFT) - 1)
static uint _aout_slice;
static uint _aout_chan;
struct aout_value
{
	uint16_t value;
};
struct aout_value const aout_value_min[1] = {{0}};
struct aout_value const aout_value_max[1] = {{0xffff >> (16 - _AOUT_WRAP_SHIFT)}};
struct aout_value const aout_value_mid[1] = {{0x8000 >> (16 - _AOUT_WRAP_SHIFT)}};
int aout_begin()
{
	pinMode(_AOUT_GPIO, OUTPUT);
	gpio_set_function(_AOUT_GPIO, GPIO_FUNC_PWM);
	_aout_slice = pwm_gpio_to_slice_num(_AOUT_GPIO);
	_aout_chan = pwm_gpio_to_channel(_AOUT_GPIO);
	pwm_set_clkdiv(_aout_slice, (float)_AOUT_DIV);
	pwm_set_clkdiv_mode(_aout_slice, PWM_DIV_FREE_RUNNING);
	pwm_set_phase_correct(_aout_slice, true);
	pwm_set_wrap(_aout_slice, _AOUT_WRAP);
	pwm_set_enabled(_aout_slice, true);
	pwm_set_chan_level(_aout_slice, _aout_chan, 0);
	return 0;
}
int aout_end() {return 0;}
#define aout_write(av) pwm_set_chan_level(_aout_slice, _aout_chan, (av)->value)
#define aout_conv_value(v, av) do{(av)->value = (v) >> (16 - _AOUT_WRAP_SHIFT);}while (0)

/* PWM Out API
 * - int pout_begin()
 * - int pout_end()
 * - void pout_period(int us)
 * - void pout_write(struct pout_value const &pv)
 * - void pout_conv_value(uint16_t v, struct pout_value &pv)
 * - struct pout_value *pout_value_min
 * - struct pout_value *pout_value_max
 * - struct pout_value *pout_value_mid
 */

#define _POUT_GPIO (10)
#define _POUT_DIV (256)
static uint16_t _pout_sys_clk_mhz;
static uint16_t _pout_wrap;
static uint _pout_slice;
static uint _pout_chan;
struct pout_value
{
	uint16_t value;
};
struct pout_value pout_value_min[1] = {{0}};
struct pout_value pout_value_max[1] = {{0}};
struct pout_value pout_value_mid[1] = {{0}};
int pout_begin()
{
	_pout_sys_clk_mhz = clock_get_hz(clk_sys) / 1000000;
	pinMode(_POUT_GPIO, OUTPUT);
	gpio_set_function(_POUT_GPIO, GPIO_FUNC_PWM);
	_pout_wrap = 0;
	_pout_slice = pwm_gpio_to_slice_num(_POUT_GPIO);
	_pout_chan = pwm_gpio_to_channel(_POUT_GPIO);
	pwm_set_clkdiv(_pout_slice, (float)_POUT_DIV);
	pwm_set_wrap(_pout_slice, _pout_wrap);
	pwm_set_enabled(_pout_slice, true);
	pwm_set_chan_level(_pout_slice, _pout_chan, 0);
	pout_value_min->value = 0;
	pout_value_max->value = 0;
	pout_value_max->value = 0;
	return 0;
}
int pout_end() {return 0;}
void pout_period(int us)
{
	_pout_wrap = (0 < us) ? (us * _pout_sys_clk_mhz / _POUT_DIV - 1) : 0;
	pwm_set_wrap(_pout_slice, _pout_wrap);
	pout_value_min->value = 0;
	pout_value_max->value = _pout_wrap;
	pout_value_mid->value = (_pout_wrap >> 1) + 1;
}
#define pout_write(pv) pwm_set_chan_level(_pout_slice, _pout_chan, (pv)->value)
#define pout_conv_value(v, pv) do{(pv)->value = (uint32_t)_pout_wrap * (v) / 0xffff}while (0)

/* Interval Timer API
 * - volatile unsigned tick_counter
 * - int itimer_begin(void (*f)(), int us)
 * - int itimer_end()
 */

#define _IS_ITIMER_ON_CORE_1 (1)
#define itimer_callback_func(f) __not_in_flash_func(f)
static repeating_timer_t _itimer_rt[1];
volatile unsigned tick_counter = 0;
static bool itimer_callback_func(_itimer_rt_callback)(repeating_timer_t *rt) {((void (*)(void))(rt->user_data))(); tick_counter++; return true;}
#if _IS_ITIMER_ON_CORE_1
#	define _ITIMER_CMD_NOP (0)
#	define _ITIMER_CMD_ADD_REPEATING_TIMER_US (3)
#	define _ITIMER_CMD_CANCEL_REPEATING_TIMER (4)
	static void (*_itimer_callback)();
	static int _itimer_delay_us;
	int itimer_begin(void (*f)(), int us)
	{
		_itimer_callback = f;
		_itimer_delay_us = us;
		rp2040.fifo.push(_ITIMER_CMD_ADD_REPEATING_TIMER_US);
		return (int)rp2040.fifo.pop();
	}
	int itimer_end()
	{
		rp2040.fifo.push(_ITIMER_CMD_CANCEL_REPEATING_TIMER);
		return (int)rp2040.fifo.pop();
	}
#else
	int itimer_begin(void (*f)(), int us) {return add_repeating_timer_us(us, _itimer_rt_callback, (void *)(f), _itimer_rt) ? 0 : 1;}
	int itimer_end() {return cancel_repeating_timer(_itimer_rt) ? 0 : 1;}
#endif

/* Digital Out API
 * - int dout_begin()
 * - int dout_end()
 * - void dout_write(int n, int v)
 */

#define NTP_VALID_LED (0)
#define TICK_TACK_LED(n) (1)
#define NUM_TICK_TACK_LED (1 << 0)
static int const _dout_gpio[] = {12, 13};
int dout_begin()
{
	int i;
	for (i = 0 ; i < (int)(sizeof(_dout_gpio) / sizeof(_dout_gpio[0])) ; i++)
	{
		pinMode(_dout_gpio[i], OUTPUT);
	}
	return 0;
}
int dout_end()
{
	int i;
	for (i = 0 ; i < (int)(sizeof(_dout_gpio) / sizeof(_dout_gpio[0])) ; i++)
	{
		pinMode(_dout_gpio[i], INPUT);
	}
	return 0;
}
#define dout_write(n, v) digitalWrite(_dout_gpio[n], v ? 1 : 0)

/* Digital In API
 * - int din_begin()
 * - int din_end()
 * - int din_read()
 */

#define _DIN_GPIO (14)
int din_begin() {pinMode(_DIN_GPIO, INPUT); return 0;}
int din_end() {return 0;}
#define din_read() (digitalRead(_DIN_GPIO) ? 0 : 1)

/* File System API
 * - int fs_begin()
 * - int fs_end()
 * - FS *fs_open(char const *name, char const *mode)
 * - void fs_close(FS *file)
 * - size_t fs_read(void *data, size_t size, FS *file)
 * - char *fs_gets(char *s, int size, FS *file)
 */

#define _SD_SS_PIN (PIN_SPI0_SS)
#define _FILE_BUFFER_SIZE (30 * 1024)
struct fsys
{
	RP2040_SDLib::File file;
	unsigned char _buffer[_FILE_BUFFER_SIZE];
	unsigned char *buffer;
	size_t buffer_size;
};
#define FS struct fsys
int fs_begin() {return SD.begin(_SD_SS_PIN) ? 0 : 1;}
int fs_end() {return 0;}
FS *fs_open(char const *name, char const *mode)
{
	int i;
	FS *file = (FS *)calloc(1, sizeof(FS));
	FS *ret = NULL;
#	define FOPEN_RETRY_COUNT (3)
	(void)mode; //read only

	if (NULL != file)
	{
		for (i = 0 ; i < FOPEN_RETRY_COUNT ; i++)
		{
			file->file = SD.open(name, FILE_READ);
			if (file->file) break;
		}
		if (file->file)
		{
			file->buffer = file->_buffer;
			file->buffer_size = 0;
			ret = file;
			DEBUG_PRINTF("fs: open (name=%s, handle=%p)\n", name, ret);
		}
		else
		{
			free(file);
		}
	}

	return ret;
}
void fs_close(FS *file)
{
	DEBUG_PRINTF("fs: close (handle=%p)\n", file);
	file->file.close();
	free(file);
}
size_t fs_read(void *data, size_t size, FS *file)
{
	unsigned char *p = (unsigned char *)data;
	size_t s = size;
	ssize_t ss;

	while (0 < s)
	{
		if (file->buffer_size < s)
		{
			memcpy(p, file->buffer, file->buffer_size);
			p += file->buffer_size;
			s -= file->buffer_size;

			ss = file->file.read(file->buffer = file->_buffer, _FILE_BUFFER_SIZE);
			file->buffer_size = 0; if (0 < ss) file->buffer_size = ss;
			DEBUG_PRINTF("fs: read (file.read_size=%lu, file_handle=%p)\n", file->buffer_size, &(file->file));
			if (0 == file->buffer_size)
			{
				break;
			}
		}
		else
		{
			memcpy(p, file->buffer, s);
			file->buffer += s;
			file->buffer_size -= s;
			p += s;
			s = 0;
		}
	}

	//DEBUG_PRINTF("fs: read (size=%lu, handle=%p)\n", size - s, file);

	return size - s;
}
char *fs_gets(char *s, int size, FS *file)
{
	int i;
	for (i = 0 ; i < size ; i++)
	{
		if (0 == fs_read(s + i, 1, file))
		{
			s[i] = '\n';
		}
		if ('\n' == s[i])
		{
			i++;
			break;
		}
	}
	if ((size == i) && ('\n' != s[i - 1]))
	{
		for ( ; ; )
		{
			if (0 == fs_read(s + (i - 1), 1, file))
			{
				s[i - 1] = '\n';
				break;
			}
			if ('\n' == s[i - 1])
			{
				break;
			}
		}
	}
	s[i - 1] = '\0';
	return (1 < i) ? s : NULL;
}

/* ---------------------------------------------------------------- */

unsigned arg_wave_rate = 64000;
unsigned arg_melody_tempo = 60;
#if 0 < MAX_SIZE_SSID_STRING
	char arg_ssid[MAX_SIZE_SSID_STRING] = "";
#endif
#if 0 < MAX_SIZE_PASS_STRING
	char arg_pass[MAX_SIZE_PASS_STRING] = "";
#endif
int arg_timezone = 0;
char arg_ntp_server[MAX_SIZE_NET_ADDRESS_STRING] = "0.pool.ntp.org";
struct tm arg_ntp_time[1] = {{56, 34, 12, -1, -1, -1, -1, 0, 0}};
unsigned arg_disp_period = 1;
/*
 * %Y      : year (4 numeric char.)
 * %y      : year (2 numeric char.)
 * %M      : month (3 char.)
 * %N      : month (2 numeric char.)
 * %D      : day (2 numeric char.)
 * %W      : week (3 char.)
 * %h      : hour (2 numeric char.)
 * %m      : minute (2 numeric char.)
 * %s      : second (2 numeric char.)
 * %%      : % (1 char.)
 */
char arg_disp[MAX_SIZE_DISP_STRING] = "%Y/%M/%D(%W)    %h:%m:%s";
/*
 * [num]   : voice time positive offset (in second, line top only)
 * +[num]  : voice time positive offset (in second, line top only)
 * -[num]  : voice time negative offset (in second, line top only)
 * Y       : year (WAVE_PATH "yar/####.wav")
 * y       : year (WAVE_PATH "yar/##.wav")
 * M       : month (WAVE_PATH "mon/$$$.wav")
 * D       : day (WAVE_PATH "day/##.wav")
 * W       : week (WAVE_PATH "wek/$$$.wav")
 * h       : hour (WAVE_PATH "hor/##.wav")
 * m       : minute (WAVE_PATH "min/##.wav")
 * s       : second (WAVE_PATH "sec/##.wav")
 * d       : month & day (WAVE_PATH "dat/$$$##.wav")
 * t       : hour & minute (WAVE_PATH "tim/####.wav")
 * (others): user defined (WAVE_PATH "msc/$.wav")
 */
char arg_voice[MAX_SIZE_VOICE_STRING] = "MDWhm";
struct
{
	struct tm tm;
	char voice[MAX_SIZE_VOICE_STRING];
} arg_signal[MAX_NUM_SIGNAL] = {0};
struct
{
	struct tm tm;
	char melody[MAX_SIZE_MELODY_STRING];
} arg_chime[MAX_NUM_CHIME] = {0};

static const char _ini_file_name[] = INI_PATH "vc.ini";
static const char * const _month_str[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char * const _week_str[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static char const _year_wave_path[] = WAVE_PATH "yar/";
static char const _month_wave_path[] = WAVE_PATH "mon/";
static char const _day_wave_path[] = WAVE_PATH "day/";
static char const _week_wave_path[] = WAVE_PATH "wek/";
static char const _hour_wave_path[] = WAVE_PATH "hor/";
static char const _minute_wave_path[] = WAVE_PATH "min/";
static char const _second_wave_path[] = WAVE_PATH "sec/";
static char const _date_wave_path[] = WAVE_PATH "dat/";
static char const _time_wave_path[] = WAVE_PATH "tim/";
static char const _misc_wave_path[] = WAVE_PATH "msc/";
static unsigned const _wave_format = 1;
static unsigned _wave_bytes = 2;
static unsigned _wave_bits = _wave_bytes * 8;
static unsigned _wave_rate = 8000;
static unsigned const _wave_ch = 1;
static unsigned _wave_oversamp = arg_wave_rate / _wave_rate;
static int _month(char const *M)
{
	int i;
	for (i = 0 ; i < (int)(sizeof(_month_str) / sizeof(_month_str[0])) ; i++)
		if (0 == strcmp(M, _month_str[i])) return i;
	return -1;
}
static int _week(char const *W)
{
	int i;
	for (i = 0 ; i < (int)(sizeof(_week_str) / sizeof(_week_str[0])) ; i++)
		if (0 == strcmp(W, _week_str[i])) return i;
	return -1;
}
static unsigned const _melody_scale[] = {440000, 493883, 261626, 293665, 329628, 349228, 391995};

static struct
{
	struct aout_value queue[WAVE_QUEUE_SIZE];
	unsigned queue_head;
	unsigned queue_tail;
	char file_name_queue[WAVE_FILE_NAME_QUEUE_SIZE][32];
	unsigned file_name_queue_head;
	unsigned file_name_queue_tail;
} _wave_ctx[1] = {{0}};

static struct
{
	int state;
	int is_udp_ok;
	time_t t1;
	time_t t2;
	time_t t3;
	time_t t4;
	char ntp_server_address[MAX_SIZE_NET_ADDRESS_STRING];
	int ntp_server_port;
} _set_time_ntp_ctx[1] = {{0}};

#define _b2u(b, n) _b2u_ ## n(b)
#define _b2u_2(b) ltohs(*(uint16_t *)(b))
#define _b2u_4(b) ltohl(*(uint32_t *)(b))

enum
{
	SET_TIME_NTP_STATE_FAILURE = 0,
	SET_TIME_NTP_STATE_INIT,
	SET_TIME_NTP_STATE_SEND,
	SET_TIME_NTP_STATE_WAIT,
	SET_TIME_NTP_STATE_SUCCESS
};

#pragma pack(push, 1)
struct ntp_packet
{
	uint8_t li_vn_mode;
	uint8_t stratum;
	uint8_t poll;
	uint8_t precision;

	uint32_t root_delay;
	uint32_t root_dispersion;
	uint32_t ref_id;

	uint32_t ref_time_w;
	uint32_t ref_time_f;
	uint32_t org_time_w;
	uint32_t org_time_f;
	uint32_t rx_time_w;
	uint32_t rx_time_f;
	uint32_t tx_time_w;
	uint32_t tx_time_f;
};
#pragma pack(pop)

static void _set_time_ntp_nb()
{
	switch (_set_time_ntp_ctx->state)
	{
	case SET_TIME_NTP_STATE_SUCCESS:
	case SET_TIME_NTP_STATE_FAILURE:
		_set_time_ntp_ctx->state = SET_TIME_NTP_STATE_INIT;
		_set_time_ntp_ctx->is_udp_ok = 0;
		break;
	default:
		break;
	}
}

static int _set_time_ntp_result()
{
	int ret;

	switch (_set_time_ntp_ctx->state)
	{
	case SET_TIME_NTP_STATE_SUCCESS:
		ret = 0;
		break;
	case SET_TIME_NTP_STATE_FAILURE:
		ret = 1;
		break;
	default:
		ret = -1;
		break;
	}

	return ret;
}

static void _set_time_ntp_yield()
{
	int i;
	struct ntp_packet p[1] = {{0}};
	time_t t;
	char address[MAX_SIZE_NET_ADDRESS_STRING];
	int port;

	switch (_set_time_ntp_ctx->state)
	{
	case SET_TIME_NTP_STATE_INIT:
		i = udp_begin_nb();
		if (0 != i)
		{
			if (0 < i)
			{
				_set_time_ntp_ctx->state = SET_TIME_NTP_STATE_FAILURE;
			}
			break;
		}
		_set_time_ntp_ctx->is_udp_ok = 1;
		strncpy(_set_time_ntp_ctx->ntp_server_address, arg_ntp_server, MAX_SIZE_NET_ADDRESS_STRING);
		_set_time_ntp_ctx->ntp_server_address[MAX_SIZE_NET_ADDRESS_STRING - 1] = '\0';
		_set_time_ntp_ctx->ntp_server_port = 123;
		_set_time_ntp_ctx->state = SET_TIME_NTP_STATE_SEND;
		break;
	case SET_TIME_NTP_STATE_SEND:
		p->li_vn_mode = (0 << 6) | (3 << 3) | 3;
		_set_time_ntp_ctx->t1 = clk_get();
		p->tx_time_w = htonl(_set_time_ntp_ctx->t1 + 2208988800);
		if (udp_send_to(_set_time_ntp_ctx->ntp_server_address, _set_time_ntp_ctx->ntp_server_port, (unsigned char *)p, sizeof(*p)) < (ssize_t)sizeof(*p))
		{
			_set_time_ntp_ctx->state = SET_TIME_NTP_STATE_FAILURE;
			break;
		}
		_set_time_ntp_ctx->state = SET_TIME_NTP_STATE_WAIT;
		break;
	case SET_TIME_NTP_STATE_WAIT:
		if (udp_recv_from(address, &port, (unsigned char *)p, sizeof(*p)) != (ssize_t)sizeof(*p))
		{
			if (NTP_TIMEOUT <= (clk_get() - _set_time_ntp_ctx->t1))
			{
				_set_time_ntp_ctx->state = SET_TIME_NTP_STATE_FAILURE;
			}
			break;
		}
		_set_time_ntp_ctx->t4 = clk_get();
		if (0 != strcmp(address, _set_time_ntp_ctx->ntp_server_address) ||
			(port != _set_time_ntp_ctx->ntp_server_port))
		{
			_set_time_ntp_ctx->state = SET_TIME_NTP_STATE_FAILURE;
			break;
		}
		if (0 == p->stratum)
		{
			_set_time_ntp_ctx->state = SET_TIME_NTP_STATE_FAILURE;
			break;
		}
		p->ref_time_w = ntohl(p->ref_time_w);
		p->ref_time_f = ntohl(p->ref_time_f);
		p->org_time_w = ntohl(p->org_time_w);
		p->org_time_f = ntohl(p->org_time_f);
		p->rx_time_w = ntohl(p->rx_time_w);
		p->rx_time_f = ntohl(p->rx_time_f);
		p->tx_time_w = ntohl(p->tx_time_w);
		p->tx_time_f = ntohl(p->tx_time_f);
		_set_time_ntp_ctx->t2 = p->rx_time_w;
		_set_time_ntp_ctx->t3 = p->tx_time_w;
		t = ((_set_time_ntp_ctx->t3 < (time_t)0x80000000) ? (_set_time_ntp_ctx->t3 + 0x100000000) : _set_time_ntp_ctx->t3) - 2208988800;
		clk_set(t + ((_set_time_ntp_ctx->t4 - _set_time_ntp_ctx->t1) - (_set_time_ntp_ctx->t3 - _set_time_ntp_ctx->t2)) / 2);
		_set_time_ntp_ctx->state = SET_TIME_NTP_STATE_SUCCESS;
		break;
	default:
		break;
	}

	switch (_set_time_ntp_ctx->state)
	{
	case SET_TIME_NTP_STATE_SUCCESS:
	case SET_TIME_NTP_STATE_FAILURE:
		if (_set_time_ntp_ctx->is_udp_ok)
		{
			udp_end();
			_set_time_ntp_ctx->is_udp_ok = 0;
		}
		break;
	default:
		break;
	}
}

static int _set_time_ntp()
{
	int ret;
	_set_time_ntp_nb();
	while (-1 == (ret = _set_time_ntp_result())) {_set_time_ntp_yield(); system_yield();}
	return ret;
}

static void _disp(time_t t)
{
	int i;
	int j;
	struct tm *tm = clk_gettm(t);
	int is_in_conv = 0;

	cdp16x2_home();

	if (NULL == tm)
	{
		goto EXIT;
	}

	for (i = 0, j = 0 ; ('\0' != arg_disp[i]) && (j < 32) ; i++)
	{
		if (is_in_conv)
		{
			switch (arg_disp[i])
			{
			case 'Y':
				j += cdp16x2_printf("%04d", (tm->tm_year + 1900) % 10000);
				break;
			case 'y':
				j += cdp16x2_printf("%02d", (tm->tm_year + 1900) % 100);
				break;
			case 'M':
				j += cdp16x2_printf("%s", _month_str[tm->tm_mon]);
				break;
			case 'N':
				j += cdp16x2_printf("%02d", tm->tm_mon + 1);
				break;
			case 'D':
				j += cdp16x2_printf("%02d", tm->tm_mday);
				break;
			case 'W':
				j += cdp16x2_printf("%s", _week_str[tm->tm_wday]);
				break;
			case 'h':
				j += cdp16x2_printf("%02d", tm->tm_hour);
				break;
			case 'm':
				j += cdp16x2_printf("%02d", tm->tm_min);
				break;
			case 's':
				j += cdp16x2_printf("%02d", tm->tm_sec);
				break;
			default:
				cdp16x2_putc(arg_disp[i]);
				j++;
				break;
			}
			is_in_conv = 0;
		}
		else
		{
			switch (arg_disp[i])
			{
			case '%':
				is_in_conv = 1;
				break;
			default:
				cdp16x2_putc(arg_disp[i]);
				j++;
				break;
			}
		}
	}
	for ( ; j < 32 ; j++) cdp16x2_putc(' ');

EXIT:

	return;
}

static void _tick_tack(time_t t)
{
#	if 1 < NUM_TICK_TACK_LED
		int i;
		for (i = 0 ; i < NUM_TICK_TACK_LED ; i++)
		{
			dout_write(TICK_TACK_LED(i), (t & (NUM_TICK_TACK_LED - 1)) == i);
		}
#	else
		dout_write(TICK_TACK_LED(0), t & 1);
#	endif
}

static void _ntp_valid(int set_time_ntp_result, time_t t)
{
	dout_write(
		NTP_VALID_LED,
		((0 == set_time_ntp_result) || ((-1 == set_time_ntp_result) && (t & 1))) ? 1 : 0
	);
}

static int _num_len(char const *v)
{
	int i;
	int j = 0;
	if (('+' == v[j]) || ('-' == v[j])) j++;
	for (i = j ; ('0' <= v[i]) && (v[i] <= '9') ; i++);
	if (j == i) i = 0;
	return i;
}

static void _speak(char const *v, time_t t)
{
	int i;
	struct tm *tm;
	i = _num_len(v);
	tm = clk_gettm(t + ((0 < i) ? atoi(v) : 0));
	if (NULL == tm)
	{
		goto EXIT;
	}
	for ( ;
		('\0' != v[i]) && ((_wave_ctx->file_name_queue_head - _wave_ctx->file_name_queue_tail) < WAVE_FILE_NAME_QUEUE_SIZE) ;
		i++, _wave_ctx->file_name_queue_head++)
	{
		switch (v[i])
		{
		case 'Y':
			snprintf(_wave_ctx->file_name_queue[_wave_ctx->file_name_queue_head & WAVE_FILE_NAME_QUEUE_SIZE_MASK], sizeof(_wave_ctx->file_name_queue[0]),
				"%s%04d.wav", _year_wave_path, (tm->tm_year + 1900) % 10000);
			break;
		case 'y':
			snprintf(_wave_ctx->file_name_queue[_wave_ctx->file_name_queue_head & WAVE_FILE_NAME_QUEUE_SIZE_MASK], sizeof(_wave_ctx->file_name_queue[0]),
				"%s%02d.wav", _year_wave_path, (tm->tm_year + 1900) % 100);
			break;
		case 'M':
			snprintf(_wave_ctx->file_name_queue[_wave_ctx->file_name_queue_head & WAVE_FILE_NAME_QUEUE_SIZE_MASK], sizeof(_wave_ctx->file_name_queue[0]),
				"%s%s.wav", _month_wave_path, _month_str[tm->tm_mon]);
			break;
		case 'D':
			snprintf(_wave_ctx->file_name_queue[_wave_ctx->file_name_queue_head & WAVE_FILE_NAME_QUEUE_SIZE_MASK], sizeof(_wave_ctx->file_name_queue[0]),
				"%s%02d.wav", _day_wave_path, tm->tm_mday);
			break;
		case 'W':
			snprintf(_wave_ctx->file_name_queue[_wave_ctx->file_name_queue_head & WAVE_FILE_NAME_QUEUE_SIZE_MASK], sizeof(_wave_ctx->file_name_queue[0]),
				"%s%s.wav", _week_wave_path, _week_str[tm->tm_wday]);
			break;
		case 'h':
			snprintf(_wave_ctx->file_name_queue[_wave_ctx->file_name_queue_head & WAVE_FILE_NAME_QUEUE_SIZE_MASK], sizeof(_wave_ctx->file_name_queue[0]),
				"%s%02d.wav", _hour_wave_path, tm->tm_hour);
			break;
		case 'm':
			snprintf(_wave_ctx->file_name_queue[_wave_ctx->file_name_queue_head & WAVE_FILE_NAME_QUEUE_SIZE_MASK], sizeof(_wave_ctx->file_name_queue[0]),
				"%s%02d.wav", _minute_wave_path, tm->tm_min);
			break;
		case 's':
			snprintf(_wave_ctx->file_name_queue[_wave_ctx->file_name_queue_head & WAVE_FILE_NAME_QUEUE_SIZE_MASK], sizeof(_wave_ctx->file_name_queue[0]),
				"%s%02d.wav", _second_wave_path, tm->tm_sec);
			break;
		case 'd':
			snprintf(_wave_ctx->file_name_queue[_wave_ctx->file_name_queue_head & WAVE_FILE_NAME_QUEUE_SIZE_MASK], sizeof(_wave_ctx->file_name_queue[0]),
				"%s%s%02d.wav", _date_wave_path, _month_str[tm->tm_mon], tm->tm_mday);
			break;
		case 't':
			snprintf(_wave_ctx->file_name_queue[_wave_ctx->file_name_queue_head & WAVE_FILE_NAME_QUEUE_SIZE_MASK], sizeof(_wave_ctx->file_name_queue[0]),
				"%s%02d%02d.wav", _time_wave_path, tm->tm_hour, tm->tm_min);
			break;
		default:
			snprintf(_wave_ctx->file_name_queue[_wave_ctx->file_name_queue_head & WAVE_FILE_NAME_QUEUE_SIZE_MASK], sizeof(_wave_ctx->file_name_queue[0]),
				"%s%c.wav", _misc_wave_path, v[i]);
			break;
		}
	}
EXIT:
	return;
}

static void _play(char m)
{
	static char prev_m = '_';
	int is_ag = 0;
	if (prev_m == m)
	{
		goto EXIT;
	}
	if ((('A' <= m) && (m <= 'G')) || (is_ag = ('a' <= m) && (m <= 'g')))
	{
		pout_period(1000000000 / _melody_scale[m - (is_ag ? 'a' : 'A')] / (is_ag ? 2 : 1));
		pout_write(pout_value_mid);
	}
	else
	{
		pout_write(pout_value_min);
	}
	prev_m = m;
EXIT:
	return;
}

static void itimer_callback_func(_out_wave)()
{
	if (_wave_ctx->queue_tail == _wave_ctx->queue_head)
	{
		aout_write(aout_value_mid);
	}
	else
	{
		aout_write(_wave_ctx->queue + (_wave_ctx->queue_tail & WAVE_QUEUE_SIZE_MASK));
		_wave_ctx->queue_tail++;
	}
	return;
}

static void _read_wave()
{
	int i;
	static FS *wave_file = NULL;
	static unsigned wave_file_size = 0;
	static uint16_t prev_wave;
	unsigned u;
	unsigned v;
	uint16_t w;
	uint8_t buffer[4];
#   define close_fin() do                  \
	{                                      \
		fs_close(wave_file);               \
		wave_file = NULL;                  \
		wave_file_size = 0;                \
		_wave_ctx->file_name_queue_tail++; \
		goto EXIT;                         \
	} while (0)
#   define dummy_fs_read() do                               \
	{                                                       \
		for ( ; sizeof(buffer) < v ; v -= sizeof(buffer))   \
		{                                                   \
			u = fs_read(buffer, sizeof(buffer), wave_file); \
			if (sizeof(buffer) != u)                        \
			{                                               \
				close_fin();                                \
			}                                               \
		}                                                   \
		u = fs_read(buffer, v, wave_file);                  \
		if (v != u)                                         \
		{                                                   \
			close_fin();                                    \
		}                                                   \
	} while (0)

	if ((NULL == wave_file) && (_wave_ctx->file_name_queue_head != _wave_ctx->file_name_queue_tail))
	{
		wave_file = fs_open(_wave_ctx->file_name_queue[_wave_ctx->file_name_queue_tail & WAVE_FILE_NAME_QUEUE_SIZE_MASK], "r");
		if (NULL == wave_file)
		{
			_wave_ctx->file_name_queue_tail++;
		}
		else
		{
			u = fs_read(buffer, 4, wave_file);
			if ((4 != u) || (0 != memcmp(buffer, "RIFF", 4)))
			{
				close_fin();
			}
			u = fs_read(buffer, 4, wave_file);
			if (4 != u)
			{
				close_fin();
			}
			u = fs_read(buffer, 4, wave_file);
			if ((4 != u) || (0 != memcmp(buffer, "WAVE", 4)))
			{
				close_fin();
			}
			u = fs_read(buffer, 4, wave_file);
			if ((4 != u) || (0 != memcmp(buffer, "fmt ", 4)))
			{
				close_fin();
			}
			u = fs_read(buffer, 4, wave_file);
			if ((4 != u) || ((v = _b2u(buffer, 4)) < 16U))
			{
				close_fin();
			}
			u = fs_read(buffer, 2, wave_file);
			if ((2 != u) || (_wave_format != _b2u(buffer, 2)))
			{
				close_fin();
			}
			u = fs_read(buffer, 2, wave_file);
			if ((2 != u) || (_wave_ch != _b2u(buffer, 2)))
			{
				close_fin();
			}
			u = fs_read(buffer, 4, wave_file);
			if (4 != u)
			{
				close_fin();
			}
			_wave_rate = _b2u(buffer, 4);
			if (arg_wave_rate < _wave_rate)
			{
				close_fin();
			}
			_wave_oversamp = arg_wave_rate / _wave_rate;
			u = fs_read(buffer, 4, wave_file);
			if (4 != u)
			{
				close_fin();
			}
			_wave_bytes = _b2u(buffer, 4) / _wave_ch / _wave_rate;
			_wave_bits = _wave_bytes * 8;
			u = fs_read(buffer, 2, wave_file);
			if ((2 != u) || ((_wave_ch * _wave_bytes) != _b2u(buffer, 2)))
			{
				close_fin();
			}
			u = fs_read(buffer, 2, wave_file);
			if ((2 != u) || (_wave_bits != _b2u(buffer, 2)))
			{
				close_fin();
			}
			v -= 16;
			dummy_fs_read();
			for ( ; ; )
			{
				u = fs_read(buffer, 4, wave_file);
				if (4 != u)
				{
					close_fin();
				}
				if (0 == memcmp(buffer, "data", 4))
				{
					break;
				}
				u = fs_read(buffer, 4, wave_file);
				if (4 != u)
				{
					close_fin();
				}
				v = _b2u(buffer, 4);
				dummy_fs_read();
			}
			u = fs_read(buffer, 4, wave_file);
			if (u != 4)
			{
				close_fin();
			}
			wave_file_size = _b2u(buffer, 4);
			prev_wave = 0x8000;
		}
	}

	if (NULL != wave_file)
	{
		while ((_wave_ctx->queue_head - _wave_ctx->queue_tail) <= (WAVE_QUEUE_SIZE - _wave_oversamp))
		{
			u = fs_read(buffer, _wave_bytes, wave_file);
			if (_wave_bytes != u)
			{
				close_fin();
			}
			wave_file_size -= u;
			switch (_wave_bytes)
			{
			case 1:
				w = (uint16_t)(buffer[0]) << 8;
				break;
			case 2:
				w = (_b2u(buffer, 2) + 0x8000) & 0xffff;
				break;
			default:
				w = 0x8000;
				break;
			}
			if (prev_wave < w)
			{
				v = (unsigned)(w - prev_wave);
				for (i = 1 ; i <= (int)_wave_oversamp ; i++, _wave_ctx->queue_head++)
				{
					aout_conv_value(prev_wave + (uint16_t)(v * i / _wave_oversamp), _wave_ctx->queue + (_wave_ctx->queue_head & WAVE_QUEUE_SIZE_MASK));
				}
			}
			else
			{
				v = (unsigned)(prev_wave - w);
				for (i = 1 ; i <= (int)_wave_oversamp ; i++, _wave_ctx->queue_head++)
				{
					aout_conv_value(prev_wave - (uint16_t)(v * i / _wave_oversamp), _wave_ctx->queue + (_wave_ctx->queue_head & WAVE_QUEUE_SIZE_MASK));
				}
			}
			if (0 == wave_file_size)
			{
				close_fin();
			}
			prev_wave = w;
		}
	}
EXIT:
	return;
}

static char *_tmsscanf(char const *l, struct tm *t)
{
	char *ret = NULL;
	int i;
	char Y[5];
	char M[4];
	char D[4];
	char W[5];
	char h[4];
	char m[4];
	char s[4];
	static char x[256];

	i = sscanf(l, "%4s %3s %2s %3s %2s %2s %2s %255s", Y, M, D, W, h, m, s, x);
	if (7 <= i)
	{
		t->tm_year = ('*' == Y[0]) ? -1 : atoi(Y);
		t->tm_mon = ('*' == M[0]) ? -1 : _month(M);
		t->tm_mday = ('*' == D[0]) ? -1 : atoi(D);
		t->tm_wday = ('*' == W[0]) ? -1 : _week(W);
		t->tm_hour = ('*' == h[0]) ? -1 : atoi(h);
		t->tm_min = ('*' == m[0]) ? -1 : atoi(m);
		t->tm_sec = ('*' == s[0]) ? -1 : atoi(s);
		if (1900 <= t->tm_year)
		{
			t->tm_year -= 1900;
		}
		if (7 == i)
		{
			x[0] = '\0';
		}
		ret = x;
	}

	return ret;
}

static void _scan_arg()
{
	FS *ini_file = NULL;
	char line[256];
	int signal_no = 0;
	int chime_no = 0;
	char *s;
	struct tm tm[1];

	ini_file = fs_open(_ini_file_name, "rt");
	if (NULL == ini_file)
	{
		goto EXIT;
	}

	while (NULL != fs_gets(line, sizeof(line), ini_file))
	{
		if (0 == strncmp(line, "wave_rate=", 10))
		{
			arg_wave_rate = atoi(line + 10);
			if ((0 == arg_wave_rate) || (64000 < arg_wave_rate))
			{
				arg_wave_rate = 64000;
			}
		}
		else if (0 == strncmp(line, "melody_tempo=", 13))
		{
			arg_melody_tempo = atoi(line + 13);
			if (0 == arg_melody_tempo)
			{
				arg_melody_tempo = 60;
			}
		}
		else if (0 == strncmp(line, "ssid=", 5))
		{
#			if 0 < MAX_SIZE_SSID_STRING
				strncpy(arg_ssid, line + 5, sizeof(arg_ssid));
				arg_ssid[sizeof(arg_ssid) - 1] = '\0';
#			endif
		}
		else if (0 == strncmp(line, "pass=", 5))
		{
#			if 0 < MAX_SIZE_PASS_STRING
				strncpy(arg_pass, line + 5, sizeof(arg_pass));
				arg_pass[sizeof(arg_pass) - 1] = '\0';
#			endif
		}
		else if (0 == strncmp(line, "timezone=", 9))
		{
			arg_timezone = atoi(line + 9);
		}
		else if (0 == strncmp(line, "ntp_server=", 11))
		{
			strncpy(arg_ntp_server, line + 11, sizeof(arg_ntp_server));
			arg_ntp_server[sizeof(arg_ntp_server) - 1] = '\0';
		}
		else if (0 == strncmp(line, "ntp_time=", 9))
		{
			s = _tmsscanf(line + 9, tm);
			if (NULL != s)
			{
				memcpy(arg_ntp_time, tm, sizeof(*arg_ntp_time));
			}
		}
		else if (0 == strncmp(line, "disp_period=", 12))
		{
			arg_disp_period = atoi(line + 12);
			if (arg_disp_period < 1)
			{
				arg_disp_period = 1;
			}
		}
		else if (0 == strncmp(line, "disp=", 5))
		{
			strncpy(arg_disp, line + 5, sizeof(arg_disp));
			arg_disp[sizeof(arg_disp) - 1] = '\0';
		}
		else if (0 == strncmp(line, "voice=", 6))
		{
			strncpy(arg_voice, line + 6, sizeof(arg_voice));
			arg_voice[sizeof(arg_voice) - 1] = '\0';
		}
		else if (0 == strncmp(line, "signal=", 7))
		{
			if (signal_no < MAX_NUM_SIGNAL)
			{
				s = _tmsscanf(line + 7, tm);
				if (NULL != s)
				{
					memcpy(&(arg_signal[signal_no].tm), tm, sizeof(arg_signal[signal_no].tm));
					strncpy(arg_signal[signal_no].voice, s, sizeof(arg_signal[signal_no].voice));
					arg_signal[signal_no].voice[sizeof(arg_signal[signal_no].voice) - 1] = '\0';
					signal_no++;
				}
			}
		}
		else if (0 == strncmp(line, "chime=", 6))
		{
			if (chime_no < MAX_NUM_CHIME)
			{
				s = _tmsscanf(line + 6, tm);
				if (NULL != s)
				{
					memcpy(&(arg_chime[chime_no].tm), tm, sizeof(arg_chime[chime_no].tm));
					strncpy(arg_chime[chime_no].melody, s, sizeof(arg_chime[chime_no].melody));
					arg_chime[chime_no].melody[sizeof(arg_chime[chime_no].melody) - 1] = '\0';
					chime_no++;
				}
			}
		}
	}

EXIT:

	if (NULL != ini_file)
	{
		fs_close(ini_file);
	}

	return;
}

static int _is_in_time(struct tm const *tm, struct tm const *t)
{
	return ((-1 == t->tm_year) || ((t->tm_year < 100) ?
			((tm->tm_year % 100) == t->tm_year) :
			((tm->tm_year % 10000) == t->tm_year))) &&
		((-1 == t->tm_mon) || (tm->tm_mon == t->tm_mon)) &&
		((-1 == t->tm_mday) || (tm->tm_mday == t->tm_mday)) &&
		((-1 == t->tm_wday) || (tm->tm_wday == t->tm_wday)) &&
		((-1 == t->tm_hour) || (tm->tm_hour == t->tm_hour)) &&
		((-1 == t->tm_min) || (tm->tm_min == t->tm_min)) &&
		((-1 == t->tm_sec) || (tm->tm_sec == t->tm_sec));
}

static int _main()
{
	int i;
	time_t disp_time;
	time_t t;
	struct tm *tm;
	char const *v = NULL;
	unsigned melody_count;
	unsigned melody_cycle;
	int mc = 1;
	char const *m = NULL;
	char const *ms = NULL;
	char const *mp = NULL;
	int is_sw_on = 0;
	int is_signal_on[MAX_NUM_SIGNAL] = {0};
	int is_chime_on[MAX_NUM_CHIME] = {0};
	int is_net_ok;
#	if IS_RTC_OMITTED
#		define set_time_ntp_nb_proc() do      \
		{                                     \
			if (-1 != _set_time_ntp_result()) \
			{                                 \
				_set_time_ntp_nb();           \
			}                                 \
		} while (0)
#	else
		int ntp_res;
#		define set_time_ntp_nb_proc() do      \
		{                                     \
			ntp_res = _set_time_ntp_result(); \
			if (-1 != ntp_res)                \
			{                                 \
				_set_time_ntp_nb();           \
				ntp_res = -1;                 \
			}                                 \
		} while (0)
#	endif
#	if IS_NTP_RECOVERED_RTC && IS_RTC_OMITTED
#		define set_time_ntp_result_proc() do \
		{                                    \
		} while (0)
#	elif IS_NTP_RECOVERED_RTC && !IS_RTC_OMITTED
#		define set_time_ntp_result_proc() do      \
		{                                         \
			if (-1 == ntp_res)                    \
			{                                     \
				ntp_res = _set_time_ntp_result(); \
				if (0 < ntp_res)                  \
				{                                 \
					time_t t;                     \
					t = rtc_get();                \
					if (-1 == t)                  \
					{                             \
						goto NTP_FAILURE_EXIT;    \
					}                             \
					clk_set(t);                   \
					ntp_res = 0;                  \
				}                                 \
				else if (0 == ntp_res)            \
				{                                 \
					if (0 != rtc_set(clk_get()))  \
					{                             \
						ntp_res = 1;              \
						goto NTP_FAILURE_EXIT;    \
					}                             \
				}                                 \
			}                                     \
		} while (0)
#	elif !IS_NTP_RECOVERED_RTC && IS_RTC_OMITTED
#		define set_time_ntp_result_proc() do \
		{                                    \
			if (0 < _set_time_ntp_result())  \
			{                                \
				goto NTP_FAILURE_EXIT;       \
			}                                \
		} while (0)
#	elif !IS_NTP_RECOVERED_RTC && !IS_RTC_OMITTED
#		define set_time_ntp_result_proc() do      \
		{                                         \
			if (-1 == ntp_res)                    \
			{                                     \
				ntp_res = _set_time_ntp_result(); \
				if (0 < ntp_res)                  \
				{                                 \
					goto NTP_FAILURE_EXIT;        \
				}                                 \
				else if (0 == ntp_res)            \
				{                                 \
					if (0 != rtc_set(clk_get()))  \
					{                             \
						ntp_res = 1;              \
						goto NTP_FAILURE_EXIT;    \
					}                             \
				}                                 \
			}                                     \
		} while (0)
#	endif
#	if IS_INIT_NTP_RECOVERED_RTC && IS_RTC_OMITTED
#		define set_time_ntp_proc() do \
		{                             \
			_set_time_ntp();          \
		} while (0)
#	elif IS_INIT_NTP_RECOVERED_RTC && !IS_RTC_OMITTED
#		define set_time_ntp_proc() do        \
		{                                    \
			ntp_res = _set_time_ntp();       \
			if (0 != ntp_res)                \
			{                                \
				time_t t;                    \
				t = rtc_get();               \
				if (-1 == t)                 \
				{                            \
					goto NTP_FAILURE_EXIT;   \
				}                            \
				clk_set(t);                  \
				ntp_res = 0;                 \
			}                                \
			else                             \
			{                                \
				if (0 != rtc_set(clk_get())) \
				{                            \
					ntp_res = 1;             \
					goto NTP_FAILURE_EXIT;   \
				}                            \
			}                                \
		} while (0)
#	elif !IS_INIT_NTP_RECOVERED_RTC && IS_RTC_OMITTED
#		define set_time_ntp_proc() do  \
		{                              \
			if (0 != _set_time_ntp())  \
			{                          \
				goto NTP_FAILURE_EXIT; \
			}                          \
		} while (0)
#	elif !IS_INIT_NTP_RECOVERED_RTC && !IS_RTC_OMITTED
#		define set_time_ntp_proc() do        \
		{                                    \
			ntp_res = _set_time_ntp();       \
			if (0 != ntp_res)                \
			{                                \
				goto NTP_FAILURE_EXIT;       \
			}                                \
			else                             \
			{                                \
				if (0 != rtc_set(clk_get())) \
				{                            \
					ntp_res = 1;             \
					goto NTP_FAILURE_EXIT;   \
				}                            \
			}                                \
		} while (0)
#	endif

	if (0 != fs_begin()) goto FS_BEGIN_FAILURE_EXIT;
	if (0 != clk_begin()) goto CLK_BEGIN_FAILURE_EXIT;
	if (0 != rtc_begin()) goto RTC_BEGIN_FAILURE_EXIT;
	if (0 != aout_begin()) goto AOUT_BEGIN_FAILURE_EXIT;
	aout_write(aout_value_mid);
	if (0 != pout_begin()) goto POUT_BEGIN_FAILURE_EXIT;
	pout_write(pout_value_min);
	if (0 != dout_begin()) goto DOUT_BEGIN_FAILURE_EXIT;
	dout_write(NTP_VALID_LED, 0);
	for (i = 0 ; i < NUM_TICK_TACK_LED ; i++) dout_write(TICK_TACK_LED(i), 0);
	if (0 != din_begin()) goto DIN_BEGIN_FAILURE_EXIT;

	_scan_arg();
	melody_cycle = 60 * arg_wave_rate / arg_melody_tempo;

	if (0 != itimer_begin(_out_wave, 1000000 / arg_wave_rate)) goto ITIMER_BEGIN_FAILURE_EXIT;
	if (0 != cdp16x2_begin()) goto CDP16X2_BEGIN_FAILURE_EXIT;
	cdp16x2_home();
	cdp16x2_printf("Trying to         update time...");
	is_net_ok = !net_begin();
	set_time_ntp_proc();
	disp_time = clk_get() + arg_timezone;
	_disp(disp_time);
	_tick_tack(disp_time);
	_ntp_valid(_set_time_ntp_result(), disp_time);

	for ( ; ; )
	{
		if (0 != din_read())
		{
			if (!is_sw_on)
			{
				v = arg_voice;
			}
			is_sw_on = 1;
		}
		else
		{
			is_sw_on = 0;
		}

		t = clk_get() + arg_timezone;
		if (disp_time < t)
		{
			if ((t - disp_time) < MIN_DIRECT_ADJ_POS_DIFF_TIME)
			{
				t = disp_time + 1;
			}
		}
		else
		{
			if ((disp_time - t) < MIN_DIRECT_ADJ_NEG_DIFF_TIME)
			{
				t = disp_time;
			}
		}

		if (disp_time != t)
		{
			if ((disp_time / arg_disp_period) != (t / arg_disp_period))
			{
				_disp(t);
			}
			_tick_tack(t);
			_ntp_valid(_set_time_ntp_result(), t);

			disp_time = t;

			tm = clk_gettm(disp_time);
			if (NULL != tm)
			{
				if (_is_in_time(tm, arg_ntp_time))
				{
					set_time_ntp_nb_proc();
				}

				for (i = 0 ; i < MAX_NUM_SIGNAL ; i++)
				{
					if (('\0' != arg_signal[i].voice[0]) && _is_in_time(tm, &(arg_signal[i].tm)))
					{
						if (!is_signal_on[i])
						{
							v = arg_signal[i].voice;
						}
						is_signal_on[i] = 1;
					}
					else
					{
						is_signal_on[i] = 0;
					}
				}

				for (i = 0 ; i < MAX_NUM_CHIME ; i++)
				{
					if (('\0' != arg_chime[i].melody[0]) && _is_in_time(tm, &(arg_chime[i].tm)))
					{
						if (!is_chime_on[i])
						{
							m = arg_chime[i].melody;
						}
						is_chime_on[i] = 1;
					}
					else
					{
						is_chime_on[i] = 0;
					}
				}
			}
		}

		set_time_ntp_result_proc();

		if ((NULL != v) && (_wave_ctx->file_name_queue_tail == _wave_ctx->file_name_queue_head))
		{
			_speak(v, disp_time);
		}
		v = NULL;

		if ((NULL != m) && ((NULL == mp) || ('\0' == *mp)))
		{
			i = _num_len(m);
			mc = (0 < i) ? atoi(m) : 1;
			if (0 < mc)
			{
				mp = ms = m + i;
				melody_count = tick_counter;
				_play(*mp);
			}
		}
		m = NULL;

		if ((NULL != mp) && ('\0' != *mp) && (melody_cycle <= (tick_counter - melody_count)))
		{
			mp++;
			melody_count += melody_cycle;
			if ('\0' == *mp)
			{
				mc--;
				if (0 < mc)
				{
					mp = ms;
				}
			}
			_play(*mp);
		}

		_read_wave();
		_set_time_ntp_yield();
		system_yield();
	}

NTP_FAILURE_EXIT:
	if (is_net_ok) net_end();
	cdp16x2_end();
CDP16X2_BEGIN_FAILURE_EXIT:
	itimer_end();
ITIMER_BEGIN_FAILURE_EXIT:
	din_end();
DIN_BEGIN_FAILURE_EXIT:
	dout_end();
DOUT_BEGIN_FAILURE_EXIT:
	pout_end();
POUT_BEGIN_FAILURE_EXIT:
	aout_end();
AOUT_BEGIN_FAILURE_EXIT:
	rtc_end();
RTC_BEGIN_FAILURE_EXIT:
	clk_end();
CLK_BEGIN_FAILURE_EXIT:
	fs_end();
FS_BEGIN_FAILURE_EXIT:

	system_reset();

	return 0;
}

/* ---------------------------------------------------------------- */

void setup() {
	// put your setup code here, to run once:
#	if defined(DEBUG)
		Serial.begin(115200);
		while (!Serial);
		DEBUG_PRINTF(
			"voice clock for raspberry pi pico w\n"
			"(C) 2026 akino-yo\n"
		);
#	endif
}

void loop() {
	// put your main code here, to run repeatedly:
	_main();
}

#if _IS_ITIMER_ON_CORE_1
	void setup1() {
	}

	void loop1() {
		uint32_t u = rp2040.fifo.pop();
		switch (u)
		{
		case _ITIMER_CMD_ADD_REPEATING_TIMER_US:
			u = add_repeating_timer_us(_itimer_delay_us, _itimer_rt_callback, (void *)_itimer_callback, _itimer_rt) ? 0 : 1;
			break;
		case _ITIMER_CMD_CANCEL_REPEATING_TIMER:
			u = cancel_repeating_timer(_itimer_rt) ? 0 : 1;
			break;
		default:
			break;
		}
		rp2040.fifo.push(u);
	}
#endif
