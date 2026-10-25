// =====================================================================
//  ROBÔ CHAVEIRO v4 — olhos próprios + personalidade com memória
//
//  ESP32-C3 SuperMini + OLED SSD1306 128x64 (I2C) + 1 botão
//  SDA = GPIO8, SCL = GPIO9, botão no GPIO4 (outro lado no GND)
//
//  Controles na tela dos olhos:
//    1 clique  -> troca o humor (neutro, feliz, cansado, bravo, apaixonado, hacker)
//    2 cliques -> menu (jogos + status)
//  No menu: toque = próximo, segura = entra. No jogo: toque = ação, segura = sai.
//
//  Vida própria:
//   - pisca, respira, olha em volta e faz manias sozinho
//   - quando você aperta, ele para e olha pra você
//   - reage a como você foi no jogo (recorde = corações, foi mal = zoeira)
//   - sabe quanto tempo dormiu: acorda com saudade ou ranzinza
//   - tem "afeto": sobe quando você brinca, cai se esquecer dele
//   - vai escurecendo quando fica com sono, dorme com 5 min e apaga
//
//  Libs: Adafruit SSD1306 + Adafruit GFX. Só isso.
// =====================================================================

#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <sys/time.h>
#include "esp_sleep.h"
#include "esp_random.h"

// --- Tela ---
#define LARGURA 128
#define ALTURA  64
Adafruit_SSD1306 display(LARGURA, ALTURA, &Wire, -1);
#define BRANCO SSD1306_WHITE
#define PRETO  SSD1306_BLACK

// --- Pinos (C3 SuperMini) ---
#define PINO_SDA   8
#define PINO_SCL   9
#define PINO_BOTAO 4   // TEM que ser GPIO 0 a 5 no C3 (só esses acordam do sono profundo)

// --- Tempos do botão ---
const unsigned long JANELA_DUPLO = 350;  // ms pra contar clique duplo
const unsigned long TEMPO_LONGO  = 600;  // ms segurando = ação longa
const unsigned long DEBOUNCE_MS  = 25;   // ignora a "tremida" do botão mecânico

// --- Modos da tela ---
enum Modo { OLHOS, MENU, JOGO, TELA_STATUS };   // ("STATUS" puro já existe no core do ESP32)
Modo modo = OLHOS;

enum Jogo { J_DINO, J_FLAPPY, J_TIRO };
Jogo jogoAtual = J_DINO;

const char* ITENS_MENU[] = { "Dino", "Flappy", "Tiro Certo", "Status", "<- Olhos" };
const int NUM_ITENS = 5;
int menuSel = 0;

// --- Estado do botão ---
bool botaoAnterior = HIGH;
bool botaoEstavel = HIGH, botaoBruto = HIGH;
unsigned long botaoMudouEm = 0;
unsigned long inicioPress = 0;
bool longFired = false;
bool apertouNoFim = false;   // o aperto começou na tela de fim de jogo?
bool consumirHold = false;   // ignora o aperto que te trouxe pra tela nova (se ainda estiver segurando)
unsigned long ultimoClique = 0;
bool cliquePendente = false;
unsigned long tempoClique = 0;

// --- Geometria dos olhos ---
const int OLHO_W = 34, OLHO_H = 34, OLHO_R = 8;
const int OLHO_ESP = 12;
const int CX_E = 64 - OLHO_ESP/2 - OLHO_W/2;
const int CX_D = 64 + OLHO_ESP/2 + OLHO_W/2;
const int CY   = 30;
const int CXS[2] = { CX_E, CX_D };
const unsigned long FRAME_OLHOS = 25;                 // ~40 fps

// --- Brilho da tela (contraste do SSD1306, 0 a 255) ---
const int BRILHO_ACORDADO = 0xCF;
const int BRILHO_CANSADO  = 0x30;
const int BRILHO_DORMINDO = 0x02;

// --- Humor + ciclo de vida ---
const int NUM_HUMORES = 6;   // 0 neutro, 1 feliz, 2 cansado, 3 bravo, 4 apaixonado, 5 hacker
int humor = 0;
enum Vivo { ACORDADO, CANSADO, DORMINDO };
Vivo vivo = ACORDADO;
unsigned long ultimaInteracao = 0;
unsigned long proxGesto = 0;
// Pra testar, baixa esses valores (ex.: 15000 e 20000) e depois volta.
const unsigned long TEMPO_CANSADO  = 240000; // 4 min sem mexer = dá sono
const unsigned long TEMPO_DESLIGAR = 300000; // 5 min sem mexer = dorme de vez
const unsigned long TEMPO_FECHAR_OLHOS = 2500;
unsigned long dormirAte = 0;
const unsigned long GESTO_MIN = 5000, GESTO_MAX = 11000;
const unsigned long TEMPO_ATENCAO = 2500;    // quanto tempo ele te encara depois do toque
unsigned long atencaoAte = 0;

// --- Memória que sobrevive ao sono profundo (zera só se desligar na chave) ---
RTC_DATA_ATTR int afeto = 50;                    // 0 a 100
RTC_DATA_ATTR long dormiuEm = 0;                 // relógio (segundos) na hora que apagou
RTC_DATA_ATTR int recorde[3] = { 0, 0, 0 };      // dino, flappy, tiro
const long DORMIU_POUCO = 120;                   // acordou em menos de 2 min = ranzinza
const long DORMIU_MUITO = 3L * 3600;             // mais de 3 h = saudade

// --- Resultado do último jogo (pro robô reagir quando você voltar) ---
enum Resultado { RES_NADA, RES_NORMAL, RES_FOI_MAL, RES_RECORDE };
Resultado resultadoPendente = RES_NADA;
bool novoRecordeAgora = false;

