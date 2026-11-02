// Device-ul IoT din tema 01, de data aceasta pe un ESP32-C3 Super Mini.
//
// Aceeași logică și același protocol ca device-simulator din tema 01, cu trei
// taskuri FreeRTOS în loc de trei thread-uri Java:
//
//   task periodic --(coada de alarme)--> task sporadic
//        |                                     |
//        +---- telemetry ---> MQTT <--- alarm -+
//                              |
//                          commands --> command_received() --(coada de comenzi)--> task aperiodic ---> status
//
// Gata implementate (nu trebuie modificate):
//   config.h         setările: Wi-Fi, broker, STUDENT, valorile inițiale; DEVICE_ID
//                    se citește din adresa MAC a plăcii
//   connection.h/cpp Wi-Fi, ora exactă (SNTP), clientul MQTT, mqtt_publish(), epoch_ms()
//
// Așa cum este, schița se conectează la Wi-Fi și la broker și afișează pe
// Serial comenzile primite. Taskurile le scrieți voi: TODO 0 ... TODO 3.
// Exemplele de pornire sunt în laboratorul 0 din rt-labs-esp32c3; vezi
// exemple/README.md din folderul temei.
//
// Placa: ESP32C3 Dev Module, USB CDC On Boot: Enabled. Serial Monitor: 115200 baud.

#include "config.h"
#include "connection.h"

// Temperatura cipului, în °C, de la senzorul intern al ESP32-C3.
// Este temperatura siliciului, nu a camerei: imediat după pornirea Wi-Fi-ului
// poate fi în jur de 50 °C, apoi scade în câteva minute spre 35-40 °C.
// Rezoluția este de aproximativ 1 °C. Ca să provocați alarme, puneți pragul
// cu SET_THRESHOLD cu un grad sub valoarea citită.
float read_temperature(void) {
    return temperatureRead();
}

// Starea partajata intre taskuri 

// Perioada și pragul
static volatile uint32_t g_period_ms = PERIOD_MS_INITIAL;
static volatile float g_threshold = THRESHOLD_INITIAL;

// Elementul cozii de alarme
struct AlarmEvent {
    float temperature;
    float threshold;
};

// Elementul cozii de comenzi
struct Command {
    char text[COMMAND_MAX_LEN];
};

// Cozile 
static QueueHandle_t g_alarm_queue = NULL;
static QueueHandle_t g_command_queue = NULL;

// TODO 1: TASK PERIODIC, cu perioada T (inițial PERIOD_MS_INITIAL).
//   Fiecare job:
//   - citește temperatura: read_temperature()
//   - publică telemetria: mqtt_publish("telemetry", "deviceId;timestamp;temperatura")
//     (timestamp = epoch_ms(), tipărit cu %lld; temperatura cu "%.2f")
//   - dacă temperatura depășește pragul (inițial THRESHOLD_INITIAL), pune
//     un eveniment în coada de alarme, fără să se blocheze (timeout 0).
//     Taskul periodic NU tratează alarma: o tratează taskul sporadic.
//   Eliberările respectă grila r_k = r_0 + kT: folosiți așteptare absolută
//   (xTaskDelayUntil), nu relativă (delay, vTaskDelay), ca să nu apară derivă.
//   Perioada și pragul se schimbă prin comenzi (taskul aperiodic), deci sunt
//   variabile globale volatile, citite la fiecare job.
static void periodic_task(void* arg) {
    (void)arg;  

    // momentul primei eliberari 
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        
        uint32_t period_ms = g_period_ms;
        float threshold = g_threshold;

        float temperature = read_temperature();

        char message[96];
        snprintf(message, sizeof(message), "%s;%lld;%.2f",
                 DEVICE_ID, (long long)epoch_ms(), temperature);
        mqtt_publish("telemetry", message);

        bool over = temperature > threshold;
        if (over) {
            AlarmEvent ev = {temperature, threshold};
            
            if (xQueueSend(g_alarm_queue, &ev, 0) != pdTRUE) {
                Serial.println("[periodic] coada de alarme plină, eveniment pierdut");
            }
        }

        Serial.printf("[periodic] t=%lu ms  %s%s\n",
                      (unsigned long)millis(), message, over ? "  -> alarmă" : "");

        // așteptare absoluta pana la last_wake + T 
        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(period_ms));
    }
}

