#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/adc.h"
#include "hardware/pwm.h" // Para controle de LED com PWM
#include "hardware/gpio.h" // Para GPIOs genéricos

#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"

// Includes do FreeRTOS
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h" // Para semáforos

// Credenciais WIFI - Tome cuidado se publicar no github!
#define WIFI_SSID ""
#define WIFI_PASSWORD ""

// Definição dos pinos
#define ONBOARD_LED_PIN CYW43_WL_GPIO_LED_PIN // LED onboard da Pico W
#define LED_GREEN_PIN 11  // GPIO11 - LED verde (usado para laranja com PWM)
#define LED_RED_PIN 13    // GPIO13 - LED vermelho (usado para laranja e vermelho com PWM)
#define BUTTON_A_PIN 5    // GPIO5 - Botão A para simulação

// Constantes para simulação e alertas
#define TEMPERATURA_NORMAL 25.0f
#define TEMPERATURA_ALTA 38.5f
#define TEMPERATURA_SIRS 38.0f

#define BPM_NORMAL 70.0f
#define BPM_ALTO 120.0f
#define BPM_SIRS 90.0f

#define PWM_WRAP_VALUE 255 // Para controle PWM de 8 bits

// Variáveis globais para dados dos sensores (simulados) e estado
volatile float temp_sim = TEMPERATURA_NORMAL;
volatile float bpm_sim = BPM_NORMAL;
volatile int estado = 0; // 0: Normal, 1: HR Alto, 2: Temp Alta, 3: Ambos Altos

// Handle para o Mutex
SemaphoreHandle_t sensor_data_mutex;

// Handles das Tasks
TaskHandle_t heart_sensor_task_handle;
TaskHandle_t temperature_sensor_task_handle;
TaskHandle_t button_task_handle;
TaskHandle_t led_alert_task_handle;
TaskHandle_t network_poll_task_handle;
TaskHandle_t tcp_server_task_handle; // Servidor TCP for executado como uma tarefa

// --- Protótipos das Funções ---
void init_gpios_pwm(void);

// Callbacks LwIP
static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
void launch_web_server(void);

// Tasks do FreeRTOS
void heart_sensor_task(void *pvParameters);
void temperature_sensor_task(void *pvParameters);
void button_task(void *pvParameters);
void led_alert_task(void *pvParameters);
void network_poll_task(void *pvParameters);

// --- Inicializações ---
void init_gpios_pwm(void) {
    // Botão A
    gpio_init(BUTTON_A_PIN);
    gpio_set_dir(BUTTON_A_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_A_PIN); // Usar pull-up interno, botão conecta ao GND

    // LEDs para PWM
    uint led_pins[] = {LED_RED_PIN, LED_GREEN_PIN};
    for (int i = 0; i < sizeof(led_pins) / sizeof(led_pins[0]); ++i) {
        uint gpio = led_pins[i];
        gpio_set_function(gpio, GPIO_FUNC_PWM);
        uint slice_num = pwm_gpio_to_slice_num(gpio);
        
        pwm_config config = pwm_get_default_config();
        pwm_config_set_wrap(&config, PWM_WRAP_VALUE); // Define o ciclo máximo do PWM
        pwm_init(slice_num, &config, true); // Inicia o PWM, true para habilitar
        pwm_set_gpio_level(gpio, 0); // Começa com LED desligado
    }
}

// --- Tasks do FreeRTOS ---

// Tarefa para simular leitura do sensor cardíaco (placeholder)
void heart_sensor_task(void *pvParameters) {
    (void)pvParameters; // Evitar aviso de não utilizado
    while (true) {
        // Lógica futura do sensor cardíaco aqui
        vTaskDelay(pdMS_TO_TICKS(1000)); // Executa a cada 1 segundo
    }
}

// Tarefa para simular leitura do sensor de temperatura (placeholder)
void temperature_sensor_task(void *pvParameters) {
    (void)pvParameters; // Evitar aviso de não utilizado
    while (true) {
        // Lógica futura do sensor de temperatura aqui
        vTaskDelay(pdMS_TO_TICKS(1000)); // Executa a cada 1 segundo
    }
}

