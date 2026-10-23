/*
 * Vulkaris Control - Nano ESP32 (Arduino ESP32 Boards 2.x)
 * BLE + L298N + 2 servos MG90 continuos + HC-SR04
 * Garra: A1 (0x10) abre ate 120 graus estimados; A2 (0x11) fecha ate 0
 * Cabeca: A3 ativa e A4 desativa movimento aleatorio.
 * Motores: 0x01 frente, 0x02 re, 0x03 esquerda, 0x04 direita.
 * STOP 0x00, emergencia 0x7F, rearmar 0x7E.
 * Slider 0x21 permanece aceito, mas nao move a garra.
 */
#include <Arduino.h>
#include <Servo.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <math.h>

// Pinos nos conectores identificados do Nano ESP32
constexpr int ENA = D5, IN1 = D3, IN2 = D4;
constexpr int ENB = D6, IN3 = D11, IN4 = D9;
constexpr int TRIG = D7, ECHO = D8;
constexpr int PIN_GARRA = D13, PIN_CABECA = D12;

// Canais separados daqueles normalmente alocados por Servo.h.
constexpr int PWM_ESQ = 6, PWM_DIR = 7;
constexpr bool INVERTER_ESQ = false, INVERTER_DIR = false;
constexpr bool INVERTER_GARRA = true;

Servo servoGarra, servoCabeca;
constexpr int SERVO_PARADO = 1500;
constexpr int SERVO_HORARIO = 1700;
constexpr int SERVO_ANTI = 1300;
constexpr uint8_t ABERTURA_GARRA_GRAUS = 120;
constexpr float TEMPO_VOLTA_GARRA_ABRIR = 900.0f; // ms/360 graus, abertura
constexpr float TEMPO_VOLTA_GARRA_FECHAR = 850.0f; // ms/360 graus, fechamento (5% menor)
constexpr float TEMPO_VOLTA_CABECA = 1000.0f;
constexpr float TOLERANCIA_GARRA = 2.0f;
constexpr uint32_t MAX_GARRA_MS = 600;

float posicaoGarra = 0.0f; // deve corresponder fisicamente a garra fechada (0 grau)
float destinoGarra = 0.0f;
int sentidoGarra = 0; // -1, 0, +1: sentido LOGICO
uint32_t ultimaAtualizacaoGarra = 0;
uint32_t inicioAcionamentoGarra = 0;
bool garraLimitada = false;
uint8_t botaoGarraAtivo = 0; // 0 = solto, 0x10 = A1, 0x11 = A2

bool cabecaAutomatica = false, cabecaMovendo = false;
uint32_t inicioCabeca = 0, duracaoCabeca = 0, proximaCabeca = 0;

constexpr float ALERTA_CM = 10.0f, LIVRE_CM = 15.0f;
constexpr uint32_t SENSOR_MS = 80, RECUO_MS = 550, GIRO_MS = 650, ESTABILIZAR_MS = 150;
constexpr uint8_t VELOCIDADE_DESVIO = 65;
float distanciaCm = -1.0f;
uint32_t ultimaLeitura = 0;

enum EstadoDesvio { MANUAL, RECUANDO, GIRANDO, VERIFICANDO, BLOQUEADO };
EstadoDesvio estadoDesvio = MANUAL;
uint32_t inicioDesvio = 0;
uint8_t tentativas = 0;

constexpr char SERVICE_UUID[] = "f3641400-00b0-4240-ba50-05ca45bf8abc";
constexpr char COMMAND_UUID[] = "f3641401-00b0-4240-ba50-05ca45bf8abc";
constexpr char ACK_UUID[] = "f3641402-00b0-4240-ba50-05ca45bf8abc";
constexpr uint8_t VERSION = 1;
constexpr uint32_t WATCHDOG_MS = 500;
enum Result : uint8_t { Accepted = 0, UnknownOpcode = 1, InvalidPacket = 2, EmergencyLocked = 3 };
struct CommandFrame { uint8_t bytes[4]; size_t length; };
QueueHandle_t commandQueue = nullptr;
BLECharacteristic* ackChar = nullptr;
std::atomic<bool> connected{false}, disconnected{false}, queueOverflow{false};
bool emergencia = false;
uint8_t movimentoManual = 0, velocidade = 70;
uint32_t ultimoComandoMotor = 0;