// --- Reações (duram um tempo e voltam ao normal sozinhas) ---
enum Reacao {
  R_NENHUMA,
  // manias que ele faz sozinho
  R_RISADA, R_CONFUSO, R_PISCADINHA, R_SURPRESA, R_SUOR, R_DESCONFIADO,
  R_APAIXONADO, R_HACKER, R_TRISTE,
  // reações ao que você faz
  R_ACORDOU, R_FELIZ, R_IRRITADO, R_SAUDADE, R_RANZINZA, R_ZOANDO, R_ORGULHO
};
Reacao reacao = R_NENHUMA;
unsigned long reacaoInicio = 0, reacaoAte = 0;

// --- Como os olhos são desenhados no momento ---
enum ModoOlho { MO_NORMAL, MO_CORACAO, MO_ESPIRAL, MO_SURPRESO, MO_HACKER };
ModoOlho modoOlho = MO_NORMAL;
unsigned long modoOlhoInicio = 0;

// Protótipos manuais: a IDE às vezes declara as funções antes dos enums
// existirem e se perde. Declarando aqui, logo depois dos enums, não tem erro.
void iniciarReacao(Reacao r, unsigned long dur);
void iniciarJogo(Jogo j);
void setModoOlho(ModoOlho m);
void fimDeJogo(Jogo j, int pontos);

// --- Parâmetros animados: o valor "persegue" o alvo a cada quadro ---
float cansado = 0, cansadoAlvo = 0;   // pálpebra reta caindo (sono)
float bravo   = 0, bravoAlvo   = 0;   // pálpebra inclinada pra dentro
float triste  = 0, tristeAlvo  = 0;   // pálpebra inclinada pra fora
float feliz   = 0, felizAlvo   = 0;   // "mordida" embaixo que vira arco
float abertura = 1, aberturaAlvo = 1; // 1 = aberto, 0 = fechado
float escH[2] = { 1, 1 }, escHAlvo[2] = { 1, 1 };
float atencao = 0;                    // 0 a 1: quanto ele tá te encarando
float brilho = BRILHO_ACORDADO;
int brilhoEnviado = -1;
bool suor = false, lagrimas = false, tremor = false;
unsigned long ultimoFrameOlhos = 0;

// --- Detector de "para de me cutucar" ---
unsigned long spamInicio = 0;
int spamCliques = 0;
const int SPAM_LIMITE = 8;
const unsigned long SPAM_JANELA = 4000;

// --- Loop de jogo ---
const int FRAME_MS = 20;
unsigned long ultimoFrame = 0;

// --- Countdown 3,2,1,VAI ---
bool emContagem = false;
unsigned long contagemInicio = 0;
const unsigned long PASSO_CONTAGEM = 600;

// =====================================================================
//  UTILITÁRIOS
// =====================================================================

// Relógio em segundos. Continua contando durante o sono profundo.
long segundosRelogio() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec;
}

void mudarAfeto(int delta) {
  afeto += delta;
  if (afeto < 0) afeto = 0;
  if (afeto > 100) afeto = 100;
}

void aproximar(float &valor, float alvo, float k) { valor += (alvo - valor) * k; }

// Manda o brilho pra tela só quando muda de verdade (economiza I2C)
void enviarBrilho(int b) {
  if (b < 0) b = 0;
  if (b > 255) b = 255;
  if (abs(b - brilhoEnviado) < 3 && b != 0 && b != 255) return;
  display.ssd1306_command(SSD1306_SETCONTRAST);
  display.ssd1306_command(b);
  brilhoEnviado = b;
}
void brilhoCheio() { brilho = BRILHO_ACORDADO; enviarBrilho(BRILHO_ACORDADO); }

// Botão com debounce: só aceita mudança que ficou estável por DEBOUNCE_MS
bool lerBotao() {
  bool bruto = digitalRead(PINO_BOTAO);
  if (bruto != botaoBruto) { botaoBruto = bruto; botaoMudouEm = millis(); }
  if (millis() - botaoMudouEm >= DEBOUNCE_MS) botaoEstavel = botaoBruto;
  return botaoEstavel;
}

// =====================================================================
//  PINCÉIS (peças de desenho)
// =====================================================================

void olho(int cx, int cy, int w, int h, int r) {
  if (h < 2) h = 2;
  int maxR = min(w, h) / 2;
  if (r > maxR) r = maxR;
  display.fillRoundRect(cx - w/2, cy - h/2, w, h, r, BRANCO);
}

void olhoFechadoArco(int cx, int cy) {   // "^"
  for (int k = 0; k < 3; k++) {
    display.drawLine(cx - 14, cy + 5 + k, cx, cy - 4 + k, BRANCO);
    display.drawLine(cx, cy - 4 + k, cx + 14, cy + 5 + k, BRANCO);
  }
}

void coracao(int cx, int cy, int r) {
  display.fillCircle(cx - r, cy, r, BRANCO);
  display.fillCircle(cx + r, cy, r, BRANCO);
  display.fillTriangle(cx - 2*r, cy + r/3, cx + 2*r, cy + r/3, cx, cy + 2*r + r/2, BRANCO);
}

void lagrima(int x, int y) {
  display.fillCircle(x, y, 2, BRANCO);
  display.fillTriangle(x - 2, y, x + 2, y, x, y - 5, BRANCO);
}

void espiral(int cx, int cy, float giro) {
  int px = cx, py = cy;
  for (float a = 0; a < 4 * PI; a += 0.2) {
    float r = a * 1.2;
    int x = cx + cos(a + giro) * r;
    int y = cy + sin(a + giro) * r;
    display.drawLine(px, py, x, y, BRANCO);
    display.drawLine(px + 1, py, x + 1, y, BRANCO);
    px = x; py = y;
  }
}

void brilhinho(int x, int y, int t) {
  display.drawLine(x - t, y, x + t, y, BRANCO);
  display.drawLine(x, y - t, x, y + t, BRANCO);
  if (t > 2) {
    display.drawPixel(x - t/2, y - t/2, BRANCO); display.drawPixel(x + t/2, y - t/2, BRANCO);
    display.drawPixel(x - t/2, y + t/2, BRANCO); display.drawPixel(x + t/2, y + t/2, BRANCO);
  }
}

