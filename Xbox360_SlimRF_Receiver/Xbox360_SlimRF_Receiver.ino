/*
 * Xbox 360 RF Module (1410/1575) -> PC via USB  |  Wemos D1 Mini (ESP8266)
 * Baseado em: appliedcarbon.org, dazzaXx, AuraudZ, gr33nonline, ginokgx
 *
 * Ligacoes (do diagrama):
 *   RF Data  (rosa)    -> GPIO4  (D2)
 *   RF Clock (azul)    -> GPIO5  (D1)
 *   RF Sync  (amarelo) -> GPIO14 (D5)
 *   RF 3.3V  (laranja) -> 3.3V
 *   RF GND             -> GND
 *   RF D+/D-           -> cabo USB do PC (o ESP NAO participa do USB)
 *   VBUS do ESP        -> +5V do cabo USB
 *
 * Funcoes:
 *   - Inicializacao dos LEDs + animacao de boot
 *   - Botao Sync (fisico no modulo) -> modo de pareamento
 *   - Controle de LEDs dos quadrantes (verde/vermelho/laranja)
 *   - Efeitos (spin, blink, erro)
 *   - Comandos pela Serial (115200) para testes
 *   - Timeout no clock (evita travar o watchdog do ESP)
 */

#include <Arduino.h>

#define PIN_DATA   4    // D2
#define PIN_CLOCK  5    // D1
#define PIN_SYNC   14   // D5

#define CLK_TIMEOUT_US 20000UL

// ---------- Comandos do protocolo (10 bits) ----------
#define CMD_LED_INIT     0x084  // 0010000100 - inicializa LEDs
#define CMD_BOOT_ANIM    0x085  // 0010000101 - animacao de boot
#define CMD_SYNC         0x004  // 0000000100 - pareamento
#define CMD_LED_OFF      0x0C0  // 0011000000 - apaga LEDs
#define CMD_GREEN_BASE   0x0A0  // 00101xxxx - verde  (4 bits = quadrantes)
#define CMD_RED_BASE     0x0B0  // 00101 1xxxx - vermelho
#define CMD_CTRL_OFF     0x009  // 0000001001 - desliga controles conectados

// Quadrantes (bits): Q1=0x1 Q2=0x2 Q3=0x4 Q4=0x8
#define Q1 0x1
#define Q2 0x2
#define Q3 0x4
#define Q4 0x8
#define QALL 0xF

bool rfReady = false;
unsigned long lastSyncPress = 0;

// ---------- Baixo nível ----------
bool waitClockChange(int prev) {
  unsigned long t = micros();
  while (digitalRead(PIN_CLOCK) == prev) {
    if (micros() - t > CLK_TIMEOUT_US) return false;
    yield();
  }
  return true;
}

bool sendCmd(uint16_t cmd) {
  noInterrupts();
  pinMode(PIN_DATA, OUTPUT);
  digitalWrite(PIN_DATA, LOW);              // requisita envio
  int prev = digitalRead(PIN_CLOCK);
  bool ok = true;

  for (int i = 9; i >= 0; i--) {            // MSB primeiro
    interrupts();
    if (!waitClockChange(prev)) { ok = false; break; }
    prev = digitalRead(PIN_CLOCK);
    digitalWrite(PIN_DATA, (cmd >> i) & 1); // seta bit na borda
    if (!waitClockChange(prev)) { ok = false; break; }
    prev = digitalRead(PIN_CLOCK);
  }
  interrupts();

  digitalWrite(PIN_DATA, HIGH);
  pinMode(PIN_DATA, INPUT_PULLUP);
  delay(50);

  Serial.printf("CMD 0x%03X -> %s\n", cmd, ok ? "OK" : "TIMEOUT");
  return ok;
}

