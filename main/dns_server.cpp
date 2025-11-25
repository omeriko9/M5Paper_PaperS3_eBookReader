#include "dns_server.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include <string.h>

static const char *TAG = "DNS";
static int sock_fd = -1;
static bool running = false;

void dns_server_task(void *pvParameters) {
    char rx_buffer[128];
    char tx_buffer[128];
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(53);

    sock_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock_fd < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int err = bind(sock_fd, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(sock_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS Server started");

    struct sockaddr_in source_addr;
    socklen_t socklen = sizeof(source_addr);

    while (running) {
        int len = recvfrom(sock_fd, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&source_addr, &socklen);

        if (len < 0) {
            ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
            break;
        }

        if (len > 12) {
            // Prepare response
            memcpy(tx_buffer, rx_buffer, len); // Copy ID and questions
            
            // Flags: QR=1 (Response), AA=1 (Authoritative), RD=0, RA=0, RCODE=0
            tx_buffer[2] = 0x84; // Response, Authoritative
            tx_buffer[3] = 0x00; // No recursion

            // Answers count = 1
            tx_buffer[6] = 0x00;
            tx_buffer[7] = 0x01;
            
            // Authority count = 0
            tx_buffer[8] = 0x00;
            tx_buffer[9] = 0x00;
            
            // Additional count = 0
            tx_buffer[10] = 0x00;
            tx_buffer[11] = 0x00;

            // Pointer to the question name (0xC00C)
            // The answer starts after the question. 
            // We need to find the end of the question section.
            int q_len = 0;
            char *q_ptr = rx_buffer + 12;
            while (*q_ptr != 0 && q_ptr < rx_buffer + len) {
                q_len++;
                q_ptr++;
            }
            q_len++; // Null terminator
            q_len += 4; // QTYPE and QCLASS

            int answer_start = 12 + q_len;
            
            // Check buffer safety
            if (answer_start + 16 > sizeof(tx_buffer)) continue;

            // Answer Name: Pointer to the name in the question section (offset 12)
            tx_buffer[answer_start] = 0xC0;
            tx_buffer[answer_start + 1] = 0x0C;

            // Type: A (Host Address) = 1
            tx_buffer[answer_start + 2] = 0x00;
            tx_buffer[answer_start + 3] = 0x01;

            // Class: IN = 1
            tx_buffer[answer_start + 4] = 0x00;
            tx_buffer[answer_start + 5] = 0x01;

            // TTL: 60 seconds
            tx_buffer[answer_start + 6] = 0x00;
            tx_buffer[answer_start + 7] = 0x00;
            tx_buffer[answer_start + 8] = 0x00;
            tx_buffer[answer_start + 9] = 0x3C;

            // Data Length: 4 bytes
            tx_buffer[answer_start + 10] = 0x00;
            tx_buffer[answer_start + 11] = 0x04;

            // IP Address: 192.168.4.1 (C0 A8 04 01)
            tx_buffer[answer_start + 12] = 192;
            tx_buffer[answer_start + 13] = 168;
            tx_buffer[answer_start + 14] = 4;
            tx_buffer[answer_start + 15] = 1;

            int response_len = answer_start + 16;

            sendto(sock_fd, tx_buffer, response_len, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
        }
    }

    if (sock_fd != -1) {
        close(sock_fd);
        sock_fd = -1;
    }
    vTaskDelete(NULL);
}

void DnsServer::start() {
    if (running) return;
    running = true;
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, NULL);
}

void DnsServer::stop() {
    running = false;
    if (sock_fd != -1) {
        close(sock_fd); // This will unblock recvfrom
    }
}
