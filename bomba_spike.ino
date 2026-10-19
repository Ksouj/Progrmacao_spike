#include <WiFi.h>
#include <FirebaseESP32.h>
#include <sys/time.h>
#include <time.h>

// ==========================================
// DADOS DE CONEXÃO (Mude para o seu Wi-Fi)
// ==========================================
#define WIFI_SSID "NOME_DO_SEU_WIFI"
#define WIFI_PASSWORD "SUA_SENHA_WIFI"

// Mesmo banco do index.html / Anndrey.html
#define FIREBASE_HOST "spike-3301-default-rtdb.firebaseio.com"
#define FIREBASE_AUTH "SUA_API_KEY_OU_DATABASE_SECRET" // não usado (test_mode), mantido por compatibilidade

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ==========================================
// PINOS (ESP32)
// ==========================================
const int pinSmoke  = 23;  // Relé (fumaça / explosão)
const int pinBuzzer = 22;  // Buzzer
const int pinLedR   = 19;  // LED vermelho
const int pinLedG   = 18;  // LED verde
const int pinLedB   = 5;   // LED azul

// ==========================================
// CONFIGURAÇÕES DOS EFEITOS
// ==========================================
const unsigned long EXPLOSION_MS = 5000;  // tempo da fumaça/alarme após explodir
const unsigned long DISARM_FB_MS = 2000;  // LED ciano quando desarmada

// --- Som do Spike (Valorant), medido no áudio de referência ---
const int      BEEP_FREQ         = 4000;  // frequência do bip (Hz)
const uint32_t BEEP_ON_MS        = 125;   // duração de cada bip
// O ritmo muda em degraus conforme a fração do tempo já decorrida:
//   0%..52%  -> 1 bip por segundo
//   52%..77% -> 1 bip a cada 500 ms
//   77%..87% -> 1 bip a cada 250 ms
//   87%..100%-> tom contínuo até explodir
const float    STEP1_END         = 0.525f;
const float    STEP2_END         = 0.775f;
const float    STEP3_END         = 0.873f;
// Explosão: "tuuum" descendente (sweep) em vez de tom fixo
const int      BOOM_START_HZ     = 1200;
const int      BOOM_END_HZ       = 60;
const uint32_t BOOM_SWEEP_MS     = 1500;

// ==========================================
// ESTADO RECEBIDO DO FIREBASE (preenchido pela tarefa de rede)
// ==========================================
enum RemoteStatus { RS_IDLE = 0, RS_ARMED = 1, RS_DETONATED = 2 };

struct RemoteState {
  int      status;
  double   armId;    // identifica cada "armar" feito no admin
  double   fimEm;    // horário final da bomba (epoch em ms)
  uint32_t totalMs;  // tempo total configurado (ms)
};

RemoteState remote = { RS_IDLE, 0, 0, 30000 };
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool wantWriteDetonada = false;

// ==========================================
// ESTADO LOCAL DA BOMBA FÍSICA
// ==========================================
enum Phase { PH_IDLE, PH_ARMED, PH_EXPLODING, PH_DISARMED };
Phase phase = PH_IDLE;

double   curArmId = 0;
int64_t  endMs = 0;
uint32_t totalMs = 30000;
unsigned long phaseStart = 0;

bool beepOn = false;
unsigned long nextBeepAt = 0;
unsigned long beepOffAt = 0;

// ==========================================
// FUNÇÕES AUXILIARES
// ==========================================
void setLed(int r, int g, int b) {
  analogWrite(pinLedR, r);
  analogWrite(pinLedG, g);
  analogWrite(pinLedB, b);
}

void allOff() {
  setLed(0, 0, 0);
  noTone(pinBuzzer);
  digitalWrite(pinSmoke, LOW);
  beepOn = false;
}

// Hora real em ms (NTP). O site usa o relógio do servidor do Firebase, que é o mesmo horário.
int64_t epochMs() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