// Tarefa para monitorar o botão e alterar os valores simulados
void button_task(void *pvParameters) {
    (void)pvParameters;
    bool last_button_state = true; // Assume que o botão está solto (pull-up)
    TickType_t last_press_time = 0;

    while (true) {
        bool current_button_state = gpio_get(BUTTON_A_PIN);
        if (current_button_state == false && last_button_state == true) { // Botão pressionado (borda de descida)
            if (xTaskGetTickCount() - last_press_time > pdMS_TO_TICKS(250)) { // Debounce simples
                last_press_time = xTaskGetTickCount();

                if (xSemaphoreTake(sensor_data_mutex, portMAX_DELAY) == pdTRUE) {
                    estado = (estado + 1) % 4; // Cicla entre 0, 1, 2, 3

                    switch (estado) {
                        case 0: // Normal
                            temp_sim = TEMPERATURA_NORMAL;
                            bpm_sim = BPM_NORMAL;
                            printf("Simulação: Normal\n");
                            break;
                        case 1: // Cardíaco Alto
                            temp_sim = TEMPERATURA_NORMAL;
                            bpm_sim = BPM_ALTO;
                            printf("Simulação: Cardíaco Alto\n");
                            break;
                        case 2: // Temperatura Alta
                            temp_sim = TEMPERATURA_ALTA;
                            bpm_sim = BPM_NORMAL;
                            printf("Simulação: Temperatura Alta\n");
                            break;
                        case 3: // Ambos Altos
                            temp_sim = TEMPERATURA_ALTA;
                            bpm_sim = BPM_ALTO;
                            printf("Simulação: Ambos Altos (Crítico)\n");
                            break;
                    }
                    xSemaphoreGive(sensor_data_mutex);
                }
            }
        }
        last_button_state = current_button_state;
        vTaskDelay(pdMS_TO_TICKS(50)); // Verifica o botão a cada 50ms
    }
}

// Tarefa para controlar LEDs com base nos alertas
void led_alert_task(void *pvParameters) {
    (void)pvParameters;
    float temp, hr;
    bool temp_alerta, bpm_alerta;

    while (true) {
        if (xSemaphoreTake(sensor_data_mutex, portMAX_DELAY) == pdTRUE) {
            temp = temp_sim;
            hr = bpm_sim;
            xSemaphoreGive(sensor_data_mutex);
        }

        temp_alerta = (temp > TEMPERATURA_SIRS);
        bpm_alerta = (hr > BPM_SIRS);

        if (temp_alerta && bpm_alerta) { // Ambos altos = Vermelho 
            pwm_set_gpio_level(LED_RED_PIN, PWM_WRAP_VALUE); // Vermelho máximo
            pwm_set_gpio_level(LED_GREEN_PIN, 0);            // Verde desligado
            cyw43_arch_gpio_put(ONBOARD_LED_PIN, 1); // LED onboard piscando ou aceso para alerta
        } else if (temp_alerta || bpm_alerta) { // Um dos dois alto = Laranja
            pwm_set_gpio_level(LED_RED_PIN, PWM_WRAP_VALUE); // Vermelho máximo
            pwm_set_gpio_level(LED_GREEN_PIN, PWM_WRAP_VALUE / 2); // Verde médio (para fazer laranja)
            cyw43_arch_gpio_put(ONBOARD_LED_PIN, 1);
        } else { // Normal
            pwm_set_gpio_level(LED_RED_PIN, 0);              // Vermelho desligado
            pwm_set_gpio_level(LED_GREEN_PIN, 0); // Verde desligado (ou pode ser um verde fixo se quiser)
            cyw43_arch_gpio_put(ONBOARD_LED_PIN, 0); // LED onboard apagado ou em estado normal
        }
        vTaskDelay(pdMS_TO_TICKS(200)); // Atualiza o estado dos LEDs
    }
}

// Tarefa para pollar a stack Wi-Fi
void network_poll_task(void *pvParameters) {
    (void)pvParameters;
    while (true) {
        cyw43_arch_poll();
        vTaskDelay(pdMS_TO_TICKS(10)); // Poll da rede com frequência
    }
}

// --- LwIP Callbacks e Configuração do Servidor ---
static err_t tcp_server_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    LWIP_UNUSED_ARG(arg);
    if (err != ERR_OK || newpcb == NULL) {
        printf("Falha ao aceitar conexão TCP: %d\n", err);
        return ERR_VAL;
    }
    printf("Nova conexão TCP aceita.\n");
    tcp_recv(newpcb, tcp_server_recv); // Define o callback para dados recebidos
    return ERR_OK;
}