// =====================================================================
//  MOVIMENTOS AUTOMÁTICOS
// =====================================================================

unsigned long piscaInicio = 0, proxPisca = 1500;
float piscada(unsigned long agora) {
  if (agora >= proxPisca) {
    piscaInicio = agora;
    proxPisca = agora + random(2200, 4800);
    if (random(6) == 0) proxPisca = agora + 260;   // às vezes pisca duas vezes
  }
  unsigned long dt = agora - piscaInicio;
  if (dt >= 160) return 1.0;
  float f = (dt < 80) ? 1.0 - dt / 80.0 : (dt - 80) / 80.0;
  return f < 0.06 ? 0.06 : f;
}
void forcarPiscada() { piscaInicio = millis(); }

float olharX = 0, olharY = 0, alvoX = 0, alvoY = 0;
unsigned long proxOlhar = 0;
void atualizarOlhar(unsigned long agora, bool ativo) {
  if (!ativo) { alvoX = 0; alvoY = 0; }
  else if (agora >= proxOlhar) {
    float antesX = alvoX;
    alvoX = random(-12, 13);
    alvoY = random(-6, 7);
    if (random(3) == 0) { alvoX = 0; alvoY = 0; }
    if (fabs(alvoX - antesX) > 14) forcarPiscada();  // olhada longa: pisca no meio, igual gente
    proxOlhar = agora + random(1200, 3500);
  }
  olharX += (alvoX - olharX) * 0.18;
  olharY += (alvoY - olharY) * 0.18;
}

// =====================================================================
//  PERSONALIDADE
// =====================================================================

void setModoOlho(ModoOlho m) {
  if (m == modoOlho) return;
  modoOlho = m;
  modoOlhoInicio = millis();
  forcarPiscada();
}

void aplicarHumorBase() {
  cansadoAlvo = bravoAlvo = tristeAlvo = felizAlvo = 0;
  if (vivo != ACORDADO) { cansadoAlvo = 1; setModoOlho(MO_NORMAL); return; }
  switch (humor) {
    case 1: felizAlvo   = 1;   break;
    case 2: cansadoAlvo = 0.7; break;
    case 3: bravoAlvo   = 1;   break;
  }
  if      (humor == 4) setModoOlho(MO_CORACAO);
  else if (humor == 5) setModoOlho(MO_HACKER);
  else                 setModoOlho(MO_NORMAL);
}

void restaurarVisual() {
  suor = lagrimas = tremor = false;
  escHAlvo[0] = escHAlvo[1] = 1;
  aplicarHumorBase();
}

void iniciarReacao(Reacao r, unsigned long dur) {
  restaurarVisual();
  reacao = r;
  reacaoInicio = millis();
  reacaoAte = reacaoInicio + dur;
  setModoOlho(MO_NORMAL);   // mania de cara normal aparece mesmo com humor coração/hacker
  switch (r) {
    case R_RISADA:      felizAlvo = 1; break;
    case R_SURPRESA:    setModoOlho(MO_SURPRESO); break;
    case R_SUOR:        suor = true; break;
    case R_DESCONFIADO: escHAlvo[1] = 0.45; break;
    case R_APAIXONADO:  setModoOlho(MO_CORACAO); break;
    case R_HACKER:      setModoOlho(MO_HACKER); break;
    case R_TRISTE:      tristeAlvo = 1; lagrimas = true; break;
    case R_ACORDOU:     suor = true; break;                         // depois vira espiral
    case R_FELIZ:       felizAlvo = 1; break;
    case R_IRRITADO:    bravoAlvo = 1; tremor = true; break;
    case R_SAUDADE:     felizAlvo = 1; break;                       // depois vira coração
    case R_RANZINZA:    cansadoAlvo = 0.35; bravoAlvo = 0.8; break; // olho pesado e bravo: "me acordou pra quê?"
    case R_ZOANDO:      felizAlvo = 1; escHAlvo[0] = 0.5; break;    // ri de canto da tua cara
    case R_ORGULHO:     setModoOlho(MO_CORACAO); break;
    default: break;  // R_CONFUSO e R_PISCADINHA são só movimento
  }
}

// Coisas que mudam no meio da reação
void atualizarReacao() {
  if (reacao == R_NENHUMA) return;
  unsigned long agora = millis();
  unsigned long tr = agora - reacaoInicio;
  if (reacao == R_ACORDOU && tr > 500)  setModoOlho(MO_ESPIRAL);   // abre o olho... e fica zonzo
  if (reacao == R_SAUDADE && tr > 600)  setModoOlho(MO_CORACAO);   // abre o olho... e se derrete
  if (reacao == R_ZOANDO  && tr > 1300) { felizAlvo = 0; escHAlvo[0] = 1; escHAlvo[1] = 0.45; }  // e termina desconfiado
  if (agora >= reacaoAte) {
    reacao = R_NENHUMA;
    restaurarVisual();
    proxGesto = agora + random(GESTO_MIN, GESTO_MAX);
  }
}

// Sorteio das manias. Quanto mais vezes aparece na lista, mais sai.
const Reacao GESTOS[] = {
  R_RISADA, R_RISADA, R_RISADA, R_PISCADINHA, R_PISCADINHA, R_PISCADINHA,
  R_CONFUSO, R_CONFUSO, R_SURPRESA, R_SURPRESA, R_SUOR, R_SUOR,
  R_DESCONFIADO, R_DESCONFIADO, R_APAIXONADO, R_HACKER, R_TRISTE
};
const int NUM_GESTOS = sizeof(GESTOS) / sizeof(GESTOS[0]);
// O afeto puxa a personalidade pra um lado:
const Reacao GESTOS_FELIZ[]   = { R_RISADA, R_APAIXONADO, R_PISCADINHA };  // bem tratado
const Reacao GESTOS_CARENTE[] = { R_TRISTE, R_SUOR, R_DESCONFIADO };       // esquecido