// TODO 2: TASK SPORADIC, tratează alarmele de temperatură.
//   - așteaptă un eveniment din coada de alarme
//   - dacă de la ultima alarmă publicată au trecut mai puțin de
//     MIN_INTERARRIVAL_MS, respinge evenimentul (afișați un mesaj pe Serial)
//   - altfel publică: mqtt_publish("alarm", "deviceId;timestamp;temperatura;prag")
//   Pentru intervale folosiți un timp monoton, millis(), nu epoch_ms().
static void sporadic_task(void* arg) {
    (void)arg;

    uint32_t last_release = 0;
    bool first = true;  

    for (;;) {
        AlarmEvent ev;
        // blocheaza pana soseste un eveniment 
        if (xQueueReceive(g_alarm_queue, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        uint32_t now = millis();
        uint32_t since_last = now - last_release;

        if (!first && since_last < MIN_INTERARRIVAL_MS) {
            Serial.printf("   [sporadic] RESPINS: au trecut doar %lu ms (minim %lu)\n",
                          (unsigned long)since_last, (unsigned long)MIN_INTERARRIVAL_MS);
            continue;
        }
        first = false;
        last_release = now;

        char message[96];
        snprintf(message, sizeof(message), "%s;%lld;%.2f;%.2f",
                 DEVICE_ID, (long long)epoch_ms(), ev.temperature, ev.threshold);
        mqtt_publish("alarm", message);
        Serial.printf("   [sporadic] ALARMĂ publicată: %s\n", message);
    }
}

// TODO 3: TASK APERIODIC, execută comenzile primite de la business service.
//   Comenzile ajung în command_received() (mai jos), care le pune în coada de
//   comenzi; fiecare comandă eliberează un job al taskului:
//     SET_INTERVAL <ms>       schimbă perioada T a taskului periodic
//     SET_THRESHOLD <valoare> schimbă pragul de alarmă
//     RESET                   readuce perioada și pragul la valorile inițiale
//     STATUS                  raportează perioada și pragul curente
//   După fiecare comandă publică o confirmare:
//     mqtt_publish("status", "deviceId;timestamp;OK SET_INTERVAL 1000")
//   O comandă necunoscută sau un parametru invalid nu oprește taskul: se
//   publică "ERROR <motiv>".

static void execute_command(const char* command, char* result, size_t size) {
    char buf[COMMAND_MAX_LEN];  
    strncpy(buf, command, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    const char* sep = " \t\r\n";  
    char* save = NULL;
    char* name = strtok_r(buf, sep, &save);
    char* param = strtok_r(NULL, sep, &save);

    if (name == NULL) {
        snprintf(result, size, "ERROR comanda goala");
        return;
    }

    if (strcmp(name, "SET_INTERVAL") == 0) {
        if (param == NULL) {
            snprintf(result, size, "ERROR SET_INTERVAL: lipseste parametrul");
            return;
        }
        char* end = NULL;
        long ms = strtol(param, &end, 10);
        if (end == param || *end != '\0') {  // "abc" sau "100x"
            snprintf(result, size, "ERROR SET_INTERVAL: parametru invalid");
            return;
        }
        if (ms < 100) {
            snprintf(result, size, "ERROR SET_INTERVAL: minim 100 ms");
            return;
        }
        g_period_ms = (uint32_t)ms;
        snprintf(result, size, "OK SET_INTERVAL %ld", ms);

    } else if (strcmp(name, "SET_THRESHOLD") == 0) {
        if (param == NULL) {
            snprintf(result, size, "ERROR SET_THRESHOLD: lipseste parametrul");
            return;
        }
        char* end = NULL;
        float value = strtof(param, &end);
        if (end == param || *end != '\0') {
            snprintf(result, size, "ERROR SET_THRESHOLD: parametru invalid");
            return;
        }
        g_threshold = value;
        snprintf(result, size, "OK SET_THRESHOLD %.2f", value);

    } else if (strcmp(name, "RESET") == 0) {
        g_period_ms = PERIOD_MS_INITIAL;
        g_threshold = THRESHOLD_INITIAL;
        snprintf(result, size, "OK RESET interval=%lu threshold=%.2f",
                 (unsigned long)PERIOD_MS_INITIAL, THRESHOLD_INITIAL);

    } else if (strcmp(name, "STATUS") == 0) {
        snprintf(result, size, "OK STATUS interval=%lu threshold=%.2f",
                 (unsigned long)g_period_ms, (float)g_threshold);

    } else {
        snprintf(result, size, "ERROR comanda necunoscuta: %s", name);
    }
}

static void aperiodic_task(void* arg) {
    (void)arg;

    for (;;) {
        Command cmd;
        // fiecare comanda din coada elibereaza un job 
        if (xQueueReceive(g_command_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        char result[COMMAND_MAX_LEN + 48];
        execute_command(cmd.text, result, sizeof(result));

        char message[sizeof(result) + 48];
        snprintf(message, sizeof(message), "%s;%lld;%s",
                 DEVICE_ID, (long long)epoch_ms(), result);
        mqtt_publish("status", message);
        Serial.printf("      [aperiodic] %s -> %s\n", cmd.text, result);
    }
}

// Apelată de clientul MQTT pentru fiecare comandă primită (vezi connection.h).
void command_received(const char* text) {
    Serial.printf("[mqtt] comandă primită: %s\n", text);
    // TODO 3: puneți comanda în coada de comenzi, fără să vă blocați
    //   (timeout 0). text este valid doar în timpul acestui apel: elementul
    //   cozii trebuie să conțină textul însuși (de exemplu un struct cu
    //   char text[COMMAND_MAX_LEN]), nu un pointer.
    Command cmd;
    strncpy(cmd.text, text, sizeof(cmd.text) - 1);
    cmd.text[sizeof(cmd.text) - 1] = '\0';

    if (xQueueSend(g_command_queue, &cmd, 0) != pdTRUE) {
        Serial.println("[mqtt] coada de comenzi plină, comandă ignorată");
    }
}

void setup() {
    Serial.begin(115200);
    // Fără așteptare la scriere: cu placa la USB, dar fără Serial Monitor deschis,
    // fiecare Serial.printf() ar aștepta până la 2 s să fie citit și ar întârzia
    // taskul care scrie. Așa, textul pe care nu-l citește nimeni se pierde.
    Serial.setTxTimeoutMs(0);
    uint32_t t = millis();
    while (!Serial && millis() - t < 3000) delay(10);  // așteptăm Serial Monitor, cel mult 3 s
    delay(200);
    Serial.println();
    Serial.printf("=== tema 02: STUDENT %s, DEVICE_ID %s ===\n", STUDENT, DEVICE_ID);

    wifi_connect();
    time_sync();

    // TODO 0 (a): creați cele două cozi (xQueueCreate): coada de alarme și
    //   coada de comenzi. Înainte de mqtt_start(): de acolo încolo pot sosi
    //   comenzi, iar command_received() le pune în coada de comenzi.
    g_alarm_queue = xQueueCreate(8, sizeof(AlarmEvent));
    g_command_queue = xQueueCreate(8, sizeof(Command));

    mqtt_start();

    // TODO 0 (b): porniți cele trei taskuri (xTaskCreate). După mqtt_start(),
    //   ca primele mesaje publicate să nu se piardă. Alegeți prioritățile
    //   între 2 și 19: peste loopTask (1), sub taskurile sistemului pentru
    //   evenimente și Wi-Fi (20, 23). Stiva, în octeți: cel puțin 4096.
    xTaskCreate(periodic_task, "periodic", 4096, NULL, 4, NULL);
    xTaskCreate(sporadic_task, "sporadic", 4096, NULL, 3, NULL);
    xTaskCreate(aperiodic_task, "aperiodic", 4096, NULL, 2, NULL);
}

void loop() {
    // Totul se face în taskuri: loop() nu are nimic de făcut.
    delay(1000);
}