static err_t tcp_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    LWIP_UNUSED_ARG(arg);

    if (err != ERR_OK && err != ERR_ABRT) { // ERR_ABRT pode acontecer se o cliente abortar
        printf("Erro na recepção TCP: %d\n", err);
        if (p) pbuf_free(p); // Libera o pbuf se houver erro e pbuf existir
        // Não fechar o tpcb aqui necessariamente, LwIP pode cuidar disso ou tentar recuperar
        return err;
    }
    
    if (!p) { // Conexão fechada pelo cliente
        printf("Conexão TCP fechada pelo cliente.\n");
        tcp_close(tpcb);
        tcp_recv(tpcb, NULL); // Remove o callback de recepção
        return ERR_OK;
    }

    // Checa se é uma requisição GET (simplificado)
    if (p->len >= 3 && strncmp((char*)p->payload, "GET", 3) == 0) {
        char html_buffer[1024]; // Buffer para a resposta HTML
        float temp_atual, bpm_atual;
        int estado_atual;
        char status_text[50];

        // Pega os dados dos sensores de forma segura
        if (xSemaphoreTake(sensor_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            temp_atual = temp_sim;
            bpm_atual = bpm_sim;
            estado_atual = estado; // Usar a cópia local do estado
            xSemaphoreGive(sensor_data_mutex);
        } 

        // Define o texto de status com base no estado da simulação
        bool temp_alerta = (temp_atual > TEMPERATURA_SIRS);
        bool bpm_alerta = (bpm_atual > BPM_SIRS);

        if (temp_alerta && bpm_alerta) {
            strcpy(status_text, "CRiTICO!");
        } else if (bpm_alerta) {
            strcpy(status_text, "Alerta Cardiaco");
        } else if (temp_alerta) {
            strcpy(status_text, "Alerta Temperatura");
        } else if (estado_atual != -1) {
            strcpy(status_text, "Normal");
        }

        // Monta a resposta HTML
        // Estilo CSS inline e compacto para economizar espaço
        // Cores: Fundo escuro (#282c34), Texto claro (#abb2bf), Título (#61afef), Alertas (laranja, vermelho)
        snprintf(html_buffer, sizeof(html_buffer),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Connection: close\r\n\r\n" // Adicionar Connection: close
            "<!DOCTYPE html>"
            "<html><head><title>SIRS Monitor</title>"
            "<meta http-equiv='refresh' content='5'>" // Auto-refresh a cada 5 segundos
            "<style>"
            "body{font-family:Arial,sans-serif;background-color:#282c34;color:#abb2bf;text-align:center;margin:0;padding:20px;}"
            "h1{color:#61afef;font-size:2em;margin-bottom:10px;}"
            ".container{background-color:#353a40;padding:15px;border-radius:8px;box-shadow:0 4px 8px rgba(0,0,0,0.2);display:inline-block;}"
            "p{font-size:1.2em;margin:8px 0;}"
            ".status{font-weight:bold;}"
            ".normal{color:#98c379;}" /* Verde */
            ".warning{color:#e5c07b;}" /* Amarelo/Laranja */
            ".critical{color:#e06c75;}" /* Vermelho */
            "</style></head>"
            "<body><div class='container'>"
            "<h1>Monitoramento SIRS</h1>"
            "<p>Temperatura: %.1f &deg;C</p>"
            "<p>Batimentos: %.0f bpm</p>",
            temp_atual, bpm_atual);

        // Adiciona o status dinamicamente para permitir classes de cor
        char status_html[100];
        if (temp_alerta && bpm_alerta) {
            snprintf(status_html, sizeof(status_html), "<p class='status critical'>Status: %s</p>", status_text);
        } else if (temp_alerta || bpm_alerta) {
            snprintf(status_html, sizeof(status_html), "<p class='status warning'>Status: %s</p>", status_text);
        } else if (estado_atual != -1) {
            snprintf(status_html, sizeof(status_html), "<p class='status normal'>Status: %s</p>", status_text);
        } else {
             snprintf(status_html, sizeof(status_html), "<p class='status critical'>Status: %s</p>", status_text);
        }
        strncat(html_buffer, status_html, sizeof(html_buffer) - strlen(html_buffer) - 1);
        strncat(html_buffer, "</div></body></html>", sizeof(html_buffer) - strlen(html_buffer) - 1);
        
        printf("HTML enviado:\n%s\n", html_buffer); // Depuração do tamanho do buffer

        // Envia a resposta
        err_t write_err = tcp_write(tpcb, html_buffer, strlen(html_buffer), TCP_WRITE_FLAG_COPY);
        if (write_err != ERR_OK) {
            printf("Erro ao escrever para TCP: %d\n", write_err);
            // Se houver erro na escrita, pode ser necessário fechar o pcb
            tcp_close(tpcb); // Fechar em caso de erro grave de escrita
            tcp_recv(tpcb, NULL);
        } else {
            tcp_output(tpcb); // Força o envio dos dados
        }
    }

    pbuf_free(p); // Libera o buffer da requisição

    return ERR_OK;
}

