#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/adc.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "wifi_config.h"

// Calibration: sensor outputs HIGH raw values when dry, LOW when wet.
// Set MOISTURE_DRY to the stable raw reading in dry air.
// Set MOISTURE_WET to the stable raw reading fully submerged in water.
#define MOISTURE_DRY 2400
#define MOISTURE_WET 1765
#define MOISTURE_THRESHOLD 35.0f

#define LED_PIN 15

float current_moisture_pct = 0.0f;
uint16_t current_raw_adc = 0;
uint32_t current_free_memory = 0;

static int wifi_connect_with_blink(const char *ssid, const char *pw, uint32_t auth, uint32_t timeout_ms) {
    int err = cyw43_arch_wifi_connect_async(ssid, pw, auth);
    if (err) return err;

    absolute_time_t timeout = make_timeout_time_ms(timeout_ms);
    absolute_time_t next_toggle = make_timeout_time_ms(1000);
    bool led_on = true;
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_on);

    while (true) {
        if (time_reached(timeout)) return PICO_ERROR_TIMEOUT;

        cyw43_arch_poll();
        sleep_ms(10);

        int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (status == CYW43_LINK_UP) return 0;
        if (status < 0) return status;

        if (time_reached(next_toggle)) {
            led_on = !led_on;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_on);
            next_toggle = delayed_by_ms(next_toggle, 1000);
        }
    }
}

static void blink_error_forever() {
    while (true) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        sleep_ms(250);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        sleep_ms(250);
    }
}

uint32_t get_free_memory() {
    extern char __StackLimit;
    register uint32_t stack_pointer __asm("sp");
    return stack_pointer - (uint32_t)&__StackLimit;
}

void read_moisture() {
    adc_select_input(1);
    uint16_t raw = adc_read();
    current_raw_adc = raw;
    printf("Moisture raw: %u\n", raw);

    float pct = (float)(MOISTURE_DRY - raw) / (float)(MOISTURE_DRY - MOISTURE_WET) * 100.0f;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;

    current_moisture_pct = pct;
    current_free_memory = get_free_memory();

    gpio_put(LED_PIN, pct < MOISTURE_THRESHOLD);
}

static err_t receive_callback(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    if (p == NULL) {
        tcp_close(tpcb);
        return ERR_OK;
    }

    tcp_recved(tpcb, p->tot_len);

    char response[1024];
    int len;

    if (strstr(p->payload, "GET /data") != NULL) {
        len = snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n"
            "%.1f,%u,%u",
            current_moisture_pct, current_raw_adc, current_free_memory);
    } else {
        len = snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
            "<html><body>"
            "<h1>Pico W Soil Moisture</h1>"
            "<p>Moisture: <span id='m'>%.1f</span>%%</p>"
            "<p>Raw ADC: <span id='r'>%u</span></p>"
            "<p>Free memory: <span id='h'>%u</span> bytes</p>"
            "<script>"
            "function update(v,r,h){"
            "var s=document.getElementById('m');"
            "s.textContent=v;"
            "s.style.color=parseFloat(v)<35?'red':parseFloat(v)<50?'orange':'green';"
            "document.getElementById('r').textContent=r;"
            "document.getElementById('h').textContent=h;"
            "}"
            "setInterval(function(){"
            "fetch('/data').then(r=>r.text()).then(d=>{"
            "var p=d.split(',');update(p[0],p[1],p[2]);"
            "});"
            "},2000);"
            "update('%.1f','%u','%u');"
            "</script>"
            "</body></html>",
            current_moisture_pct, current_raw_adc, current_free_memory,
            current_moisture_pct, current_raw_adc, current_free_memory);
    }

    tcp_write(tpcb, response, len, TCP_WRITE_FLAG_COPY);
    tcp_output(tpcb);

    pbuf_free(p);
    tcp_close(tpcb);

    return ERR_OK;
}

static void error_callback(void *arg, err_t err) {
    printf("TCP error: %d\n", err);
}

static err_t connection_callback(void *arg, struct tcp_pcb *newpcb, err_t err) {
    tcp_err(newpcb, error_callback);
    tcp_recv(newpcb, receive_callback);
    return ERR_OK;
}

bool start_http_server() {
    struct tcp_pcb *pcb = tcp_new();
    if (pcb == NULL) return false;
    tcp_bind(pcb, IP_ADDR_ANY, 80);
    pcb = tcp_listen(pcb);
    if (pcb == NULL) return false;
    tcp_accept(pcb, connection_callback);
    printf("HTTP server started on port 80\n");
    return true;
}

int main()
{
    stdio_init_all();
    sleep_ms(3000);

    if (cyw43_arch_init_with_country(CYW43_COUNTRY_USA) != 0) {
        printf("Wi-Fi init failed\n");
        return -1;
    }

    cyw43_arch_enable_sta_mode();
    netif_set_hostname(netif_list, "pico-moisture");

    printf("Attempting to connect to Wi-Fi...\n");

    int connection_result = wifi_connect_with_blink(WIFI_SSID_1, WIFI_PASS_1, CYW43_AUTH_WPA2_AES_PSK, 15000);
    if (connection_result != 0) {
        printf("Office failed, trying home...\n");
        connection_result = wifi_connect_with_blink(WIFI_SSID_2, WIFI_PASS_2, CYW43_AUTH_WPA2_AES_PSK, 15000);
    }
    if (connection_result != 0) {
        printf("Wi-Fi connection failed: %d\n", connection_result);
        blink_error_forever();
    }

    printf("Connected!\n");
    printf("IP Address: %s\n", ip4addr_ntoa(netif_ip4_addr(netif_list)));

    if (!start_http_server()) {
        printf("HTTP server failed to start\n");
        blink_error_forever();
    }

    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);

    // GPIO27 = ADC1 (pin 32 on Pico W) — GPIO26 is used by the CYW43 WiFi driver
    adc_init();
    adc_gpio_init(27);
    adc_select_input(1);

    sleep_ms(3000);

    int loop_count = 0;
    while (true) {
        cyw43_arch_poll();
        if (loop_count >= 10) {
            read_moisture();
            loop_count = 0;
        }
        loop_count++;
        sleep_ms(100);
    }
}