void pararMotores() {
  ledcWrite(PWM_ESQ, 0);
  ledcWrite(PWM_DIR, 0);
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
}
void ajustarVelocidade(uint8_t percentual) {
  int pwm = map(percentual, 0, 100, 0, 255);
  ledcWrite(PWM_ESQ, pwm); ledcWrite(PWM_DIR, pwm);
}
void sentidoMotor(int p1, int p2, bool frente, bool inverter) {
  bool sentido = frente != inverter;
  digitalWrite(p1, sentido ? HIGH : LOW);
  digitalWrite(p2, sentido ? LOW : HIGH);
}
void moverRobo(uint8_t direcao, uint8_t vel) {
  pararMotores();
  if (vel == 0) return;
  switch (direcao) {
    case 1:
      sentidoMotor(IN1, IN2, true, INVERTER_ESQ);
      sentidoMotor(IN3, IN4, true, INVERTER_DIR); break;
    case 2:
      sentidoMotor(IN1, IN2, false, INVERTER_ESQ);
      sentidoMotor(IN3, IN4, false, INVERTER_DIR); break;
    case 3:
      sentidoMotor(IN1, IN2, false, INVERTER_ESQ);
      sentidoMotor(IN3, IN4, true, INVERTER_DIR); break;
    case 4:
      sentidoMotor(IN1, IN2, true, INVERTER_ESQ);
      sentidoMotor(IN3, IN4, false, INVERTER_DIR); break;
    default: return;
  }
  ajustarVelocidade(vel);
}

void atualizarEstimativaGarra() {
  uint32_t agora = millis();
  if (sentidoGarra != 0) {
    uint32_t decorrido = agora - ultimaAtualizacaoGarra;
    // Calibracoes separadas: no retorno o servo e mais rapido.
    float tempoVolta = sentidoGarra > 0
      ? TEMPO_VOLTA_GARRA_ABRIR
      : TEMPO_VOLTA_GARRA_FECHAR;
    float graus = decorrido * 360.0f / tempoVolta;
    posicaoGarra += sentidoGarra * graus;
    posicaoGarra = constrain(posicaoGarra, 0.0f, (float)ABERTURA_GARRA_GRAUS);
  }
  ultimaAtualizacaoGarra = agora;
}
void pararGarra() {
  atualizarEstimativaGarra();
  servoGarra.writeMicroseconds(SERVO_PARADO);
  sentidoGarra = 0;
}
void cancelarDestinoGarra() {
  pararGarra();
  destinoGarra = posicaoGarra;
}
void definirDestinoGarra(uint8_t angulo) {
  atualizarEstimativaGarra();
  float novoDestino = (float)constrain((int)angulo, 0, (int)ABERTURA_GARRA_GRAUS);
  if (fabsf(novoDestino - destinoGarra) > 0.5f) {
    destinoGarra = novoDestino;
    garraLimitada = false; // apenas novo destino libera novo percurso
  }
  Serial.printf("GARRA: estimado %.1f -> %.1f\n", posicaoGarra, destinoGarra);
}
void controlarGarraSlider() {
  atualizarEstimativaGarra();
  if (garraLimitada) return;
  if (sentidoGarra != 0 && millis() - inicioAcionamentoGarra >= MAX_GARRA_MS) {
    cancelarDestinoGarra();
    garraLimitada = true;
    Serial.println("GARRA: limite de 600 ms, aguardando novo destino");
    return;
  }
  float diferenca = destinoGarra - posicaoGarra;
  if (fabsf(diferenca) <= TOLERANCIA_GARRA) {
    if (sentidoGarra != 0) pararGarra();
    return;
  }
  int novoSentido = diferenca > 0 ? 1 : -1;
  if (novoSentido != sentidoGarra) {
    // Cada novo trecho de movimentacao tem seu proprio timeout.
    inicioAcionamentoGarra = millis();
    bool horario = novoSentido > 0;
    if (INVERTER_GARRA) horario = !horario;
    servoGarra.writeMicroseconds(horario ? SERVO_HORARIO : SERVO_ANTI);
    sentidoGarra = novoSentido;
  }
}

void pararCabeca() {
  servoCabeca.writeMicroseconds(SERVO_PARADO);
  cabecaMovendo = false;
}
void atualizarCabecaAleatoria() {
  if (!cabecaAutomatica || !connected.load() || emergencia) return;
  uint32_t agora = millis();
  if (cabecaMovendo) {
    if (agora - inicioCabeca >= duracaoCabeca) pararCabeca();
    return;
  }
  if ((int32_t)(agora - proximaCabeca) < 0) return;
  bool horario = random(0, 2) == 1;
  int angulo = random(15, 46);
  duracaoCabeca = min((uint32_t)(angulo * TEMPO_VOLTA_CABECA / 360.0f), (uint32_t)250);
  inicioCabeca = agora;
  cabecaMovendo = true;
  servoCabeca.writeMicroseconds(horario ? SERVO_HORARIO : SERVO_ANTI);
  proximaCabeca = agora + duracaoCabeca + random(800, 2001);
}