// ratio: 1.0 = tempo cheio (verde) ... 0.0 = acabando (vermelho). Passa pelo amarelo.
void ledGradient(float ratio) {
  int r, g;
  if (ratio > 0.5f) {
    r = (int)((1.0f - ratio) * 2.0f * 255.0f);
    g = 255;
  } else {
    r = 255;
    g = (int)(ratio * 2.0f * 255.0f);
  }
  setLed(r, g, 0);
}

// ==========================================
// LEITURA DO FIREBASE (roda em outra tarefa, não trava os bips/LEDs)
// ==========================================
bool fbGetRaw(const char *path, String &out) {
  if (!Firebase.get(fbdo, path)) return false;
  out = fbdo.payload();
  out.trim();
  out.replace("\"", "");
  return out.length() > 0 && out != "null";
}

void firebaseTask(void *pv) {
  double   lastArmId = 0;
  uint32_t lastTotal = 30000;

  for (;;) {
    if (Firebase.ready()) {
      String s;
      if (fbGetRaw("/bomba/status", s)) {
        RemoteState n;
        portENTER_CRITICAL(&mux);
        n = remote;
        portEXIT_CRITICAL(&mux);

        bool ok = true;

        if (s == "ARMADA") {
          n.status = RS_ARMED;
          String a, f;
          if (fbGetRaw("/bomba/armId", a) && fbGetRaw("/bomba/fimEm", f)) {
            n.armId = atof(a.c_str());
            n.fimEm = atof(f.c_str());
            if (n.armId != lastArmId) {
              String t;
              if (fbGetRaw("/bomba/tempoTotal", t)) {
                lastTotal = (uint32_t)(atof(t.c_str()) * 1000.0);
                lastArmId = n.armId;
              } else {
                ok = false;
              }
            }
            n.totalMs = lastTotal;
          } else {
            ok = false;
          }
        } else if (s == "DETONADA") {
          n.status = RS_DETONATED;
        } else {
          n.status = RS_IDLE;  // DESARMADA (inclui reset)
        }

        if (ok) {
          portENTER_CRITICAL(&mux);
          remote = n;
          portEXIT_CRITICAL(&mux);
        }

        // Se a bomba física explodiu pelo próprio tempo, avisa o Firebase
        if (wantWriteDetonada) {
          if (s == "ARMADA") Firebase.setString(fbdo, "/bomba/status", "DETONADA");
          wantWriteDetonada = false;
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(150));
  }
}

// ==========================================
// TRANSIÇÕES
// ==========================================
void startArmed(const RemoteState &r) {
  allOff();
  phase = PH_ARMED;
  totalMs = r.totalMs > 0 ? r.totalMs : 30000;
  endMs = (int64_t)r.fimEm;
  nextBeepAt = 0;
  Serial.println("Bomba ARMADA via site!");
}

void startDisarmed() {
  allOff();
  phase = PH_DISARMED;
  phaseStart = millis();
  setLed(0, 255, 255);  // ciano = desarmada
  Serial.println("Bomba DESARMADA!");
}

void startExplosion(bool ownTimer) {
  allOff();
  phase = PH_EXPLODING;
  phaseStart = millis();
  setLed(255, 0, 0);
  tone(pinBuzzer, BOOM_START_HZ);
  digitalWrite(pinSmoke, HIGH);
  if (ownTimer) wantWriteDetonada = true;
  Serial.println("TEMPO ESGOTADO! BOOOOOOOOOOOM!");
}

// Bips e LED no estilo Spike: ritmo em degraus (1s -> 500ms -> 250ms -> contínuo).
// A cor vai de verde -> amarelo -> vermelho conforme o tempo acaba.
void armedEffects(int64_t remainingMs) {
  float ratio = (float)remainingMs / (float)totalMs;
  if (ratio < 0.0f) ratio = 0.0f;
  if (ratio > 1.0f) ratio = 1.0f;
  float elapsed = 1.0f - ratio;

  unsigned long now = millis();

  // Fase final: tom contínuo
  if (elapsed >= STEP3_END) {
    if (!beepOn) {
      beepOn = true;
      tone(pinBuzzer, BEEP_FREQ);
    }
    ledGradient(ratio);
    return;
  }

  uint32_t period = (elapsed < STEP1_END) ? 1000 : (elapsed < STEP2_END) ? 500 : 250;

  if (!beepOn && now >= nextBeepAt) {
    beepOn = true;
    beepOffAt = now + BEEP_ON_MS;
    nextBeepAt = now + period;
    tone(pinBuzzer, BEEP_FREQ);
    ledGradient(ratio);
  } else if (beepOn && now >= beepOffAt) {
    beepOn = false;
    noTone(pinBuzzer);
    setLed(0, 0, 0);
  }
}

// Som da explosão: frequência cai de BOOM_START_HZ até BOOM_END_HZ (efeito "tuuum")
void explosionSound() {
  unsigned long dt = millis() - phaseStart;
  if (dt < BOOM_SWEEP_MS) {
    float k = (float)dt / (float)BOOM_SWEEP_MS;
    int f = BOOM_START_HZ - (int)((BOOM_START_HZ - BOOM_END_HZ) * k * k);
    tone(pinBuzzer, f);
  } else {
    noTone(pinBuzzer);
  }
}

// ==========================================
// SETUP
// ==========================================
void setup() {
  Serial.begin(115200);

  pinMode(pinBuzzer, OUTPUT);
  pinMode(pinSmoke, OUTPUT);
  pinMode(pinLedR, OUTPUT);
  pinMode(pinLedG, OUTPUT);
  pinMode(pinLedB, OUTPUT);
  allOff();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Conectando ao Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi conectado!");

  // Hora via internet (necessária para ficar sincronizado com os sites)
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  Serial.print("Sincronizando hora");
  while (time(nullptr) < 1700000000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nHora sincronizada!");

  config.database_url = FIREBASE_HOST;
  config.signer.test_mode = true;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // A leitura do Firebase roda no núcleo 0; o loop() (bips/LEDs) roda no núcleo 1
  xTaskCreatePinnedToCore(firebaseTask, "firebase", 12288, NULL, 1, NULL, 0);

  Serial.println("Sistema pronto e conectado ao Firebase.");
}

// ==========================================
// LOOP
// ==========================================
void loop() {
  RemoteState r;
  portENTER_CRITICAL(&mux);
  r = remote;
  portEXIT_CRITICAL(&mux);

  int64_t nowMs = epochMs();

  if (r.status == RS_ARMED) {
    if (r.armId != curArmId) {
      // Nova armação (ou re-armar/reset feito no admin)
      curArmId = r.armId;
      if ((int64_t)r.fimEm - nowMs > 0) {
        startArmed(r);
      } else {
        Serial.println("ARMADA antiga ignorada (tempo já passou).");
      }
    } else if (phase == PH_ARMED) {
      endMs = (int64_t)r.fimEm;  // acompanha ajustes (ex.: -5s por código errado)
    }
  } else if (r.status == RS_IDLE) {
    // Desarmada pelo admin ou pelo site de desarme (ou reset)
    if (phase == PH_ARMED) {
      startDisarmed();
    } else if (phase == PH_EXPLODING) {
      allOff();
      phase = PH_IDLE;
    }
  } else if (r.status == RS_DETONATED) {
    // Os sites detonaram primeiro: a bomba física também explode
    if (phase == PH_ARMED) startExplosion(false);
  }

  switch (phase) {
    case PH_ARMED: {
      int64_t remaining = endMs - nowMs;
      if (remaining <= 0) {
        startExplosion(true);
      } else {
        armedEffects(remaining);
      }
      break;
    }
    case PH_EXPLODING:
      explosionSound();
      if (millis() - phaseStart >= EXPLOSION_MS) {
        allOff();
        phase = PH_IDLE;
      }
      break;
    case PH_DISARMED:
      if (millis() - phaseStart >= DISARM_FB_MS) {
        allOff();
        phase = PH_IDLE;
      }
      break;
    default:
      break;
  }
}