unsigned long duracaoGesto(Reacao r) {
  switch (r) {
    case R_RISADA:      return 1100;
    case R_CONFUSO:     return 900;
    case R_PISCADINHA:  return 1100;
    case R_SURPRESA:    return 1400;
    case R_DESCONFIADO: return 1800;
    case R_HACKER:      return 2200;
    default:            return 2500;
  }
}

void fazerGestoAleatorio() {
  Reacao r = GESTOS[random(NUM_GESTOS)];
  int sorte = random(100);
  if (afeto >= 70 && sorte < 35) r = GESTOS_FELIZ[random(3)];
  if (afeto <= 30 && sorte < 35) r = GESTOS_CARENTE[random(3)];
  iniciarReacao(r, duracaoGesto(r));
}

void proximoHumor() {
  humor = (humor + 1) % NUM_HUMORES;
  aplicarHumorBase();
}

int afetoPorCliqueNaSessao = 0;   // limita quanto dá pra "farmar" afeto só clicando
void contarCutucada() {
  if (afetoPorCliqueNaSessao < 5) { mudarAfeto(1); afetoPorCliqueNaSessao++; }
  if (millis() - spamInicio > SPAM_JANELA) { spamInicio = millis(); spamCliques = 0; }
  spamCliques++;
  if (spamCliques >= SPAM_LIMITE) { spamCliques = 0; mudarAfeto(-3); iniciarReacao(R_IRRITADO, 1500); }
}

bool registrarInteracao() { // devolve true se acordou o robô
  Vivo antes = vivo;
  ultimaInteracao = millis();
  atencaoAte = millis() + TEMPO_ATENCAO;     // para tudo e olha pra você
  proxGesto = millis() + TEMPO_ATENCAO + random(GESTO_MIN, GESTO_MAX);
  if (antes == ACORDADO) return false;
  vivo = ACORDADO;
  humor = 0;
  aberturaAlvo = 1;
  if (antes == DORMINDO) iniciarReacao(R_ACORDOU, 1800);
  else restaurarVisual();
  return true;
}

// Acordou do sono profundo pelo botão: a cara depende de quanto tempo dormiu
void acordarDoSono() {
  long dormiu = segundosRelogio() - dormiuEm;
  mudarAfeto(-(int)(dormiu / 7200) * 2);   // -2 de afeto a cada 2 h esquecido
  vivo = DORMINDO;
  abertura = 0;
  registrarInteracao();                     // abre o olho (padrão: acorda zonzo)
  if (dormiu < DORMIU_POUCO)       iniciarReacao(R_RANZINZA, 2500);
  else if (dormiu > DORMIU_MUITO)  { mudarAfeto(3); iniciarReacao(R_SAUDADE, 3000); }
}

// =====================================================================
//  DESENHO DOS OLHOS
// =====================================================================

void olhoNormal(int i, int dx, int dy, float pisc, unsigned long tr) {
  float curioso = 0;
  if (i == 0 && olharX < -6) curioso = 0.15;
  if (i == 1 && olharX >  6) curioso = 0.15;

  float ab = abertura * pisc;
  if (reacao == R_PISCADINHA && i == 1) {
    if      (tr < 100) ab *= 1.0 - tr / 100.0;
    else if (tr < 900) ab = 0;
    else if (tr < 1000) ab *= (tr - 900) / 100.0;
  }

  float zoom = 1.0 + 0.08 * atencao;                 // te encarando: olho um tico maior
  int w = OLHO_W * (1.0 + 0.06 * atencao);
  int h = OLHO_H * (escH[i] + curioso) * ab * zoom;
  int cx = CXS[i] + dx, cy = CY + dy;

  if (h <= 4) {
    if (reacao == R_PISCADINHA && i == 1) {
      olhoFechadoArco(cx, cy);
      brilhinho(cx + 21, cy - 16, 2 + (tr / 80) % 4);
    } else {
      display.fillRoundRect(cx - 15, cy + 2, 30, 4, 2, BRANCO);
    }
    return;
  }

  olho(cx, cy, w, h, OLHO_R);
  int topo = cy - h/2 - 1, esq = cx - w/2 - 1, dir = cx + w/2 + 1;

  if (cansado > 0.02)
    display.fillRect(esq, topo, w + 2, (int)(h * 0.45 * cansado) + 1, PRETO);

  int pb = h * 0.55 * bravo;
  if (pb > 1) {
    if (i == 0) display.fillTriangle(esq, topo, dir, topo, dir, topo + pb, PRETO);
    else        display.fillTriangle(esq, topo, dir, topo, esq, topo + pb, PRETO);
  }
  int pt = h * 0.5 * triste;
  if (pt > 1) {
    if (i == 0) display.fillTriangle(esq, topo, dir, topo, esq, topo + pt, PRETO);
    else        display.fillTriangle(esq, topo, dir, topo, dir, topo + pt, PRETO);
  }
  if (feliz > 0.02)
    display.fillCircle(cx, cy + h * 0.59, h * 0.59 * feliz + 0.5, PRETO);
}

void desenharCoracoes(unsigned long t, int dx, int dy) {
  int r = 6 + fabs(sin(t / 220.0)) * 2.5;
  coracao(CX_E + dx, CY + dy - r * 0.7, r);
  coracao(CX_D + dx, CY + dy - r * 0.7, r);
  int subida = (t / 25) % 80;
  if (subida < 64) coracao(120, 64 - subida, 2);
}

void desenharEspirais(unsigned long t, int dx, int dy) {
  int balanco = sin(t / 280.0) * 5;
  float giro = t / 110.0;
  espiral(CX_E + dx + balanco, CY + dy,  giro);
  espiral(CX_D + dx + balanco, CY + dy, -giro);
}