float lerDistancia() {
  digitalWrite(TRIG, LOW); delayMicroseconds(2);
  digitalWrite(TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG, LOW);
  uint32_t duracao = pulseIn(ECHO, HIGH, 20000UL);
  if (duracao == 0) return -1;
  float cm = duracao * 0.0343f / 2.0f;
  return (cm >= 2 && cm <= 340) ? cm : -1;
}
void atualizarSensor() {
  if (millis() - ultimaLeitura < SENSOR_MS) return;
  ultimaLeitura = millis();
  distanciaCm = lerDistancia();
}
bool obstaculoProximo() { return distanciaCm > 0 && distanciaCm <= ALERTA_CM; }
bool caminhoLivre() { return distanciaCm >= LIVRE_CM; }

void iniciarDesvio() {
  if (estadoDesvio != MANUAL || emergencia || !connected.load()) return;
  movimentoManual = 0;
  pararMotores();
  tentativas = 0;
  estadoDesvio = RECUANDO;
  inicioDesvio = millis();
  moverRobo(2, VELOCIDADE_DESVIO);
  Serial.println("DESVIO: recuando");
}
void atualizarDesvio() {
  if (estadoDesvio == MANUAL) return;
  uint32_t agora = millis();
  switch (estadoDesvio) {
    case RECUANDO:
      if (agora - inicioDesvio >= RECUO_MS) {
        pararMotores();
        moverRobo(tentativas % 2 == 0 ? 4 : 3, VELOCIDADE_DESVIO);
        estadoDesvio = GIRANDO; inicioDesvio = agora;
      }
      break;
    case GIRANDO:
      if (agora - inicioDesvio >= GIRO_MS) {
        pararMotores(); estadoDesvio = VERIFICANDO; inicioDesvio = agora;
      }
      break;
    case VERIFICANDO:
      if (agora - inicioDesvio < ESTABILIZAR_MS) break;
      if ((int32_t)(ultimaLeitura - (inicioDesvio + ESTABILIZAR_MS)) < 0) break;
      if (caminhoLivre()) {
        pararMotores(); estadoDesvio = MANUAL; movimentoManual = 0;
        Serial.println("DESVIO: controle manual liberado");
      } else {
        ++tentativas;
        if (tentativas >= 2) {
          pararMotores(); estadoDesvio = BLOQUEADO;
          Serial.println("DESVIO: sem caminho livre");
        } else {
          estadoDesvio = RECUANDO; inicioDesvio = agora;
          moverRobo(2, VELOCIDADE_DESVIO);
        }
      }
      break;
    case BLOQUEADO:
      if (caminhoLivre()) { pararMotores(); estadoDesvio = MANUAL; movimentoManual = 0; }
      break;
    default: break;
  }
}

void stopAll(const char* motivo) {
  movimentoManual = 0;
  estadoDesvio = MANUAL;
  cabecaAutomatica = false;
  pararMotores(); pararCabeca();
  cancelarDestinoGarra();
  garraLimitada = false;
  botaoGarraAtivo = 0;
  Serial.printf("STOP: %s\n", motivo);
}

Result executar(uint8_t opcode, uint8_t valor) {
  switch (opcode) {
    case 0x00: if (valor != 0) return InvalidPacket; break;
    case 0x01: case 0x02: case 0x03: case 0x04:
    case 0x7E: case 0x7F:
      if (valor != 1) return InvalidPacket; break;
    case 0x10: case 0x11: case 0x12: case 0x13:
      if (valor > 1) return InvalidPacket; break;
    case 0x20: if (valor > 100) return InvalidPacket; break;
    case 0x21: if (valor > 180) return InvalidPacket; break;
    default: return UnknownOpcode;
  }
  if (opcode == 0x00) { stopAll("STOP BLE"); return Accepted; }
  if (opcode == 0x7F) { emergencia = true; stopAll("EMERGENCIA"); return Accepted; }
  if (opcode == 0x7E) { stopAll("REARME"); emergencia = false; return Accepted; }
  if (emergencia) return EmergencyLocked;

  if (opcode >= 0x01 && opcode <= 0x04) {
    // O ultrassonico nao bloqueia nenhum comando de direcao.
    if (velocidade == 0) { pararMotores(); movimentoManual = 0; return Accepted; }
    if (movimentoManual != opcode) moverRobo(opcode, velocidade);
    movimentoManual = opcode;
    ultimoComandoMotor = millis();
  } else if (opcode == 0x10 || opcode == 0x11) {
    // A1: abrir ate 120 graus estimados.
    // A2: fechar ate 0 grau estimado.
    // Um toque inicia o percurso temporizado completo.
    // Soltar o botao nao cancela o percurso; STOP cancela.
    if (valor == 0) {
      if (botaoGarraAtivo == opcode) botaoGarraAtivo = 0;
    } else if (botaoGarraAtivo != opcode) {
      botaoGarraAtivo = opcode;
      definirDestinoGarra(opcode == 0x10 ? ABERTURA_GARRA_GRAUS : 0);
    }
  } else if (opcode == 0x12 && valor == 1) {
    cabecaAutomatica = true; proximaCabeca = millis();
  } else if (opcode == 0x13 && valor == 1) {
    cabecaAutomatica = false; pararCabeca();
  } else if (opcode == 0x20) {
    velocidade = valor;
    if (estadoDesvio == MANUAL) {
      if (valor == 0) { pararMotores(); movimentoManual = 0; }
      else if (movimentoManual != 0) ajustarVelocidade(valor);
    }
  } else if (opcode == 0x21) {
    // Slider desativado para a garra. Mantido para compatibilidade BLE.
    // A1 e A2 sao os controles exclusivos de abertura/fechamento.
  }
  return Accepted;
}
void acknowledge(uint8_t seq, Result resultado) {
  if (!connected.load()) return;
  uint8_t pacote[] = { VERSION, seq, (uint8_t)resultado };
  ackChar->setValue(pacote, sizeof(pacote));
  ackChar->notify();
}
class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    const auto valor = c->getValue();
    CommandFrame frame{};
    frame.length = valor.length();
    size_t n = frame.length < sizeof(frame.bytes) ? frame.length : sizeof(frame.bytes);
    for (size_t i = 0; i < n; ++i) frame.bytes[i] = (uint8_t)valor[i];
    if (xQueueSend(commandQueue, &frame, 0) != pdTRUE) queueOverflow.store(true);
  }
};
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) override { connected.store(true); Serial.println("BLE conectado"); }
  void onDisconnect(BLEServer*) override { connected.store(false); disconnected.store(true); }
};