// ---------- Alto nível ----------
void ledsOff()                 { sendCmd(CMD_LED_OFF); }
void ledsGreen(uint8_t q)      { sendCmd(CMD_GREEN_BASE | (q & 0xF)); }
void ledsRed(uint8_t q)        { sendCmd(CMD_RED_BASE   | (q & 0xF)); }
void ledsOrange(uint8_t q)     { ledsGreen(q); ledsRed(q); }   // verde+vermelho
void syncMode()                { Serial.println("Modo SYNC"); sendCmd(CMD_SYNC); }
void controllersOff()          { sendCmd(CMD_CTRL_OFF); }

void initRF() {
  delay(2000);                              // modulo estabilizar
  rfReady = sendCmd(CMD_LED_INIT);
  delay(100);
  sendCmd(CMD_BOOT_ANIM);
  Serial.println(rfReady ? "RF pronto" : "RF nao respondeu (verifique clock/data)");
}

void effectSpin(uint8_t color, int turns) {           // 0=verde 1=verm 2=laranja
  const uint8_t seq[4] = {Q1, Q2, Q4, Q3};
  for (int t = 0; t < turns; t++)
    for (int i = 0; i < 4; i++) {
      if (color == 0) ledsGreen(seq[i]);
      else if (color == 1) ledsRed(seq[i]);
      else ledsOrange(seq[i]);
      delay(120);
    }
  ledsOff();
}

void effectBlink(uint8_t color, int n) {
  for (int i = 0; i < n; i++) {
    if (color == 0) ledsGreen(QALL); else if (color == 1) ledsRed(QALL); else ledsOrange(QALL);
    delay(250); ledsOff(); delay(250);
  }
}

void effectRRoD() { ledsRed(Q1 | Q2 | Q4); }          // "Red Ring" de brincadeira

// ---------- Botao Sync ----------
void handleSyncButton() {
  if (digitalRead(PIN_SYNC) == LOW && millis() - lastSyncPress > 1000) {
    delay(30);
    if (digitalRead(PIN_SYNC) == LOW) {
      lastSyncPress = millis();
      unsigned long t = millis();
      while (digitalRead(PIN_SYNC) == LOW) yield();
      if (millis() - t > 3000) controllersOff();      // segurar 3s = desliga controles
      else syncMode();
    }
  }
}

// ---------- Serial ----------
void printHelp() {
  Serial.println(F(
    "\nComandos:\n"
    " i  init LEDs + boot\n"
    " s  sync\n"
    " o  apagar LEDs\n"
    " g<hex> verde  (ex: gF, g1)\n"
    " r<hex> vermelho\n"
    " y<hex> laranja\n"
    " p  spin    b  blink    e  RRoD\n"
    " c  desligar controles\n"
    " x<hex> comando bruto 10 bits (ex: x084)\n"
    " h  ajuda"));
}

void handleSerial() {
  if (!Serial.available()) return;
  String l = Serial.readStringUntil('\n'); l.trim();
  if (!l.length()) return;
  char c = l[0];
  uint16_t v = l.length() > 1 ? strtol(l.substring(1).c_str(), NULL, 16) : QALL;
  switch (c) {
    case 'i': initRF(); break;
    case 's': syncMode(); break;
    case 'o': ledsOff(); break;
    case 'g': ledsGreen(v); break;
    case 'r': ledsRed(v); break;
    case 'y': ledsOrange(v); break;
    case 'p': effectSpin(0, 3); break;
    case 'b': effectBlink(2, 3); break;
    case 'e': effectRRoD(); break;
    case 'c': controllersOff(); break;
    case 'x': sendCmd(v & 0x3FF); break;
    default:  printHelp();
  }
}

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_DATA, INPUT_PULLUP);
  pinMode(PIN_CLOCK, INPUT_PULLUP);
  pinMode(PIN_SYNC, INPUT_PULLUP);
  Serial.println("\nXbox 360 RF - ESP8266");
  initRF();
  printHelp();
}

void loop() {
  handleSyncButton();
  handleSerial();
  yield();
}
