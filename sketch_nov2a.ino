#include <ThreeWire.h>
#include <RtcDS1302.h>

// ---------- ПИНЫ ----------
const int trig1 = 6;        // верхний датчик (прямо)
const int echo1 = 7;
const int trig2 = 4;        // нижний датчик (вперёд-вниз)
const int echo2 = 5;
const int sensor2Power = 2; // VCC нижнего датчика подключён сюда
const int buzzerPin = 11;

ThreeWire myWire(10, 8, A0);   // DAT, CLK, RST
RtcDS1302<ThreeWire> Rtc(myWire);

// ---------- НАСТРОЙКИ ----------
const bool SET_TIME  = false;   // true только для первой загрузки!
const bool USE_SIREN = true;

const int MAX_DIST    = 100;
const int MIN_DIST    = 5;
const int ALARM_RADAR = 50;
const int HAND_DIST   = 8;
const int CANCEL_DIST = 15;
const unsigned long STOP_ALARM_MS = 1000;
const unsigned long SIREN_HOLD_MS = 3000;
const unsigned long SIREN_LEN_MS  = 10000;
const unsigned long ALARM_MAX_MS  = 60000;

struct Reminder { int hour; int minute; int type; };
Reminder reminders[] = { {8, 0, 1}, {13, 30, 2}, {21, 0, 3} };
const int COUNT = 3;

// ---------- СОСТОЯНИЕ ----------
unsigned long nearSince = 0, lastNear = 0;
unsigned long nextBeep1 = 0, nextBeep2 = 0, busyUntil = 0;
unsigned long lastRtcCheck = 0, alarmStart = 0;
bool blockUntilRelease = false;
bool alarmOn = false;
int alarmType = 0;
long lastAlarmKey = -1;
int lastMinute = -1;
int dblStage = 0;
unsigned long dblAt = 0;
int dblFreq = 0;

// ---------- ДАТЧИК ----------
long getDistance(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  long duration = pulseIn(echoPin, HIGH, 25000);
  if (duration == 0) return 999;
  return duration * 0.034 / 2;
}

// ---------- ЖЕСТ (по верхнему датчику) ----------
bool holdNear(long d, unsigned long needMs) {
  unsigned long now = millis();
  if (blockUntilRelease) {
    if (d >= HAND_DIST) { blockUntilRelease = false; nearSince = 0; }
    return false;
  }
  if (d < HAND_DIST) {
    if (nearSince == 0) nearSince = now;
    lastNear = now;
    return (now - nearSince >= needMs);
  }
  if (now - lastNear > 300) nearSince = 0;
  return false;
}

// ---------- РАДАР 1: короткие высокие одиночные ----------
void radar1Tick(long d) {
  unsigned long now = millis();
  if (now < nextBeep1 || now < busyUntil) return;
  d = constrain(d, MIN_DIST, MAX_DIST);
  int pause = map(d, MIN_DIST, MAX_DIST, 40, 800);
  int freq  = map(d, MIN_DIST, MAX_DIST, 3000, 1000);
  tone(buzzerPin, freq, 40);
  busyUntil = now + 40;
  nextBeep1 = now + pause;
}

// ---------- РАДАР 2: двойной писк пониже ----------
void radar2Tick(long d) {
  unsigned long now = millis();
  if (now < nextBeep2 || now < busyUntil || dblStage != 0) return;
  d = constrain(d, MIN_DIST, MAX_DIST);
  int pause = map(d, MIN_DIST, MAX_DIST, 150, 900);
  dblFreq   = map(d, MIN_DIST, MAX_DIST, 900, 500);
  tone(buzzerPin, dblFreq, 35);
  busyUntil = now + 35;
  dblStage = 1;
  dblAt = now + 80;
  nextBeep2 = now + pause;
}

void doubleBeepTick() {         // второй писк «двойного»
  unsigned long now = millis();
  if (dblStage == 1 && now >= dblAt && now >= busyUntil) {
    tone(buzzerPin, dblFreq, 35);
    busyUntil = now + 35;
    dblStage = 0;
  }
}