void desenharSurpreso(unsigned long t, int dx, int dy) {
  float s;
  if (t < 220)      s = 0.5 + 0.7 * (t / 220.0);
  else if (t < 420) s = 1.2 - 0.2 * ((t - 220) / 200.0);
  else              s = 1.0;
  int R = 15 * s;
  int px = random(-1, 2), py = random(-1, 2);
  for (int i = 0; i < 2; i++) {
    int cx = CXS[i] + dx;
    display.fillCircle(cx, CY + dy, R, BRANCO);
    display.fillCircle(cx, CY + dy, R - 4, PRETO);
    display.fillCircle(cx + px, CY + dy + py, 5, BRANCO);
  }
}

void desenharHacker(unsigned long t, int dx, int dy) {
  bool glitch = (t % 1300) < 180;
  int w = 30, h = 26;
  for (int i = 0; i < 2; i++) {
    int cx = CXS[i] + dx, top = CY - 4 - h/2 + dy / 2;
    for (int y = top; y < top + h; y += 3) {
      int off = glitch ? random(-7, 8) : 0;
      display.fillRect(cx - w/2 + off, y, w, 3, BRANCO);
    }
    for (int y = top + 2; y < top + h; y += 3)
      display.drawFastHLine(cx - w/2 - 8, y, w + 16, PRETO);
  }
  int ruido = glitch ? 60 : 4;
  for (int i = 0; i < ruido; i++) display.drawPixel(random(LARGURA), random(52), BRANCO);
  if (glitch && random(4) == 0) display.fillRect(0, 0, LARGURA, 52, SSD1306_INVERSE);

  const char* msg = "> acesso liberado";
  int len = strlen(msg);
  int n = t / 60; if (n > len) n = len;
  display.setTextSize(1); display.setTextColor(BRANCO);
  display.setCursor(0, 56);
  for (int i = 0; i < n; i++) display.print(msg[i]);
  if ((t / 300) % 2) display.print("_");
}

void desenharSuor(unsigned long agora) {
  for (int k = 0; k < 3; k++) {
    float fase = fmod((agora + k * 500) / 1500.0, 1.0);
    lagrima(5 + k * 7, 4 + fase * 16);
  }
}

void desenharLagrimas(unsigned long agora, int dy) {
  for (int k = 0; k < 2; k++) {
    float fase = fmod((agora + k * 450) / 900.0, 1.0);
    int y = CY + dy + 17 + fase * fase * 30;
    lagrima(CX_E - 11, y);
    lagrima(CX_D + 11, y);
  }
}

void desenharZzz(unsigned long agora) {
  display.setTextColor(BRANCO);
  for (int k = 0; k < 3; k++) {
    float p = fmod((agora + k * 800) / 2400.0, 1.0);
    display.setTextSize(p < 0.45 ? 1 : 2);
    display.setCursor(104 + p * 14, 24 - p * 24);
    display.print("Z");
  }
  display.setTextSize(1);
}

void desenharOlhos() {
  unsigned long agora = millis();
  unsigned long tr = agora - reacaoInicio;
  bool atento = (agora < atencaoAte) && vivo == ACORDADO;

  // 1) parâmetros perseguem os alvos (transição suave)
  aproximar(cansado, cansadoAlvo, 0.12);
  aproximar(bravo,   bravoAlvo,   0.20);
  aproximar(triste,  tristeAlvo,  0.20);
  aproximar(feliz,   felizAlvo,   0.20);
  aproximar(abertura, aberturaAlvo, vivo == DORMINDO ? 0.08 : 0.25);
  aproximar(atencao, atento ? 1 : 0, 0.20);
  for (int i = 0; i < 2; i++) aproximar(escH[i], escHAlvo[i], 0.25);

  // brilho: vai apagando conforme o sono
  int brilhoAlvo = (vivo == ACORDADO) ? BRILHO_ACORDADO : (vivo == CANSADO ? BRILHO_CANSADO : BRILHO_DORMINDO);
  aproximar(brilho, brilhoAlvo, vivo == DORMINDO ? 0.04 : 0.06);
  enviarBrilho((int)brilho);

  // 2) movimento
  atualizarOlhar(agora, vivo == ACORDADO && !atento);
  float pisc = (vivo == DORMINDO) ? 1.0 : piscada(agora);
  int dx = olharX, dy = olharY;
  dy += lroundf(sin(agora / 650.0) * 1.4);                   // respiração
  if (vivo == ACORDADO && !atento && random(40) == 0)         // microtremidinha do olho
    dx += random(-1, 2);
  if (tremor) dx += ((agora / 40) % 2) ? 2 : -2;
  if (reacao == R_CONFUSO)
    dx += sin(tr / 45.0) * 6 * (1.0 - tr / 900.0);
  if (reacao == R_RISADA || reacao == R_FELIZ || reacao == R_ZOANDO) {
    float amort = 1.0 - tr / 1100.0;
    if (amort > 0) dy += sin(tr / 55.0) * 3 * amort;
  }

  // 3) desenha
  display.clearDisplay();
  unsigned long tm = agora - modoOlhoInicio;
  switch (modoOlho) {
    case MO_CORACAO:  desenharCoracoes(tm, dx, dy); break;
    case MO_ESPIRAL:  desenharEspirais(tm, dx, dy); break;
    case MO_SURPRESO: desenharSurpreso(tm, dx, dy); break;
    case MO_HACKER:   desenharHacker(tm, dx, dy);   break;
    default:
      for (int i = 0; i < 2; i++) olhoNormal(i, dx, dy, pisc, tr);
  }
  if (suor)     desenharSuor(agora);
  if (lagrimas) desenharLagrimas(agora, dy);
  if (vivo == DORMINDO) desenharZzz(agora);
  display.display();
}