void setup() {
  Serial.begin(115200); // apenas diagnostico; controle sempre por BLE
  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
  pinMode(TRIG, OUTPUT); pinMode(ECHO, INPUT);
  digitalWrite(TRIG, LOW);

  // Primeiro os servos, usando a biblioteca Servo.h que funcionou no teste.
  servoGarra.writeMicroseconds(SERVO_PARADO);
  servoCabeca.writeMicroseconds(SERVO_PARADO);
  servoGarra.attach(PIN_GARRA);
  servoCabeca.attach(PIN_CABECA);
  servoGarra.writeMicroseconds(SERVO_PARADO);
  servoCabeca.writeMicroseconds(SERVO_PARADO);
  ultimaAtualizacaoGarra = millis();

  // PWM da L298N, em canais separados dos servos.
  ledcSetup(PWM_ESQ, 20000, 8);
  ledcSetup(PWM_DIR, 20000, 8);
  ledcAttachPin(ENA, PWM_ESQ);
  ledcAttachPin(ENB, PWM_DIR);
  pararMotores();

  commandQueue = xQueueCreate(16, sizeof(CommandFrame));
  if (!commandQueue) {
    Serial.println("Falha fila BLE");
    while (true) { pararMotores(); cancelarDestinoGarra(); pararCabeca(); delay(1000); }
  }
  randomSeed(micros());
  BLEDevice::init("Vulkaris Nano");
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  BLEService* service = server->createService(SERVICE_UUID);
  BLECharacteristic* commandChar = service->createCharacteristic(
    COMMAND_UUID, BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_WRITE);
  commandChar->setCallbacks(new CommandCallbacks());
  ackChar = service->createCharacteristic(ACK_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  ackChar->addDescriptor(new BLE2902());
  service->start();
  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->start();
  ultimaLeitura = millis() - SENSOR_MS;
  Serial.println("Vulkaris Control online - BLE | A1: 120 graus | A2: 0 grau");
}

void loop() {
  if (disconnected.exchange(false)) {
    stopAll("BLE desconectado");
    xQueueReset(commandQueue);
    BLEDevice::startAdvertising();
  }
  if (queueOverflow.exchange(false)) {
    emergencia = true;
    stopAll("Fila BLE cheia");
    xQueueReset(commandQueue);
  }

  CommandFrame frame{};
  if (connected.load() && xQueueReceive(commandQueue, &frame, 0) == pdTRUE) {
    Result resultado = InvalidPacket;
    if (frame.length == 4 && frame.bytes[0] == VERSION)
      resultado = executar(frame.bytes[2], frame.bytes[3]);
    if (frame.length >= 2) acknowledge(frame.bytes[1], resultado);
  }
  atualizarSensor();
  if (connected.load() && !emergencia) {
    controlarGarraSlider(); // agora o destino vem dos botoes A1/A2
    // O sensor apenas monitora a distancia; desvio automatico desativado.
    atualizarCabecaAleatoria();
  }
  if (estadoDesvio == MANUAL && movimentoManual != 0 &&
      millis() - ultimoComandoMotor >= WATCHDOG_MS) {
    pararMotores(); movimentoManual = 0;
  }
  delay(1);
}