// ---------- НАПОМИНАНИЯ ----------
void checkReminders() {
  RtcDateTime t = Rtc.GetDateTime();
  if (t.Minute() == lastMinute) return;
  lastMinute = t.Minute();
  for (int i = 0; i < COUNT; i++) {
    if (reminders[i].hour == t.Hour() && reminders[i].minute == t.Minute()) {
      alarmOn = true;
      alarmType = reminders[i].type;
      alarmStart = millis();
      lastAlarmKey = -1;
    }
  }
}

void alarmTick() {
  unsigned long el = millis() - alarmStart;
  if (el > ALARM_MAX_MS) { alarmOn = false; return; }
  unsigned long cycle = alarmType * 500UL + 2500UL;
  unsigned long phase = el % cycle;
  unsigned long n = el / cycle;
  if (phase < alarmType * 500UL && (phase % 500UL) < 400UL) {
    long key = (long)(n * 10 + phase / 500UL);
    if (key != lastAlarmKey) {
      tone(buzzerPin, 400, 400);
      busyUntil = millis() + 400;
      lastAlarmKey = key;
    }
  }
}

// ---------- СИРЕНА ----------
void sirenSequence() {
  for (int i = 0; i < 3; i++) {
    tone(buzzerPin, 600, 300);
    delay(500);
    if (getDistance(trig1, echo1) >= CANCEL_DIST) {
      tone(buzzerPin, 1500, 150);
      nearSince = 0;
      blockUntilRelease = true;
      return;
    }
  }
  unsigned long start = millis();
  while (millis() - start < SIREN_LEN_MS) {
    for (int f = 800; f <= 2000; f += 50) { tone(buzzerPin, f); delay(10); }
    for (int f = 2000; f >= 800; f -= 50) { tone(buzzerPin, f); delay(10); }
  }
  noTone(buzzerPin);
  nearSince = 0;
  blockUntilRelease = true;
}

// ---------- ОСНОВНОЕ ----------
void setup() {
  pinMode(sensor2Power, OUTPUT);
  digitalWrite(sensor2Power, HIGH);   // питание нижнего датчика
  delay(100);                         // пусть датчик запустится

  pinMode(trig1, OUTPUT);
  pinMode(echo1, INPUT);
  pinMode(trig2, OUTPUT);
  pinMode(echo2, INPUT);
  pinMode(buzzerPin, OUTPUT);

  Rtc.Begin();
  if (SET_TIME) {
    Rtc.SetIsWriteProtected(false);
    Rtc.SetIsRunning(true);
    Rtc.SetDateTime(RtcDateTime(__DATE__, __TIME__));
  }
  tone(buzzerPin, 1000, 200);   // включилось
}

void loop() {
  long d1 = getDistance(trig1, echo1);
  delay(30);                    // пауза, чтобы датчики не слышали друг друга
  long d2 = getDistance(trig2, echo2);
  unsigned long now = millis();

  if (now - lastRtcCheck > 500) { lastRtcCheck = now; checkReminders(); }

  bool handUp = (d1 < HAND_DIST);   // ладонь у лица: нижний датчик молчит

  if (alarmOn) {
    if (holdNear(d1, STOP_ALARM_MS)) {          // жест: выключить напоминание
      alarmOn = false;
      noTone(buzzerPin);
      tone(buzzerPin, 1500, 150);
      nearSince = 0;
      blockUntilRelease = true;
    } else if (d1 < ALARM_RADAR || (d2 < ALARM_RADAR && !handUp)) {
      if (d1 < ALARM_RADAR) radar1Tick(d1);     // препятствие главнее напоминания
      if (d2 < ALARM_RADAR && !handUp) radar2Tick(d2);
    } else {
      alarmTick();
    }
  } else {
    if (USE_SIREN && holdNear(d1, SIREN_HOLD_MS)) {
      sirenSequence();                          // жест: сирена
    } else {
      if (d1 < MAX_DIST) radar1Tick(d1);
      if (d2 < MAX_DIST && !handUp) radar2Tick(d2);
    }
  }
  doubleBeepTick();
}