// ===================== FIM DE JOGO (comum aos 3) =====================
void fimDeJogo(Jogo j, int pontos) {
  novoRecordeAgora = false;
  mudarAfeto(1);                                    // brincou com ele: afeto sobe
  Resultado r;
  if (pontos > recorde[j] && pontos > 0) {
    recorde[j] = pontos; novoRecordeAgora = true; r = RES_RECORDE;
  } else if (pontos <= 2 || pontos * 3 < recorde[j]) {
    r = RES_FOI_MAL;
  } else {
    r = RES_NORMAL;
  }
  if (r > resultadoPendente) resultadoPendente = r; // guarda o melhor momento da sessão
  if (r == RES_RECORDE) mudarAfeto(5);
}

void textoFimDeJogo(int y) {
  display.setCursor(novoRecordeAgora ? 22 : 34, y);
  display.print(novoRecordeAgora ? "NOVO RECORDE!" : "GAME OVER");
  display.setCursor(2, 48); display.print("toque=denovo segura=sai");
}

void placar(int pontos, Jogo j) {
  display.setTextSize(1); display.setTextColor(BRANCO);
  display.setCursor(0, 0); display.print(pontos);
  display.setCursor(80, 0); display.print("rec "); display.print(recorde[j]);
}

// ===================== DINO =====================
const int CHAO = 54, DINO_X = 16, DINO_W = 8, DINO_H = 10;
const float GRAV = 0.8, PULO = -7.2;
float dino_y, dino_velY; bool dino_noChao;
float dino_obsX; int dino_obsW, dino_obsH; float dino_vel;
int dino_pontos; bool dino_gameOver;

void dino_nasceObs() { dino_obsX = LARGURA + random(0, 50); dino_obsW = random(4, 8); dino_obsH = random(10, 17); }
void dino_reset() { dino_y = CHAO - DINO_H; dino_velY = 0; dino_noChao = true; dino_vel = 2.2; dino_pontos = 0; dino_gameOver = false; dino_nasceObs(); }
void dino_pular() { if (dino_noChao) { dino_velY = PULO; dino_noChao = false; } }
void dino_update() {
  if (dino_gameOver) return;
  dino_velY += GRAV; dino_y += dino_velY;
  if (dino_y >= CHAO - DINO_H) { dino_y = CHAO - DINO_H; dino_velY = 0; dino_noChao = true; }
  dino_obsX -= dino_vel;
  if (dino_obsX + dino_obsW < 0) { dino_pontos++; dino_vel += 0.15; if (dino_vel > 5) dino_vel = 5; dino_nasceObs(); }
  if (DINO_X < dino_obsX + dino_obsW && DINO_X + DINO_W > dino_obsX && dino_y + DINO_H > CHAO - dino_obsH) {
    dino_gameOver = true;
    fimDeJogo(J_DINO, dino_pontos);
  }
}
void dino_draw() {
  display.clearDisplay();
  display.drawLine(0, CHAO, LARGURA - 1, CHAO, BRANCO);
  display.fillRect(DINO_X, (int)dino_y, DINO_W, DINO_H, BRANCO);
  display.fillRect((int)dino_obsX, CHAO - dino_obsH, dino_obsW, dino_obsH, BRANCO);
  placar(dino_pontos, J_DINO);
  if (dino_gameOver) textoFimDeJogo(22);
  display.display();
}

// ===================== FLAPPY =====================
const int FL_X = 26, FL_TAM = 7, PIPE_W = 10;
const int GAP = 32;
const float FL_GRAV = 0.28;
const float FL_FLAP = -3.2;
float fl_y, fl_velY, fl_pipeX, fl_vel; int fl_gapY, fl_pontos; bool fl_gameOver;

void fl_novoCano() { fl_pipeX = LARGURA; fl_gapY = random(6, ALTURA - GAP - 4); }
void fl_reset() { fl_y = ALTURA / 2; fl_velY = 0; fl_vel = 1.8; fl_pontos = 0; fl_gameOver = false; fl_novoCano(); }
void fl_flap() { if (!fl_gameOver) fl_velY = FL_FLAP; }
void fl_update() {
  if (fl_gameOver) return;
  fl_velY += FL_GRAV; fl_y += fl_velY; fl_pipeX -= fl_vel;
  if (fl_pipeX + PIPE_W < 0) { fl_pontos++; fl_vel += 0.1; if (fl_vel > 3.5) fl_vel = 3.5; fl_novoCano(); }
  bool bateu = (fl_y < 0 || fl_y > ALTURA - FL_TAM);
  if (FL_X + FL_TAM > fl_pipeX && FL_X < fl_pipeX + PIPE_W)
    if (fl_y < fl_gapY || fl_y + FL_TAM > fl_gapY + GAP) bateu = true;
  if (bateu) { fl_gameOver = true; fimDeJogo(J_FLAPPY, fl_pontos); }
}
void fl_draw() {
  display.clearDisplay();
  display.fillRect((int)fl_pipeX, 0, PIPE_W, fl_gapY, BRANCO);
  display.fillRect((int)fl_pipeX, fl_gapY + GAP, PIPE_W, ALTURA - (fl_gapY + GAP), BRANCO);
  display.fillRect(FL_X, (int)fl_y, FL_TAM, FL_TAM, BRANCO);
  placar(fl_pontos, J_FLAPPY);
  if (fl_gameOver) {
    display.fillRect(0, 20, LARGURA, 11, PRETO);    // fundo pro texto não sumir no cano
    display.fillRect(0, 46, LARGURA, 11, PRETO);
    textoFimDeJogo(22);
  }
  display.display();
}

// ===================== TIRO CERTO =====================
const int TR_X = 4, TR_W = 120;
const int TR_Y = 28, TR_H = 12;
const int ZONA_INICIAL = 26, ZONA_MIN = 6;
const float TIRO_VEL_INICIAL = 1.6, TIRO_VEL_MAX = 6.0;

float tiro_cursor, tiro_vel; int tiro_dir;
int tiro_zonaX, tiro_zonaW;
int tiro_pontos;
bool tiro_gameOver;
unsigned long tiro_flashAte = 0;