void launch_web_server(void) {
    struct tcp_pcb *server_pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!server_pcb) {
        printf("Falha ao criar PCB do servidor TCP\n");
        return;
    }

    err_t err = tcp_bind(server_pcb, IP_ADDR_ANY, 80);
    if (err != ERR_OK) {
        printf("Falha ao associar servidor TCP à porta 80: %d\n", err);
        tcp_close(server_pcb); // Libera o PCB se o bind falhar
        return;
    }

    server_pcb = tcp_listen_with_backlog(server_pcb, 1); // backlog de 1 é suficiente para este exemplo
    if (!server_pcb) {
        printf("Falha ao colocar servidor TCP em modo de escuta\n");
        // O PCB original pode ter sido liberado por tcp_listen em caso de erro.
        // Se tcp_listen falhar e retornar NULL, o pcb antigo já foi desalocado.
    } else {
        tcp_accept(server_pcb, tcp_server_accept);
        printf("Servidor TCP ouvindo na porta 80\n");
    }
}


// --- Função Principal ---
int main() {
    stdio_init_all();

    // Inicializa GPIOs e PWM
    init_gpios_pwm();

    // Inicializa a arquitetura cyw43 (Wi-Fi)
    if (cyw43_arch_init()) {
        printf("Falha ao inicializar Wi-Fi\n");
        return -1;
    }
    cyw43_arch_enable_sta_mode();
    cyw43_arch_gpio_put(ONBOARD_LED_PIN, 0); // LED onboard desligado inicialmente

    // Conecta ao Wi-Fi
    printf("Conectando ao Wi-Fi: %s...\n", WIFI_SSID);
    int retries = 0;
    while (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000)) {
        printf("Falha ao conectar ao Wi-Fi. Tentativa %d\n", ++retries);
        if (retries > 5) {
            printf("Muitas falhas ao conectar. Desistindo.\n");
            cyw43_arch_deinit();
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(1000)); // Espera antes de tentar novamente
    }
    printf("Conectado ao Wi-Fi!\n");
    printf("IP do dispositivo: %s\n", ipaddr_ntoa(netif_ip_addr4(netif_default)));
    cyw43_arch_gpio_put(ONBOARD_LED_PIN, 1); // Acende LED onboard para indicar conexão Wi-Fi

    // Cria o Mutex para os dados dos sensores
    sensor_data_mutex = xSemaphoreCreateMutex();
    if (sensor_data_mutex == NULL) {
        printf("Falha ao criar mutex!\n");
        return -1;
    }

    // Lança o servidor web (configura LwIP, não bloqueia)
    launch_web_server();

    // Cria as tasks do FreeRTOS
    xTaskCreate(heart_sensor_task, "HeartSensorTask", configMINIMAL_STACK_SIZE * 2, NULL, tskIDLE_PRIORITY + 1, &heart_sensor_task_handle);

    xTaskCreate(temperature_sensor_task, "TempSensorTask", configMINIMAL_STACK_SIZE * 2, NULL, tskIDLE_PRIORITY + 1, &temperature_sensor_task_handle);
    
    xTaskCreate(button_task, "ButtonTask", configMINIMAL_STACK_SIZE * 2, NULL, tskIDLE_PRIORITY + 2, &button_task_handle);

    xTaskCreate(led_alert_task, "LedAlertTask", configMINIMAL_STACK_SIZE * 2, NULL, tskIDLE_PRIORITY + 2, &led_alert_task_handle);

    xTaskCreate(network_poll_task, "NetworkPollTask", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 3, &network_poll_task_handle); // Prioridade mais alta para rede

    printf("Iniciando scheduler do FreeRTOS...\n");
    vTaskStartScheduler();

    return 0;
}