void tiro_novaZona() { tiro_zonaX = TR_X + random(0, TR_W - tiro_zonaW); }
void tiro_reset() {
  tiro_cursor = TR_X; tiro_dir = 1; tiro_vel = TIRO_VEL_INICIAL;
  tiro_zonaW = ZONA_INICIAL; tiro_pontos = 0; tiro_gameOver = false;
  tiro_flashAte = 0;
  tiro_novaZona();
}
void tiro_tap() {
  if (tiro_gameOver) return;
  int c = (int)tiro_cursor;
  if (c >= tiro_zonaX && c <= tiro_zonaX + tiro_zonaW) {
    tiro_pontos++;
    tiro_zonaW -= 2; if (tiro_zonaW < ZONA_MIN) tiro_zonaW = ZONA_MIN;
    tiro_vel += 0.25; if (tiro_vel > TIRO_VEL_MAX) tiro_vel = TIRO_VEL_MAX;
    tiro_novaZona();
    tiro_flashAte = millis() + 300;
  } else {
    tiro_gameOver = true;
    fimDeJogo(J_TIRO, tiro_pontos);
  }
}
void tiro_update() {
  if (tiro_gameOver) return;
  tiro_cursor += tiro_vel * tiro_dir;
  if (tiro_cursor >= TR_X + TR_W - 1) { tiro_cursor = TR_X + TR_W - 1; tiro_dir = -1; }
  if (tiro_cursor <= TR_X)            { tiro_cursor = TR_X;            tiro_dir = 1; }
}
void tiro_draw() {
  display.clearDisplay();
  placar(tiro_pontos, J_TIRO);
  display.drawRect(TR_X, TR_Y, TR_W, TR_H, BRANCO);
  display.fillRect(tiro_zonaX, TR_Y + 2, tiro_zonaW, TR_H - 4, BRANCO);
  display.fillRect((int)tiro_cursor - 1, TR_Y - 6, 3, TR_H + 12, SSD1306_INVERSE);
  if (millis() < tiro_flashAte) { display.setCursor(52, 48); display.print("BOA!"); }
  if (tiro_gameOver) {
    display.fillRect(0, 10, LARGURA, 12, PRETO);
    display.fillRect(0, 46, LARGURA, 11, PRETO);
    textoFimDeJogo(12);
  }
  display.display();
}

// ===================== COUNTDOWN =====================
void iniciarContagem() {
  emContagem = true;
  contagemInicio = millis();
  longFired = true;
}
void desenharContagem() {
  int passo = (millis() - contagemInicio) / PASSO_CONTAGEM;
  display.clearDisplay();
  display.setTextColor(BRANCO);
  if (passo >= 3) { display.setTextSize(2); display.setCursor(40, 24); display.print("VAI!"); }
  else            { display.setTextSize(3); display.setCursor(55, 20); display.print(3 - passo); }
  display.display();
}

// ===================== STATUS =====================
void desenharStatus() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(BRANCO);
  display.setCursor(40, 0); display.print("STATUS");
  display.setCursor(0, 14); display.print("afeto");
  display.drawRect(36, 14, 70, 8, BRANCO);
  display.fillRect(38, 16, afeto * 66 / 100, 4, BRANCO);
  display.setCursor(110, 14); display.print(afeto);
  display.setCursor(0, 28); display.print("rec dino   "); display.print(recorde[J_DINO]);
  display.setCursor(0, 38); display.print("rec flappy "); display.print(recorde[J_FLAPPY]);
  display.setCursor(0, 48); display.print("rec tiro   "); display.print(recorde[J_TIRO]);
  display.setCursor(86, 56); display.print("toque<-");
  display.display();
}

// ===================== SONO PROFUNDO =====================
void adormecer() {
  modo = OLHOS; emContagem = false; reacao = R_NENHUMA;
  vivo = CANSADO; restaurarVisual();
  vivo = DORMINDO;
  aberturaAlvo = 0;
  dormirAte = millis() + TEMPO_FECHAR_OLHOS;
}

void desligarDeVez() {
  dormiuEm = segundosRelogio();          // pra saber quanto tempo dormiu quando acordar
  display.clearDisplay();
  display.display();
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  esp_deep_sleep_enable_gpio_wakeup(1ULL << PINO_BOTAO, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}

// ===================== TRANSIÇÕES =====================
void desenharMenu() {
  display.clearDisplay(); display.setTextColor(BRANCO); display.setTextSize(1);
  display.setCursor(46, 0); display.print("MENU");
  for (int i = 0; i < NUM_ITENS; i++) {
    display.setCursor(20, 12 + i * 10);
    display.print(i == menuSel ? "> " : "  ");
    display.print(ITENS_MENU[i]);
  }
  display.display();
}
void abrirMenu() {
  modo = MENU; menuSel = 0; longFired = false; consumirHold = (botaoEstavel == LOW); emContagem = false;
  reacao = R_NENHUMA; restaurarVisual();
  brilhoCheio();
  desenharMenu();
}
void voltarOlhos() {
  modo = OLHOS; vivo = ACORDADO; humor = 0; consumirHold = (botaoEstavel == LOW);
  ultimaInteracao = millis();
  proxGesto = millis() + random(GESTO_MIN, GESTO_MAX);
  aberturaAlvo = 1;
  // a cara depende de como você foi nos jogos
  switch (resultadoPendente) {
    case RES_RECORDE: iniciarReacao(R_ORGULHO, 3000); break;   // orgulhoso de você
    case RES_FOI_MAL: iniciarReacao(R_ZOANDO, 2600);  break;   // ri da tua cara
    default:          iniciarReacao(R_FELIZ, 2500);   break;
  }
  resultadoPendente = RES_NADA;
}
void iniciarJogo(Jogo j) {
  modo = JOGO; jogoAtual = j; consumirHold = (botaoEstavel == LOW); ultimoFrame = millis();
  novoRecordeAgora = false;
  if (j == J_DINO) dino_reset(); else if (j == J_FLAPPY) fl_reset(); else tiro_reset();
  iniciarContagem();
}
void abrirStatus() { modo = TELA_STATUS; consumirHold = (botaoEstavel == LOW); desenharStatus(); }
void selecionarMenu() {
  if (menuSel == 0) iniciarJogo(J_DINO);
  else if (menuSel == 1) iniciarJogo(J_FLAPPY);
  else if (menuSel == 2) iniciarJogo(J_TIRO);
  else if (menuSel == 3) abrirStatus();
  else voltarOlhos();
}
bool jogoAcabou() {
  if (jogoAtual == J_DINO)   return dino_gameOver;
  if (jogoAtual == J_FLAPPY) return fl_gameOver;
  return tiro_gameOver;
}
void reiniciarJogo() {
  if (jogoAtual == J_DINO) dino_reset(); else if (jogoAtual == J_FLAPPY) fl_reset(); else tiro_reset();
  novoRecordeAgora = false;
  iniciarContagem();
}
void acaoJogo() {   // toque durante a partida
  if (jogoAtual == J_DINO) dino_pular();
  else if (jogoAtual == J_FLAPPY) fl_flap();
  else tiro_tap();
}

// ===================== SETUP / LOOP =====================
void setup() {
  bool acordouPeloBotao = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO);

  WiFi.mode(WIFI_OFF);
  Wire.begin(PINO_SDA, PINO_SCL);
  Wire.setClock(400000);                   // I2C rápido: animação mais lisa
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C); // tela preta? tenta 0x3D
  display.setTextWrap(false);
  pinMode(PINO_BOTAO, INPUT_PULLUP);
  randomSeed(esp_random());                // aleatório de hardware: cada acordada é diferente
  brilhoCheio();

  botaoBruto = botaoEstavel = botaoAnterior = digitalRead(PINO_BOTAO);
  consumirHold = (botaoAnterior == LOW);   // o aperto que acordou não conta como clique
  ultimaInteracao = millis();

  if (acordouPeloBotao) acordarDoSono();
  else                  adormecer();       // ligou na chave: fecha o olho e vai dormir
}

void loop() {
  bool botaoAgora = lerBotao();
  bool descida = (botaoAnterior == HIGH && botaoAgora == LOW);
  bool subida  = (botaoAnterior == LOW && botaoAgora == HIGH);
  bool podeInput = !consumirHold;
  if (consumirHold && subida) consumirHold = false;   // essa soltada é engolida, não vira clique
  if (descida) ultimaInteracao = millis();

  if (modo != OLHOS && millis() - ultimaInteracao > TEMPO_DESLIGAR) adormecer();

  if (modo == OLHOS) {
    if (podeInput && descida) {
      bool woke = registrarInteracao();
      if (woke) { ultimoClique = 0; }
      else if (millis() - ultimoClique <= JANELA_DUPLO) { cliquePendente = false; abrirMenu(); }
      else { cliquePendente = true; tempoClique = millis(); ultimoClique = millis(); }
      if (woke == false && !cliquePendente) ultimoClique = millis();
    }
    if (podeInput && cliquePendente && millis() - tempoClique > JANELA_DUPLO) {
      cliquePendente = false;
      proximoHumor();
      contarCutucada();
    }

    atualizarReacao();

    unsigned long ocioso = millis() - ultimaInteracao;
    if (vivo == ACORDADO) {
      if (ocioso > TEMPO_CANSADO) {
        vivo = CANSADO; reacao = R_NENHUMA; restaurarVisual();
      } else if (reacao == R_NENHUMA && millis() >= proxGesto) {
        fazerGestoAleatorio();
      }
    } else if (vivo == CANSADO && ocioso > TEMPO_DESLIGAR) {
      adormecer();
    } else if (vivo == DORMINDO && millis() >= dormirAte) {
      desligarDeVez();
    }

    if (modo == OLHOS && millis() - ultimoFrameOlhos >= FRAME_OLHOS) {
      ultimoFrameOlhos = millis();
      desenharOlhos();
    }

  } else if (modo == MENU) {
    if (podeInput) {
      if (descida) { inicioPress = millis(); longFired = false; }
      if (botaoAgora == LOW && !longFired && millis() - inicioPress >= TEMPO_LONGO) { longFired = true; selecionarMenu(); }
      if (subida && !longFired) { menuSel = (menuSel + 1) % NUM_ITENS; desenharMenu(); }
    }

  } else if (modo == TELA_STATUS) {
    if (podeInput && subida) abrirMenu();   // qualquer toque volta pro menu

  } else { // JOGO
    if (emContagem) {
      if (millis() - contagemInicio >= PASSO_CONTAGEM * 4) {
        emContagem = false;
        ultimoFrame = millis();
      } else {
        desenharContagem();
        botaoAnterior = botaoAgora;
        return;
      }
    }

    // Jogando: toque = ação na hora (pulo não pode atrasar). Segurar = sai.
    // Tela de fim: soltar = joga de novo, segurar = sai pro menu.
    if (podeInput) {
      if (descida) {
        inicioPress = millis(); longFired = false;
        apertouNoFim = jogoAcabou();
        if (!apertouNoFim) acaoJogo();
      }
      if (botaoAgora == LOW && !longFired && millis() - inicioPress >= TEMPO_LONGO) { longFired = true; abrirMenu(); }
      if (subida && !longFired && apertouNoFim) { apertouNoFim = false; reiniciarJogo(); }
    }
    if (modo == JOGO && !emContagem && millis() - ultimoFrame >= FRAME_MS) {
      ultimoFrame = millis();
      if (jogoAtual == J_DINO)        { dino_update(); dino_draw(); }
      else if (jogoAtual == J_FLAPPY) { fl_update();   fl_draw(); }
      else                            { tiro_update(); tiro_draw(); }
    }
  }

  botaoAnterior = botaoAgora;
}
